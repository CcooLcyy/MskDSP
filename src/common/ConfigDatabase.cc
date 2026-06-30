#include "mskdsp/ConfigDatabase.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sqlite3.h>

namespace mskdsp {
namespace {
constexpr uint32_t kConfigDatabaseSchemaVersion = 1;
constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

void trace(const ConfigDatabase::TraceFn& fn, const std::string& message) {
  if (fn) {
    fn(message);
  }
}

std::string pathFields(const std::filesystem::path& path) {
  std::error_code ec;
  const auto absPath = std::filesystem::absolute(path, ec);
  std::string out = "db_path=" + path.string();
  if (ec) {
    out += ", abs_db_path=<解析失败:" + ec.message() + ">";
  } else {
    out += ", abs_db_path=" + absPath.string();
  }
  return out;
}

std::string identityFields(const std::filesystem::path& path, std::string_view moduleName, std::string_view configKey) {
  return pathFields(path) + ", module_name=" + std::string(moduleName) + ", config_key=" + std::string(configKey);
}

std::string keysField(const std::vector<std::string>& configKeys) {
  std::string out = "config_keys=[";
  for (size_t i = 0; i < configKeys.size(); ++i) {
    if (i != 0) {
      out += ",";
    }
    out += configKeys[i];
  }
  out += "]";
  return out;
}

std::string sqliteError(sqlite3* db) {
  if (db == nullptr) {
    return "数据库句柄为空";
  }
  const char* err = sqlite3_errmsg(db);
  return err == nullptr ? "未知 SQLite 错误" : err;
}

grpc::Status internalError(sqlite3* db, std::string_view prefix) {
  return grpc::Status(grpc::StatusCode::INTERNAL, std::string(prefix) + ": " + sqliteError(db));
}

uint64_t checksum(std::string_view payload) {
  uint64_t hash = kFnvOffset;
  for (unsigned char ch : payload) {
    hash ^= static_cast<uint64_t>(ch);
    hash *= kFnvPrime;
  }
  return hash;
}

std::string checksumHex(std::string_view payload) {
  return std::format("{:016x}", checksum(payload));
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
        return grpc::Status(grpc::StatusCode::INTERNAL, "创建配置数据库目录失败: " + ec.message());
      }
    }
    const int rc = sqlite3_open_v2(path.string().c_str(),
                                   &db_,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                   nullptr);
    if (rc != SQLITE_OK) {
      return internalError(db_, "打开配置数据库失败");
    }
    return Exec("PRAGMA busy_timeout=5000");
  }

  grpc::Status Exec(std::string_view sql) {
    char* err = nullptr;
    const std::string sqlText(sql);
    const int rc = sqlite3_exec(db_, sqlText.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
      std::string message = err == nullptr ? sqliteError(db_) : err;
      sqlite3_free(err);
      return grpc::Status(grpc::StatusCode::INTERNAL, message);
    }
    return grpc::Status::OK;
  }

  sqlite3* get() { return db_; }

private:
  sqlite3* db_{nullptr};
};

class Statement {
public:
  Statement(sqlite3* db, std::string_view sql) : db_(db) {
    const std::string sqlText(sql);
    const int rc = sqlite3_prepare_v2(db_, sqlText.c_str(), -1, &stmt_, nullptr);
    if (rc != SQLITE_OK) {
      status_ = internalError(db_, "准备 SQL 失败");
    }
  }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  ~Statement() {
    if (stmt_ != nullptr) {
      sqlite3_finalize(stmt_);
    }
  }

  grpc::Status status() const { return status_; }
  sqlite3_stmt* get() { return stmt_; }

private:
  sqlite3* db_{nullptr};
  sqlite3_stmt* stmt_{nullptr};
  grpc::Status status_{grpc::Status::OK};
};

grpc::Status bindText(sqlite3* db, sqlite3_stmt* stmt, int index, std::string_view value) {
  const int rc = sqlite3_bind_text(stmt, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
  if (rc != SQLITE_OK) {
    return internalError(db, "绑定文本参数失败");
  }
  return grpc::Status::OK;
}

grpc::Status bindBlob(sqlite3* db, sqlite3_stmt* stmt, int index, std::string_view value) {
  const int rc = sqlite3_bind_blob(stmt, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
  if (rc != SQLITE_OK) {
    return internalError(db, "绑定二进制参数失败");
  }
  return grpc::Status::OK;
}

grpc::Status ensureSchema(SqliteDb* db) {
  if (db == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "db 为空");
  }
  auto status = db->Exec("PRAGMA foreign_keys=ON");
  if (!status.ok()) {
    return status;
  }
  status = db->Exec(R"sql(
CREATE TABLE IF NOT EXISTS config_blobs (
  module_name TEXT NOT NULL,
  config_key TEXT NOT NULL,
  proto_type TEXT NOT NULL,
  schema_version INTEGER NOT NULL,
  payload BLOB NOT NULL,
  checksum TEXT NOT NULL,
  updated_at TEXT NOT NULL DEFAULT (datetime('now')),
  PRIMARY KEY(module_name, config_key)
)
)sql");
  if (!status.ok()) {
    return status;
  }
  return db->Exec(std::format("PRAGMA user_version={}", kConfigDatabaseSchemaVersion));
}

grpc::Status validateIdentity(std::string_view moduleName, std::string_view configKey) {
  if (moduleName.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "module_name 不能为空");
  }
  if (configKey.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "config_key 不能为空");
  }
  return grpc::Status::OK;
}
}  // namespace

ConfigDatabase::ConfigDatabase(std::filesystem::path dbPath) :
  dbPath_(std::move(dbPath)) {}

grpc::Status ConfigDatabase::SaveBlob(std::string_view moduleName,
                                      std::string_view configKey,
                                      std::string_view protoType,
                                      std::string_view payload,
                                      uint32_t schemaVersion,
                                      TraceFn traceFn) const {
  auto status = validateIdentity(moduleName, configKey);
  if (!status.ok()) {
    return status;
  }
  if (protoType.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "proto_type 不能为空");
  }
  if (schemaVersion == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "schema_version 不能为空");
  }

  SqliteDb db;
  status = db.Open(dbPath_);
  if (!status.ok()) {
    trace(traceFn, "打开配置数据库失败: " + pathFields(dbPath_) + ", 原因=" + status.error_message());
    return status;
  }
  status = ensureSchema(&db);
  if (!status.ok()) {
    trace(traceFn, "初始化配置数据库失败: " + pathFields(dbPath_) + ", 原因=" + status.error_message());
    return status;
  }
  status = db.Exec("BEGIN IMMEDIATE");
  if (!status.ok()) {
    trace(traceFn, "SQLite 配置保存事务启动失败: " + identityFields(dbPath_, moduleName, configKey) +
                       ", payload_size=" + std::to_string(payload.size()) + ", 原因=" + status.error_message());
    return status;
  }

  {
    Statement stmt(db.get(), R"sql(
INSERT INTO config_blobs(module_name, config_key, proto_type, schema_version, payload, checksum, updated_at)
VALUES (?, ?, ?, ?, ?, ?, datetime('now'))
ON CONFLICT(module_name, config_key) DO UPDATE SET
  proto_type=excluded.proto_type,
  schema_version=excluded.schema_version,
  payload=excluded.payload,
  checksum=excluded.checksum,
  updated_at=datetime('now')
)sql");
    status = stmt.status();
    if (status.ok()) {
      status = bindText(db.get(), stmt.get(), 1, moduleName);
    }
    if (status.ok()) {
      status = bindText(db.get(), stmt.get(), 2, configKey);
    }
    if (status.ok()) {
      status = bindText(db.get(), stmt.get(), 3, protoType);
    }
    if (status.ok()) {
      const int rc = sqlite3_bind_int(stmt.get(), 4, static_cast<int>(schemaVersion));
      if (rc != SQLITE_OK) {
        status = internalError(db.get(), "绑定 schema_version 失败");
      }
    }
    if (status.ok()) {
      status = bindBlob(db.get(), stmt.get(), 5, payload);
    }
    const auto sum = checksumHex(payload);
    if (status.ok()) {
      status = bindText(db.get(), stmt.get(), 6, sum);
    }
    if (status.ok()) {
      const int rc = sqlite3_step(stmt.get());
      if (rc != SQLITE_DONE) {
        status = internalError(db.get(), "保存配置 payload 失败");
      }
    }
  }

  if (!status.ok()) {
    (void)db.Exec("ROLLBACK");
    trace(traceFn, "SQLite 配置保存失败: " + identityFields(dbPath_, moduleName, configKey) +
                       ", payload_size=" + std::to_string(payload.size()) + ", 原因=" + status.error_message());
    return status;
  }
  status = db.Exec("COMMIT");
  if (!status.ok()) {
    trace(traceFn, "SQLite 配置提交失败: " + identityFields(dbPath_, moduleName, configKey) +
                       ", payload_size=" + std::to_string(payload.size()) + ", 原因=" + status.error_message());
    return status;
  }
  trace(traceFn, "SQLite 配置保存完成: " + identityFields(dbPath_, moduleName, configKey) +
                     ", payload_size=" + std::to_string(payload.size()));
  return grpc::Status::OK;
}

grpc::Status ConfigDatabase::LoadBlob(std::string_view moduleName,
                                      std::string_view configKey,
                                      std::string* payload,
                                      bool* found,
                                      TraceFn traceFn) const {
  if (payload == nullptr || found == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "输出参数为空");
  }
  payload->clear();
  *found = false;
  auto status = validateIdentity(moduleName, configKey);
  if (!status.ok()) {
    return status;
  }

  std::error_code ec;
  const bool dbExists = std::filesystem::exists(dbPath_, ec);
  if (ec) {
    trace(traceFn, "检查 SQLite 配置数据库是否存在失败: " + identityFields(dbPath_, moduleName, configKey) +
                       ", found=false, payload_size=0, 原因=" + ec.message());
  }
  if (!dbExists) {
    trace(traceFn, "SQLite 配置数据库不存在: " + identityFields(dbPath_, moduleName, configKey) +
                       ", found=false, payload_size=0");
    return grpc::Status::OK;
  }

  SqliteDb db;
  status = db.Open(dbPath_);
  if (!status.ok()) {
    trace(traceFn, "打开配置数据库失败: " + identityFields(dbPath_, moduleName, configKey) +
                       ", found=false, payload_size=0, 原因=" + status.error_message());
    return status;
  }
  status = ensureSchema(&db);
  if (!status.ok()) {
    trace(traceFn, "初始化配置数据库失败: " + identityFields(dbPath_, moduleName, configKey) +
                       ", found=false, payload_size=0, 原因=" + status.error_message());
    return status;
  }

  Statement stmt(db.get(), R"sql(
SELECT payload, checksum FROM config_blobs WHERE module_name=? AND config_key=?
)sql");
  status = stmt.status();
  if (status.ok()) {
    status = bindText(db.get(), stmt.get(), 1, moduleName);
  }
  if (status.ok()) {
    status = bindText(db.get(), stmt.get(), 2, configKey);
  }
  if (!status.ok()) {
    trace(traceFn, "查询 SQLite 配置项失败: " + identityFields(dbPath_, moduleName, configKey) +
                       ", found=false, payload_size=0, 原因=" + status.error_message());
    return status;
  }

  const int rc = sqlite3_step(stmt.get());
  if (rc == SQLITE_DONE) {
    trace(traceFn, "SQLite 配置项不存在: " + identityFields(dbPath_, moduleName, configKey) +
                       ", found=false, payload_size=0");
    return grpc::Status::OK;
  }
  if (rc != SQLITE_ROW) {
    auto errorStatus = internalError(db.get(), "加载配置 payload 失败");
    trace(traceFn, "加载 SQLite 配置项失败: " + identityFields(dbPath_, moduleName, configKey) +
                       ", found=false, payload_size=0, 原因=" + errorStatus.error_message());
    return errorStatus;
  }
  const auto* blob = static_cast<const char*>(sqlite3_column_blob(stmt.get(), 0));
  const int blobSize = sqlite3_column_bytes(stmt.get(), 0);
  const auto* sumText = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
  const int sumSize = sqlite3_column_bytes(stmt.get(), 1);
  if (blobSize > 0 && blob == nullptr) {
    trace(traceFn, "SQLite 配置 payload 为空指针: " + identityFields(dbPath_, moduleName, configKey) +
                       ", found=true, payload_size=" + std::to_string(blobSize));
    return grpc::Status(grpc::StatusCode::INTERNAL, "SQLite 配置 payload 为空指针");
  }
  payload->assign(blob == nullptr ? "" : blob, static_cast<size_t>(blobSize));
  const std::string storedChecksum(sumText == nullptr ? "" : sumText, static_cast<size_t>(sumSize));
  const auto actualChecksum = checksumHex(*payload);
  if (storedChecksum != actualChecksum) {
    payload->clear();
    trace(traceFn, "SQLite 配置 checksum 校验失败: " + identityFields(dbPath_, moduleName, configKey) +
                       ", found=true, payload_size=" + std::to_string(blobSize));
    return grpc::Status(grpc::StatusCode::INTERNAL, "SQLite 配置 checksum 校验失败");
  }
  *found = true;
  trace(traceFn, "SQLite 配置加载完成: " + identityFields(dbPath_, moduleName, configKey) +
                     ", found=true, payload_size=" + std::to_string(payload->size()));
  return grpc::Status::OK;
}

grpc::Status ConfigDatabase::HasAnyBlob(std::string_view moduleName,
                                        const std::vector<std::string>& configKeys,
                                        bool* found,
                                        TraceFn traceFn) const {
  if (found == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "found 为空");
  }
  *found = false;
  if (moduleName.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "module_name 不能为空");
  }
  if (configKeys.empty()) {
    trace(traceFn, "SQLite 模块配置查询跳过: " + pathFields(dbPath_) +
                       ", module_name=" + std::string(moduleName) +
                       ", config_keys=[], found=false");
    return grpc::Status::OK;
  }
  std::error_code ec;
  const bool dbExists = std::filesystem::exists(dbPath_, ec);
  if (ec) {
    trace(traceFn, "检查 SQLite 配置数据库是否存在失败: " + pathFields(dbPath_) +
                       ", module_name=" + std::string(moduleName) + ", " + keysField(configKeys) +
                       ", found=false, 原因=" + ec.message());
  }
  if (!dbExists) {
    trace(traceFn, "SQLite 配置数据库不存在: " + pathFields(dbPath_) +
                       ", module_name=" + std::string(moduleName) + ", " + keysField(configKeys) +
                       ", found=false");
    return grpc::Status::OK;
  }
  SqliteDb db;
  auto status = db.Open(dbPath_);
  if (!status.ok()) {
    trace(traceFn, "打开配置数据库失败: " + pathFields(dbPath_) +
                       ", module_name=" + std::string(moduleName) + ", " + keysField(configKeys) +
                       ", found=false, 原因=" + status.error_message());
    return status;
  }
  status = ensureSchema(&db);
  if (!status.ok()) {
    trace(traceFn, "初始化配置数据库失败: " + pathFields(dbPath_) +
                       ", module_name=" + std::string(moduleName) + ", " + keysField(configKeys) +
                       ", found=false, 原因=" + status.error_message());
    return status;
  }
  for (const auto& key : configKeys) {
    Statement stmt(db.get(), "SELECT 1 FROM config_blobs WHERE module_name=? AND config_key=? LIMIT 1");
    status = stmt.status();
    if (status.ok()) {
      status = bindText(db.get(), stmt.get(), 1, moduleName);
    }
    if (status.ok()) {
      status = bindText(db.get(), stmt.get(), 2, key);
    }
    if (!status.ok()) {
      trace(traceFn, "查询 SQLite 模块配置失败: " + identityFields(dbPath_, moduleName, key) +
                         ", found=false, 原因=" + status.error_message());
      return status;
    }
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
      *found = true;
      trace(traceFn, "SQLite 发现模块配置: " + identityFields(dbPath_, moduleName, key) + ", found=true");
      return grpc::Status::OK;
    }
    if (rc != SQLITE_DONE) {
      auto errorStatus = internalError(db.get(), "查询模块配置痕迹失败");
      trace(traceFn, "查询 SQLite 模块配置失败: " + identityFields(dbPath_, moduleName, key) +
                         ", found=false, 原因=" + errorStatus.error_message());
      return errorStatus;
    }
  }
  trace(traceFn, "SQLite 未发现模块配置: " + pathFields(dbPath_) +
                     ", module_name=" + std::string(moduleName) + ", " + keysField(configKeys) +
                     ", found=false");
  return grpc::Status::OK;
}

grpc::Status ConfigDatabase::DeleteBlobs(std::string_view moduleName,
                                         const std::vector<std::string>& configKeys,
                                         TraceFn traceFn) const {
  if (moduleName.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "module_name 不能为空");
  }
  if (configKeys.empty()) {
    trace(traceFn, "SQLite 配置清理跳过: " + pathFields(dbPath_) +
                       ", module_name=" + std::string(moduleName) +
                       ", config_keys=[], 删除记录数=0");
    return grpc::Status::OK;
  }
  std::error_code ec;
  const bool dbExists = std::filesystem::exists(dbPath_, ec);
  if (ec) {
    trace(traceFn, "检查 SQLite 配置数据库是否存在失败: " + pathFields(dbPath_) +
                       ", module_name=" + std::string(moduleName) + ", " + keysField(configKeys) +
                       ", 删除记录数=0, 原因=" + ec.message());
  }
  if (!dbExists) {
    trace(traceFn, "SQLite 配置数据库不存在，清理跳过: " + pathFields(dbPath_) +
                       ", module_name=" + std::string(moduleName) + ", " + keysField(configKeys) +
                       ", 删除记录数=0");
    return grpc::Status::OK;
  }
  SqliteDb db;
  auto status = db.Open(dbPath_);
  if (!status.ok()) {
    trace(traceFn, "打开配置数据库失败: " + pathFields(dbPath_) +
                       ", module_name=" + std::string(moduleName) + ", " + keysField(configKeys) +
                       ", 原因=" + status.error_message());
    return status;
  }
  status = ensureSchema(&db);
  if (!status.ok()) {
    trace(traceFn, "初始化配置数据库失败: " + pathFields(dbPath_) +
                       ", module_name=" + std::string(moduleName) + ", " + keysField(configKeys) +
                       ", 原因=" + status.error_message());
    return status;
  }
  status = db.Exec("BEGIN IMMEDIATE");
  if (!status.ok()) {
    trace(traceFn, "SQLite 配置清理事务启动失败: " + pathFields(dbPath_) +
                       ", module_name=" + std::string(moduleName) + ", " + keysField(configKeys) +
                       ", 原因=" + status.error_message());
    return status;
  }
  int deleted = 0;
  for (const auto& key : configKeys) {
    {
      Statement stmt(db.get(), "DELETE FROM config_blobs WHERE module_name=? AND config_key=?");
      status = stmt.status();
      if (status.ok()) {
        status = bindText(db.get(), stmt.get(), 1, moduleName);
      }
      if (status.ok()) {
        status = bindText(db.get(), stmt.get(), 2, key);
      }
      if (status.ok()) {
        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) {
          status = internalError(db.get(), "删除 SQLite 配置项失败");
        } else {
          deleted += sqlite3_changes(db.get());
        }
      }
    }
    if (!status.ok()) {
      (void)db.Exec("ROLLBACK");
      trace(traceFn, "SQLite 配置清理失败: " + identityFields(dbPath_, moduleName, key) +
                         ", 原因=" + status.error_message());
      return status;
    }
  }
  status = db.Exec("COMMIT");
  if (!status.ok()) {
    trace(traceFn, "SQLite 配置清理提交失败: " + pathFields(dbPath_) +
                       ", module_name=" + std::string(moduleName) + ", " + keysField(configKeys) +
                       ", 删除记录数=" + std::to_string(deleted) +
                       ", 原因=" + status.error_message());
    return status;
  }
  trace(traceFn, "SQLite 配置清理完成: " + pathFields(dbPath_) +
                     ", module_name=" + std::string(moduleName) + ", " + keysField(configKeys) +
                     ", 删除记录数=" + std::to_string(deleted));
  return grpc::Status::OK;
}

const std::filesystem::path& ConfigDatabase::dbPath() const {
  return dbPath_;
}

}  // namespace mskdsp
