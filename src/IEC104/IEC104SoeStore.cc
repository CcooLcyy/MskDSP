#include "IEC104SoeStore.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sqlite3.h>

#include "Logger.h"

namespace IEC104 {
namespace {

constexpr size_t kMarkAcknowledgedBatchSize = 500;

std::string sqliteError(sqlite3* db) {
  if (db == nullptr) {
    return "数据库句柄为空";
  }
  const char* message = sqlite3_errmsg(db);
  return message == nullptr ? "未知 SQLite 错误" : message;
}

grpc::Status internalError(sqlite3* db, std::string_view prefix) {
  return grpc::Status(grpc::StatusCode::INTERNAL, std::string(prefix) + ": " + sqliteError(db));
}

class SqliteDb {
public:
  SqliteDb() = default;
  SqliteDb(const SqliteDb&) = delete;
  SqliteDb& operator=(const SqliteDb&) = delete;

  ~SqliteDb() {
    if (db_ != nullptr) {
      sqlite3_close(db_);
    }
  }

  grpc::Status Open(const std::filesystem::path& path) {
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent, ec);
      if (ec) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "创建 SOE 数据库目录失败: " + ec.message());
      }
    }

    const int rc = sqlite3_open_v2(path.string().c_str(),
                                   &db_,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                   nullptr);
    if (rc != SQLITE_OK) {
      return internalError(db_, "打开 SOE 数据库失败");
    }

    auto status = Exec("PRAGMA busy_timeout=5000");
    if (!status.ok()) {
      return status;
    }
    return Exec("PRAGMA foreign_keys=ON");
  }

  grpc::Status Exec(std::string_view sql) {
    char* error = nullptr;
    const std::string sqlText(sql);
    const int rc = sqlite3_exec(db_, sqlText.c_str(), nullptr, nullptr, &error);
    if (rc == SQLITE_OK) {
      return grpc::Status::OK;
    }

    std::string message = error == nullptr ? sqliteError(db_) : error;
    sqlite3_free(error);
    return grpc::Status(grpc::StatusCode::INTERNAL, message);
  }

  sqlite3* get() const { return db_; }

private:
  sqlite3* db_{nullptr};
};

class Statement {
public:
  Statement(sqlite3* db, std::string_view sql) : db_(db) {
    const std::string sqlText(sql);
    const int rc = sqlite3_prepare_v2(db_, sqlText.c_str(), -1, &statement_, nullptr);
    if (rc != SQLITE_OK) {
      status_ = internalError(db_, "准备 SOE SQL 失败");
    }
  }

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  ~Statement() {
    if (statement_ != nullptr) {
      sqlite3_finalize(statement_);
    }
  }

  const grpc::Status& status() const { return status_; }
  sqlite3_stmt* get() const { return statement_; }

private:
  sqlite3* db_{nullptr};
  sqlite3_stmt* statement_{nullptr};
  grpc::Status status_{grpc::Status::OK};
};

class Transaction {
public:
  explicit Transaction(SqliteDb* db) : db_(db) {}
  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;

  ~Transaction() {
    if (active_) {
      db_->Exec("ROLLBACK");
    }
  }

  grpc::Status Begin() {
    auto status = db_->Exec("BEGIN IMMEDIATE");
    active_ = status.ok();
    return status;
  }

  grpc::Status Commit() {
    auto status = db_->Exec("COMMIT");
    if (status.ok()) {
      active_ = false;
    }
    return status;
  }

private:
  SqliteDb* db_;
  bool active_{false};
};

grpc::Status bindText(sqlite3* db, sqlite3_stmt* statement, int index, std::string_view value) {
  const int rc = sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
  return rc == SQLITE_OK ? grpc::Status::OK : internalError(db, "绑定 SOE 文本参数失败");
}

grpc::Status bindInt64(sqlite3* db, sqlite3_stmt* statement, int index, int64_t value) {
  const int rc = sqlite3_bind_int64(statement, index, value);
  return rc == SQLITE_OK ? grpc::Status::OK : internalError(db, "绑定 SOE 整数参数失败");
}

grpc::Status validateConnectionName(std::string_view connName) {
  if (connName.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "SOE 连接名称不能为空");
  }
  return grpc::Status::OK;
}

grpc::Status ensureSchema(SqliteDb* db) {
  auto status = db->Exec(R"sql(
CREATE TABLE IF NOT EXISTS iec104_soe_connections (
  conn_name TEXT NOT NULL PRIMARY KEY,
  next_sequence INTEGER NOT NULL CHECK(next_sequence >= 1)
)
)sql");
  if (!status.ok()) {
    return status;
  }

  status = db->Exec(R"sql(
CREATE TABLE IF NOT EXISTS iec104_soe_events (
  conn_name TEXT NOT NULL,
  event_sequence INTEGER NOT NULL CHECK(event_sequence >= 1),
  ioa INTEGER NOT NULL CHECK(ioa >= 0 AND ioa <= 16777215),
  bool_value INTEGER NOT NULL CHECK(bool_value IN (0, 1)),
  ts_ms INTEGER NOT NULL,
  quality INTEGER NOT NULL CHECK(quality >= 0 AND quality <= 255),
  acknowledged INTEGER NOT NULL DEFAULT 0 CHECK(acknowledged IN (0, 1)),
  PRIMARY KEY(conn_name, event_sequence),
  FOREIGN KEY(conn_name) REFERENCES iec104_soe_connections(conn_name)
    ON UPDATE CASCADE ON DELETE CASCADE
)
)sql");
  if (!status.ok()) {
    return status;
  }

  return db->Exec(R"sql(
CREATE INDEX IF NOT EXISTS idx_iec104_soe_events_unacknowledged
ON iec104_soe_events(conn_name, acknowledged, event_sequence)
)sql");
}

grpc::Status openStore(const std::filesystem::path& path, SqliteDb* db) {
  auto status = db->Open(path);
  if (!status.ok()) {
    return status;
  }
  status = ensureSchema(db);
  if (!status.ok()) {
    return grpc::Status(grpc::StatusCode::INTERNAL, "初始化 SOE 数据库表失败: " + status.error_message());
  }
  return grpc::Status::OK;
}

grpc::Status stepDone(sqlite3* db, sqlite3_stmt* statement, std::string_view prefix) {
  if (sqlite3_step(statement) != SQLITE_DONE) {
    return internalError(db, prefix);
  }
  return grpc::Status::OK;
}

grpc::Status readNextSequence(sqlite3* db, std::string_view connName, uint64_t* sequence) {
  Statement statement(db, "SELECT next_sequence FROM iec104_soe_connections WHERE conn_name=?");
  if (!statement.status().ok()) {
    return statement.status();
  }
  auto status = bindText(db, statement.get(), 1, connName);
  if (!status.ok()) {
    return status;
  }
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    return internalError(db, "读取 SOE 事件序号失败");
  }

  const sqlite3_int64 value = sqlite3_column_int64(statement.get(), 0);
  if (value < 1) {
    return grpc::Status(grpc::StatusCode::INTERNAL, "SOE 事件序号状态非法");
  }
  *sequence = static_cast<uint64_t>(value);
  return grpc::Status::OK;
}

grpc::Status countRecords(sqlite3* db, std::string_view connName, size_t* count) {
  Statement statement(db, "SELECT COUNT(*) FROM iec104_soe_events WHERE conn_name=?");
  if (!statement.status().ok()) {
    return statement.status();
  }
  auto status = bindText(db, statement.get(), 1, connName);
  if (!status.ok()) {
    return status;
  }
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    return internalError(db, "统计 SOE 记录数失败");
  }
  *count = static_cast<size_t>(sqlite3_column_int64(statement.get(), 0));
  return grpc::Status::OK;
}

grpc::Status readOverwriteInfo(sqlite3* db,
                               std::string_view connName,
                               size_t count,
                               uint64_t* oldestSequence,
                               size_t* unacknowledgedCount) {
  Statement statement(db,
                      "SELECT MIN(event_sequence), "
                      "COALESCE(SUM(CASE WHEN acknowledged=0 THEN 1 ELSE 0 END), 0) "
                      "FROM (SELECT event_sequence, acknowledged FROM iec104_soe_events "
                      "WHERE conn_name=? ORDER BY event_sequence LIMIT ?)");
  if (!statement.status().ok()) {
    return statement.status();
  }
  auto status = bindText(db, statement.get(), 1, connName);
  if (!status.ok()) {
    return status;
  }
  status = bindInt64(db, statement.get(), 2, static_cast<int64_t>(count));
  if (!status.ok()) {
    return status;
  }
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    return grpc::Status(grpc::StatusCode::INTERNAL, "读取 SOE 覆盖统计失败");
  }
  if (sqlite3_column_type(statement.get(), 0) == SQLITE_NULL) {
    return grpc::Status(grpc::StatusCode::INTERNAL, "SOE 覆盖统计结果为空");
  }
  *oldestSequence = static_cast<uint64_t>(sqlite3_column_int64(statement.get(), 0));
  *unacknowledgedCount = static_cast<size_t>(sqlite3_column_int64(statement.get(), 1));
  return grpc::Status::OK;
}

grpc::Status deleteOldestRecords(sqlite3* db, std::string_view connName, size_t count) {
  Statement statement(db,
                      "DELETE FROM iec104_soe_events WHERE rowid IN ("
                      "SELECT rowid FROM iec104_soe_events WHERE conn_name=? "
                      "ORDER BY event_sequence LIMIT ?)");
  if (!statement.status().ok()) {
    return statement.status();
  }
  auto status = bindText(db, statement.get(), 1, connName);
  if (!status.ok()) {
    return status;
  }
  status = bindInt64(db, statement.get(), 2, static_cast<int64_t>(count));
  if (!status.ok()) {
    return status;
  }
  return stepDone(db, statement.get(), "覆盖最旧 SOE 事件失败");
}

grpc::Status loadRecords(sqlite3* db,
                         std::string_view connName,
                         bool onlyUnacknowledged,
                         std::vector<SoeRecord>* records) {
  std::string sql = "SELECT event_sequence, ioa, bool_value, ts_ms, quality, acknowledged "
                    "FROM iec104_soe_events WHERE conn_name=?";
  if (onlyUnacknowledged) {
    sql += " AND acknowledged=0";
  }
  sql += " ORDER BY event_sequence";

  Statement statement(db, sql);
  if (!statement.status().ok()) {
    return statement.status();
  }
  auto status = bindText(db, statement.get(), 1, connName);
  if (!status.ok()) {
    return status;
  }

  std::vector<SoeRecord> loaded;
  int rc = SQLITE_ROW;
  while ((rc = sqlite3_step(statement.get())) == SQLITE_ROW) {
    loaded.push_back(SoeRecord{static_cast<uint64_t>(sqlite3_column_int64(statement.get(), 0)),
                               std::string(connName),
                               static_cast<uint32_t>(sqlite3_column_int64(statement.get(), 1)),
                               sqlite3_column_int(statement.get(), 2) != 0,
                               sqlite3_column_int64(statement.get(), 3),
                               static_cast<uint8_t>(sqlite3_column_int(statement.get(), 4)),
                               sqlite3_column_int(statement.get(), 5) != 0});
  }
  if (rc != SQLITE_DONE) {
    return internalError(db, onlyUnacknowledged ? "查询未确认 SOE 事件失败" : "查询 SOE 历史事件失败");
  }

  *records = std::move(loaded);
  return grpc::Status::OK;
}

}  // 匿名命名空间结束

IEC104SoeStore::IEC104SoeStore(std::filesystem::path databasePath) :
  databasePath_(std::move(databasePath)) {}

grpc::Status IEC104SoeStore::Append(std::string_view connName,
                                    uint32_t ioa,
                                    bool state,
                                    int64_t tsMs,
                                    uint8_t quality,
                                    SoeAppendResult* result) {
  auto status = validateConnectionName(connName);
  if (!status.ok()) {
    return status;
  }
  if (ioa > 0xFFFFFFu) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "SOE 信息对象地址超出 24 位范围");
  }

  SqliteDb db;
  status = openStore(databasePath_, &db);
  if (!status.ok()) {
    LOG_ERROR("IEC104 SOE 存储打开失败: conn_name={}, 错误={}", connName, status.error_message());
    return status;
  }

  Transaction transaction(&db);
  status = transaction.Begin();
  if (!status.ok()) {
    return grpc::Status(grpc::StatusCode::INTERNAL, "启动 SOE 写入事务失败: " + status.error_message());
  }

  Statement ensureConnection(db.get(),
                             "INSERT OR IGNORE INTO iec104_soe_connections(conn_name, next_sequence) VALUES(?, 1)");
  if (!ensureConnection.status().ok()) {
    return ensureConnection.status();
  }
  status = bindText(db.get(), ensureConnection.get(), 1, connName);
  if (!status.ok()) {
    return status;
  }
  status = stepDone(db.get(), ensureConnection.get(), "初始化 SOE 连接序号失败");
  if (!status.ok()) {
    return status;
  }

  uint64_t sequence = 0;
  status = readNextSequence(db.get(), connName, &sequence);
  if (!status.ok()) {
    return status;
  }
  if (sequence >= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "SOE 事件序号已耗尽");
  }

  Statement updateSequence(db.get(),
                           "UPDATE iec104_soe_connections SET next_sequence=? WHERE conn_name=?");
  if (!updateSequence.status().ok()) {
    return updateSequence.status();
  }
  status = bindInt64(db.get(), updateSequence.get(), 1, static_cast<int64_t>(sequence + 1));
  if (status.ok()) {
    status = bindText(db.get(), updateSequence.get(), 2, connName);
  }
  if (status.ok()) {
    status = stepDone(db.get(), updateSequence.get(), "更新 SOE 事件序号失败");
  }
  if (!status.ok()) {
    return status;
  }

  Statement insertEvent(db.get(),
                        "INSERT INTO iec104_soe_events("
                        "conn_name, event_sequence, ioa, bool_value, ts_ms, quality, acknowledged) "
                        "VALUES(?, ?, ?, ?, ?, ?, 0)");
  if (!insertEvent.status().ok()) {
    return insertEvent.status();
  }
  status = bindText(db.get(), insertEvent.get(), 1, connName);
  if (status.ok()) {
    status = bindInt64(db.get(), insertEvent.get(), 2, static_cast<int64_t>(sequence));
  }
  if (status.ok()) {
    status = bindInt64(db.get(), insertEvent.get(), 3, ioa);
  }
  if (status.ok()) {
    status = bindInt64(db.get(), insertEvent.get(), 4, state ? 1 : 0);
  }
  if (status.ok()) {
    status = bindInt64(db.get(), insertEvent.get(), 5, tsMs);
  }
  if (status.ok()) {
    status = bindInt64(db.get(), insertEvent.get(), 6, quality);
  }
  if (status.ok()) {
    status = stepDone(db.get(), insertEvent.get(), "写入 SOE 事件失败");
  }
  if (!status.ok()) {
    return status;
  }

  size_t recordCount = 0;
  status = countRecords(db.get(), connName, &recordCount);
  if (!status.ok()) {
    return status;
  }

  const size_t overwrittenCount = recordCount > kCapacityPerConnection
                                      ? recordCount - kCapacityPerConnection
                                      : 0;
  std::optional<uint64_t> oldestOverwrittenSequence;
  size_t overwrittenUnacknowledgedCount = 0;
  if (overwrittenCount != 0) {
    uint64_t oldest = 0;
    status = readOverwriteInfo(db.get(),
                               connName,
                               overwrittenCount,
                               &oldest,
                               &overwrittenUnacknowledgedCount);
    if (!status.ok()) {
      return status;
    }
    oldestOverwrittenSequence = oldest;
    status = deleteOldestRecords(db.get(), connName, overwrittenCount);
    if (!status.ok()) {
      return status;
    }
  }

  status = transaction.Commit();
  if (!status.ok()) {
    return grpc::Status(grpc::StatusCode::INTERNAL, "提交 SOE 写入事务失败: " + status.error_message());
  }

  if (result != nullptr) {
    result->record = SoeRecord{sequence,
                               std::string(connName),
                               ioa,
                               state,
                               tsMs,
                               quality,
                               false};
    result->overflowed = overwrittenCount != 0;
    result->overwrittenCount = overwrittenCount;
    result->overwrittenUnacknowledgedCount = overwrittenUnacknowledgedCount;
    result->oldestOverwrittenSequence = oldestOverwrittenSequence;
  }

  if (overwrittenCount != 0) {
    LOG_WARNING("IEC104 SOE 缓存已满，已覆盖最旧记录: conn_name={}, 容量={}, 覆盖条数={}, 未确认覆盖条数={}, 最旧事件序号={}",
                connName,
                kCapacityPerConnection,
                overwrittenCount,
                overwrittenUnacknowledgedCount,
                oldestOverwrittenSequence.value_or(0));
  }
  return grpc::Status::OK;
}

grpc::Status IEC104SoeStore::LoadUnacknowledged(std::string_view connName,
                                                std::vector<SoeRecord>* records) const {
  if (records == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "SOE 查询结果不能为空");
  }
  auto status = validateConnectionName(connName);
  if (!status.ok()) {
    return status;
  }

  SqliteDb db;
  status = openStore(databasePath_, &db);
  if (!status.ok()) {
    LOG_ERROR("IEC104 SOE 存储打开失败: conn_name={}, 错误={}", connName, status.error_message());
    return status;
  }

  return loadRecords(db.get(), connName, true, records);
}

grpc::Status IEC104SoeStore::LoadRecent(std::string_view connName,
                                        std::vector<SoeRecord>* records) const {
  if (records == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "SOE 查询结果不能为空");
  }
  auto status = validateConnectionName(connName);
  if (!status.ok()) {
    return status;
  }

  SqliteDb db;
  status = openStore(databasePath_, &db);
  if (!status.ok()) {
    LOG_ERROR("IEC104 SOE 存储打开失败: conn_name={}, 错误={}", connName, status.error_message());
    return status;
  }
  return loadRecords(db.get(), connName, false, records);
}

grpc::Status IEC104SoeStore::MarkAcknowledged(std::string_view connName,
                                              std::span<const uint64_t> sequences,
                                              size_t* updatedCount) {
  auto status = validateConnectionName(connName);
  if (!status.ok()) {
    return status;
  }
  if (updatedCount != nullptr) {
    *updatedCount = 0;
  }
  if (sequences.empty()) {
    return grpc::Status::OK;
  }

  SqliteDb db;
  status = openStore(databasePath_, &db);
  if (!status.ok()) {
    LOG_ERROR("IEC104 SOE 存储打开失败: conn_name={}, 错误={}", connName, status.error_message());
    return status;
  }

  Transaction transaction(&db);
  status = transaction.Begin();
  if (!status.ok()) {
    return grpc::Status(grpc::StatusCode::INTERNAL, "启动 SOE 确认事务失败: " + status.error_message());
  }

  size_t totalUpdated = 0;
  for (size_t offset = 0; offset < sequences.size(); offset += kMarkAcknowledgedBatchSize) {
    const size_t batchSize = std::min(kMarkAcknowledgedBatchSize, sequences.size() - offset);
    std::string sql = "UPDATE iec104_soe_events SET acknowledged=1 "
                      "WHERE conn_name=? AND acknowledged=0 AND event_sequence IN (";
    for (size_t i = 0; i < batchSize; ++i) {
      if (i != 0) {
        sql += ',';
      }
      sql += '?';
    }
    sql += ')';

    Statement statement(db.get(), sql);
    if (!statement.status().ok()) {
      return statement.status();
    }
    status = bindText(db.get(), statement.get(), 1, connName);
    for (size_t i = 0; status.ok() && i < batchSize; ++i) {
      if (sequences[offset + i] > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "SOE 事件序号超出 SQLite 整数范围");
      }
      status = bindInt64(db.get(),
                         statement.get(),
                         static_cast<int>(i + 2),
                         static_cast<int64_t>(sequences[offset + i]));
    }
    if (!status.ok()) {
      return status;
    }
    status = stepDone(db.get(), statement.get(), "更新 SOE 确认状态失败");
    if (!status.ok()) {
      return status;
    }
    totalUpdated += static_cast<size_t>(sqlite3_changes(db.get()));
  }

  status = transaction.Commit();
  if (!status.ok()) {
    return grpc::Status(grpc::StatusCode::INTERNAL, "提交 SOE 确认事务失败: " + status.error_message());
  }
  if (updatedCount != nullptr) {
    *updatedCount = totalUpdated;
  }
  LOG_INFO("IEC104 SOE 确认状态已持久化: conn_name={}, 请求条数={}, 更新条数={}",
           connName,
           sequences.size(),
           totalUpdated);
  return grpc::Status::OK;
}

grpc::Status IEC104SoeStore::RenameConnection(std::string_view oldConnName,
                                              std::string_view newConnName) {
  auto status = validateConnectionName(oldConnName);
  if (!status.ok()) {
    return status;
  }
  status = validateConnectionName(newConnName);
  if (!status.ok()) {
    return status;
  }
  if (oldConnName == newConnName) {
    return grpc::Status::OK;
  }

  SqliteDb db;
  status = openStore(databasePath_, &db);
  if (!status.ok()) {
    LOG_ERROR("IEC104 SOE 存储打开失败: conn_name={}, 错误={}", oldConnName, status.error_message());
    return status;
  }

  Transaction transaction(&db);
  status = transaction.Begin();
  if (!status.ok()) {
    return grpc::Status(grpc::StatusCode::INTERNAL, "启动 SOE 连接改名事务失败: " + status.error_message());
  }

  Statement sourceExists(db.get(), "SELECT 1 FROM iec104_soe_connections WHERE conn_name=?");
  if (!sourceExists.status().ok()) {
    return sourceExists.status();
  }
  status = bindText(db.get(), sourceExists.get(), 1, oldConnName);
  if (!status.ok()) {
    return status;
  }
  const int sourceResult = sqlite3_step(sourceExists.get());
  if (sourceResult == SQLITE_DONE) {
    status = transaction.Commit();
    return status.ok() ? grpc::Status::OK
                       : grpc::Status(grpc::StatusCode::INTERNAL,
                                      "提交 SOE 连接改名事务失败: " + status.error_message());
  }
  if (sourceResult != SQLITE_ROW) {
    return internalError(db.get(), "查询 SOE 原连接失败");
  }

  Statement targetExists(db.get(), "SELECT 1 FROM iec104_soe_connections WHERE conn_name=?");
  if (!targetExists.status().ok()) {
    return targetExists.status();
  }
  status = bindText(db.get(), targetExists.get(), 1, newConnName);
  if (!status.ok()) {
    return status;
  }
  const int targetResult = sqlite3_step(targetExists.get());
  if (targetResult == SQLITE_ROW) {
    return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "SOE 目标连接名称已存在");
  }
  if (targetResult != SQLITE_DONE) {
    return internalError(db.get(), "查询 SOE 目标连接失败");
  }

  Statement rename(db.get(), "UPDATE iec104_soe_connections SET conn_name=? WHERE conn_name=?");
  if (!rename.status().ok()) {
    return rename.status();
  }
  status = bindText(db.get(), rename.get(), 1, newConnName);
  if (status.ok()) {
    status = bindText(db.get(), rename.get(), 2, oldConnName);
  }
  if (status.ok()) {
    status = stepDone(db.get(), rename.get(), "更新 SOE 连接名称失败");
  }
  if (!status.ok()) {
    return status;
  }

  status = transaction.Commit();
  if (!status.ok()) {
    return grpc::Status(grpc::StatusCode::INTERNAL, "提交 SOE 连接改名事务失败: " + status.error_message());
  }
  LOG_INFO("IEC104 SOE 连接名称已更新: old_conn_name={}, new_conn_name={}", oldConnName, newConnName);
  return grpc::Status::OK;
}

grpc::Status IEC104SoeStore::DeleteConnection(std::string_view connName) {
  auto status = validateConnectionName(connName);
  if (!status.ok()) {
    return status;
  }

  SqliteDb db;
  status = openStore(databasePath_, &db);
  if (!status.ok()) {
    LOG_ERROR("IEC104 SOE 存储打开失败: conn_name={}, 错误={}", connName, status.error_message());
    return status;
  }

  Statement statement(db.get(), "DELETE FROM iec104_soe_connections WHERE conn_name=?");
  if (!statement.status().ok()) {
    return statement.status();
  }
  status = bindText(db.get(), statement.get(), 1, connName);
  if (status.ok()) {
    status = stepDone(db.get(), statement.get(), "删除 SOE 连接记录失败");
  }
  if (!status.ok()) {
    return status;
  }
  LOG_INFO("IEC104 SOE 连接记录已删除: conn_name={}, 删除连接数={}", connName, sqlite3_changes(db.get()));
  return grpc::Status::OK;
}

const std::filesystem::path& IEC104SoeStore::databasePath() const {
  return databasePath_;
}

}  // IEC104 命名空间结束
