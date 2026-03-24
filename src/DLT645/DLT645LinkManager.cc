#include "DLT645LinkManager.h"

#include <algorithm>
#include <boost/json.hpp>
#include <boost/system/error_code.hpp>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

#include "DLT645LibInfo.h"
#include "Logger.h"
#include "ThreadUtil.hpp"

namespace {
constexpr uint8_t kFrameStart = 0x68;
constexpr uint8_t kFrameEnd = 0x16;
constexpr uint8_t kReadControl = 0x11;
constexpr uint8_t kWriteControl = 0x14;
constexpr uint8_t kDefaultQos = 1;
constexpr uint32_t kDefaultPollIntervalMs = 1000;
constexpr uint32_t kDefaultPollItemIntervalMs = 0;
constexpr uint32_t kDefaultRequestTimeoutMs = 3000;
constexpr const char *kAppName = "AGVC";
constexpr const char *kAppTypeLora = "loraManager";
constexpr const char *kAppTypeCarrier = "ccoRouter";
constexpr const char *kAppTypeUart = "uartManager";
constexpr size_t kMinFrameSize = 12;
constexpr uint32_t kDefaultSerialBaudRate = 9600;
constexpr uint32_t kDefaultSerialDataBits = 8;
constexpr uint32_t kDefaultSerialByteTimeoutMs = 100;
constexpr uint32_t kDefaultSerialFrameTimeoutMs = 100;
constexpr uint32_t kDefaultSerialEstSize = 256;

bool useArchiveManagement(DLT645Proto::CommMode mode) {
  return mode == DLT645Proto::COMM_MODE_LORA || mode == DLT645Proto::COMM_MODE_CARRIER;
}

std::string makeArchiveKey(const DLT645Proto::LinkConfig &config) {
  return std::format("{}|{}", static_cast<int>(config.comm_mode()), config.meter_addr());
}

bool parseTokenString(const boost::json::value &value, std::string *out) {
  if (out == nullptr || !value.is_string()) {
    return false;
  }
  *out = value.as_string().c_str();
  return !out->empty();
}

std::string base64Encode(const std::vector<uint8_t> &data) {
  static const char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  for (size_t i = 0; i < data.size(); i += 3) {
    uint32_t octet_a = data[i];
    uint32_t octet_b = (i + 1 < data.size()) ? data[i + 1] : 0;
    uint32_t octet_c = (i + 2 < data.size()) ? data[i + 2] : 0;

    uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
    out.push_back(kTable[(triple >> 18) & 0x3F]);
    out.push_back(kTable[(triple >> 12) & 0x3F]);
    out.push_back(i + 1 < data.size() ? kTable[(triple >> 6) & 0x3F] : '=');
    out.push_back(i + 2 < data.size() ? kTable[triple & 0x3F] : '=');
  }
  return out;
}

bool base64Decode(std::string_view text, std::vector<uint8_t> *out) {
  if (out == nullptr) {
    return false;
  }
  static const int kDecodeTable[256] = {
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -2, -1, -1, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

  out->clear();
  uint32_t buffer = 0;
  int bitsCollected = 0;
  for (char ch : text) {
    int value = kDecodeTable[static_cast<unsigned char>(ch)];
    if (value == -1) {
      if (ch == '=') {
        break;
      }
      continue;
    }
    if (value == -2) {
      break;
    }
    buffer = (buffer << 6) | static_cast<uint32_t>(value);
    bitsCollected += 6;
    if (bitsCollected >= 8) {
      bitsCollected -= 8;
      out->push_back(static_cast<uint8_t>((buffer >> bitsCollected) & 0xFF));
    }
  }
  return !out->empty() || text.empty();
}

bool isDigits(const std::string &text) {
  if (text.empty()) {
    return false;
  }
  for (char ch : text) {
    if (ch < '0' || ch > '9') {
      return false;
    }
  }
  return true;
}

bool isHex(const std::string &text) {
  if (text.empty()) {
    return false;
  }
  for (char ch : text) {
    const bool ok = (ch >= '0' && ch <= '9') ||
        (ch >= 'a' && ch <= 'f') ||
        (ch >= 'A' && ch <= 'F');
    if (!ok) {
      return false;
    }
  }
  return true;
}

std::string toLowerAscii(std::string text) {
  for (char &ch : text) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return text;
}

std::string trimAscii(std::string text) {
  auto isSpace = [](char ch) {
    return std::isspace(static_cast<unsigned char>(ch)) != 0;
  };
  while (!text.empty() && isSpace(text.front())) {
    text.erase(text.begin());
  }
  while (!text.empty() && isSpace(text.back())) {
    text.pop_back();
  }
  return text;
}

bool parseInt32Text(const std::string &text, int32_t *out) {
  if (out == nullptr || text.empty()) {
    return false;
  }
  char *end = nullptr;
  const auto value = std::strtol(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') {
    return false;
  }
  *out = static_cast<int32_t>(value);
  return true;
}

int32_t parseStatusCode(const boost::json::value &value, bool *ok) {
  if (ok != nullptr) {
    *ok = false;
  }
  if (value.is_int64()) {
    if (ok != nullptr) {
      *ok = true;
    }
    return static_cast<int32_t>(value.as_int64());
  }
  if (value.is_uint64()) {
    if (ok != nullptr) {
      *ok = true;
    }
    return static_cast<int32_t>(value.as_uint64());
  }
  if (!value.is_string()) {
    return 0;
  }
  std::string text = value.as_string().c_str();
  text = trimAscii(std::move(text));
  if (text.empty()) {
    return 0;
  }
  int32_t parsed = 0;
  if (parseInt32Text(text, &parsed)) {
    if (ok != nullptr) {
      *ok = true;
    }
    return parsed;
  }

  auto lower = toLowerAscii(text);
  if (lower == "ok" || lower == "success") {
    if (ok != nullptr) {
      *ok = true;
    }
    return 0;
  }
  if (lower == "fail") {
    if (ok != nullptr) {
      *ok = true;
    }
    return 1;
  }
  if (lower == "frametimeout") {
    if (ok != nullptr) {
      *ok = true;
    }
    return 2;
  }
  if (lower == "porterror") {
    if (ok != nullptr) {
      *ok = true;
    }
    return 3;
  }
  if (lower == "buffull") {
    if (ok != nullptr) {
      *ok = true;
    }
    return 4;
  }
  if (lower == "formaterror") {
    if (ok != nullptr) {
      *ok = true;
    }
    return 5;
  }
  if (ok != nullptr) {
    *ok = true;
  }
  return 1;
}

struct ArchiveAddStatusResult {
  bool ok = false;
  int32_t status = 0;
  bool archiveExists = false;
};

ArchiveAddStatusResult parseArchiveAddStatus(const boost::json::value &value) {
  ArchiveAddStatusResult result;
  if (value.is_int64()) {
    result.ok = true;
    result.status = static_cast<int32_t>(value.as_int64());
    result.archiveExists = result.status == 2;
    return result;
  }
  if (value.is_uint64()) {
    result.ok = true;
    result.status = static_cast<int32_t>(value.as_uint64());
    result.archiveExists = result.status == 2;
    return result;
  }
  if (!value.is_string()) {
    return result;
  }

  std::string text = trimAscii(value.as_string().c_str());
  if (text.empty()) {
    return result;
  }

  int32_t parsed = 0;
  if (parseInt32Text(text, &parsed)) {
    result.ok = true;
    result.status = parsed;
    result.archiveExists = parsed == 2;
    return result;
  }

  result.status = parseStatusCode(value, &result.ok);
  return result;
}

std::string loraStatusToMessage(int32_t status) {
  switch (status) {
  case 0:
    return "成功";
  case 2:
    return "帧超时";
  case 3:
    return "端口错误";
  case 4:
    return "缓冲区满";
  case 5:
    return "格式错误";
  case 1:
  default:
    return "失败";
  }
}

bool sleepWithStop(std::stop_token st, std::chrono::milliseconds total) {
  constexpr auto kSlice = std::chrono::milliseconds(100);
  auto left = total;
  while (left > std::chrono::milliseconds::zero()) {
    if (st.stop_requested()) {
      return false;
    }
    const auto step = std::min(left, kSlice);
    std::this_thread::sleep_for(step);
    left -= step;
  }
  return !st.stop_requested();
}

std::string serialParityToText(DLT645Proto::SerialParity parity) {
  switch (parity) {
  case DLT645Proto::SERIAL_PARITY_ODD:
    return "odd";
  case DLT645Proto::SERIAL_PARITY_EVEN:
    return "even";
  case DLT645Proto::SERIAL_PARITY_NONE:
  case DLT645Proto::SERIAL_PARITY_UNSPECIFIED:
  default:
    return "none";
  }
}

uint32_t serialStopBitsToNumber(DLT645Proto::SerialStopBits stopBits) {
  switch (stopBits) {
  case DLT645Proto::SERIAL_STOP_BITS_TWO:
    return 2;
  case DLT645Proto::SERIAL_STOP_BITS_ONE:
  case DLT645Proto::SERIAL_STOP_BITS_UNSPECIFIED:
  default:
    return 1;
  }
}

grpc::Status restorePointTableFromProto(const DLT645Proto::PointTable &table, DLT645::PointTable *out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "点表输出参数为空");
  }
  DLT645::PointTable next;
  auto status = next.Upsert(table.points(), table.blocks(), true);
  if (!status.ok()) {
    return status;
  }
  *out = std::move(next);
  return grpc::Status::OK;
}
}  // namespace

namespace DLT645 {

LinkManager::LinkManager(std::string moduleName) :
  dataCenter_(moduleName),
  mqttClient_(moduleName),
  moduleName_(std::move(moduleName)) {}

void LinkManager::LoadPersistedConfig() {
  LOG_INFO("DLT645 开始加载本地持久化配置");

  {
    DLT645Proto::MqttConfig mqttConfig;
    auto status = mqttStore_.Load(&mqttConfig);
    if (!status.ok()) {
      LOG_ERROR("DLT645 MQTT 持久化配置加载失败: 原因={}", status.error_message());
    } else if (!mqttConfig.host().empty() && mqttConfig.port() != 0 && !mqttConfig.client_id().empty()) {
      mqttClient_.setConfig(mqttConfig);
      LOG_INFO("DLT645 已加载 MQTT 持久化配置: host={}, port={}, client_id={}", mqttConfig.host(), mqttConfig.port(), mqttConfig.client_id());
    } else {
      LOG_INFO("DLT645 未找到 MQTT 持久化配置");
    }
  }

  DLT645Proto::LinksConfig linksConfig;
  auto linksStatus = linkStore_.Load(&linksConfig);
  if (!linksStatus.ok()) {
    LOG_ERROR("DLT645 链路持久化配置加载失败: 原因={}", linksStatus.error_message());
    return;
  }

  DLT645Proto::PointTablesConfig pointTablesConfig;
  auto pointTablesStatus = pointTableStore_.Load(&pointTablesConfig);
  if (!pointTablesStatus.ok()) {
    LOG_ERROR("DLT645 点表持久化配置加载失败: 原因={}", pointTablesStatus.error_message());
    return;
  }
  LOG_INFO("DLT645 持久化配置载入摘要: 链路记录数={}, 点表记录数={}", linksConfig.links_size(), pointTablesConfig.point_tables_size());

  std::unordered_map<std::string, DLT645Proto::PointTable> pointTablesByConn;
  pointTablesByConn.reserve(static_cast<size_t>(pointTablesConfig.point_tables_size()));
  for (const auto &table : pointTablesConfig.point_tables()) {
    pointTablesByConn.emplace(table.conn_name(), table);
  }

  if (linksConfig.links().empty()) {
    if (!pointTablesByConn.empty()) {
      LOG_WARNING("DLT645 未找到链路持久化配置，但存在 {} 条点表持久化配置，准备清理孤立点表", pointTablesByConn.size());
      auto saveStatus = savePointTablesConfig(DLT645Proto::PointTablesConfig());
      if (!saveStatus.ok()) {
        LOG_ERROR("DLT645 清理孤立点表持久化配置失败: 原因={}", saveStatus.error_message());
      }
    } else {
      LOG_INFO("DLT645 未找到链路持久化配置");
    }
    return;
  }

  std::unordered_map<std::string, std::shared_ptr<LinkRuntime>> restoredLinks;
  restoredLinks.reserve(static_cast<size_t>(linksConfig.links_size()));
  bool needResaveLinks = false;
  bool needResavePointTables = false;
  std::unordered_set<std::string> pointTablesLeftByDataCenterFailure;

  for (const auto &persistedLink : linksConfig.links()) {
    if (!persistedLink.has_config()) {
      LOG_WARNING("DLT645 跳过空链路持久化记录");
      needResaveLinks = true;
      continue;
    }

    DLT645Proto::LinkConfig normalized;
    auto status = normalizeLinkConfig(persistedLink.config(), &normalized);
    if (!status.ok()) {
      LOG_ERROR("DLT645 链路持久化配置非法，已跳过: conn_name={}, 原因={}", persistedLink.config().conn_name(), status.error_message());
      needResaveLinks = true;
      if (!persistedLink.config().conn_name().empty() && pointTablesByConn.erase(persistedLink.config().conn_name()) > 0) {
        needResavePointTables = true;
      }
      continue;
    }

    size_t persistedPointCount = 0;
    size_t persistedBlockCount = 0;
    if (auto persistedTableIt = pointTablesByConn.find(normalized.conn_name());
        persistedTableIt != pointTablesByConn.end()) {
      persistedPointCount = static_cast<size_t>(persistedTableIt->second.points_size());
      persistedBlockCount = static_cast<size_t>(persistedTableIt->second.blocks_size());
    }
    LOG_INFO("DLT645 开始恢复链路持久化记录: conn_name={}, 持久化conn_id={}, 待删除={}, 持久化点数={}, 持久化数据块数={}", normalized.conn_name(), persistedLink.conn_id(), persistedLink.pending_delete(), persistedPointCount, persistedBlockCount);

    DataCenterProto::ConnectionInfo connInfo;
    status = dataCenter_.GetOrCreateConnection(normalized.conn_name(), &connInfo);
    if (!status.ok()) {
      if (persistedPointCount > 0 || persistedBlockCount > 0) {
        pointTablesLeftByDataCenterFailure.emplace(normalized.conn_name());
      }
      LOG_ERROR("DLT645 恢复链路时获取 DataCenter 连接失败: conn_name={}, 原因={}, 本地点表点数={}, 本地点表数据块数={}", normalized.conn_name(), status.error_message(), persistedPointCount, persistedBlockCount);
      continue;
    }

    auto runtime = std::make_shared<LinkRuntime>();
    runtime->config = normalized;
    runtime->connId = connInfo.conn_id();
    runtime->state = persistedLink.pending_delete() ? DLT645Proto::LINK_STATE_PENDING_DELETE
                                                    : DLT645Proto::LINK_STATE_STOPPED;

    size_t pointCount = 0;
    size_t blockCount = 0;
    auto tableIt = pointTablesByConn.find(normalized.conn_name());
    if (tableIt != pointTablesByConn.end()) {
      status = restorePointTableFromProto(tableIt->second, &runtime->pointTable);
      if (!status.ok()) {
        LOG_ERROR("DLT645 恢复点表失败，已跳过该点表: conn_name={}, 原因={}", normalized.conn_name(), status.error_message());
        needResavePointTables = true;
      } else {
        pointCount = static_cast<size_t>(tableIt->second.points_size());
        blockCount = static_cast<size_t>(tableIt->second.blocks_size());
        runtime->pointTableConfigured = pointCount > 0 || blockCount > 0;
        if (!runtime->pointTableConfigured) {
          LOG_WARNING("DLT645 恢复到空点表和空数据块，链路将保持已停止等待后续补全: conn_name={}", normalized.conn_name());
        }
      }
      pointTablesByConn.erase(tableIt);
    } else {
      LOG_WARNING("DLT645 恢复链路时未找到对应点表，链路将保持已停止等待后续补全: conn_name={}", normalized.conn_name());
    }

    auto syncStatus = dataCenter_.UpsertConnTags(runtime->connId, runtime->pointTable.Tags(), true);
    if (!syncStatus.ok()) {
      LOG_ERROR("DLT645 恢复链路时同步 DataCenter 连接标签注册表失败: conn_name={}, conn_id={}, 原因={}", normalized.conn_name(), runtime->connId, syncStatus.error_message());
    }

    if (persistedLink.conn_id() != 0 && persistedLink.conn_id() != runtime->connId) {
      LOG_WARNING("DLT645 恢复链路时发现 conn_id 已变化: conn_name={}, 持久化conn_id={}, 当前conn_id={}", normalized.conn_name(), persistedLink.conn_id(), runtime->connId);
      needResaveLinks = true;
    }

    restoredLinks[normalized.conn_name()] = runtime;
    LOG_INFO("DLT645 已恢复链路配置: conn_name={}, conn_id={}, 点数={}, 数据块数={}, 状态={}", normalized.conn_name(), runtime->connId, pointCount, blockCount, persistedLink.pending_delete() ? "待删除" : "已停止");
  }

  for (const auto &[connName, _] : pointTablesByConn) {
    auto tableIt = pointTablesByConn.find(connName);
    const size_t pointCount = tableIt == pointTablesByConn.end() ? 0u : static_cast<size_t>(tableIt->second.points_size());
    const size_t blockCount = tableIt == pointTablesByConn.end() ? 0u : static_cast<size_t>(tableIt->second.blocks_size());
    LOG_WARNING("DLT645 点表持久化配置未进入本次恢复快照: conn_name={}, 点数={}, 数据块数={}, 原因={}", connName, pointCount, blockCount, pointTablesLeftByDataCenterFailure.contains(connName) ? "链路恢复阶段获取 DataCenter 连接失败" : "未找到对应链路");
    needResavePointTables = true;
  }

  DLT645Proto::LinksConfig linksSnapshot;
  DLT645Proto::PointTablesConfig pointTablesSnapshot;
  {
    std::lock_guard<std::mutex> lock(mu_);
    linksByName_ = std::move(restoredLinks);
    linksSnapshot = dumpLinksConfigLocked();
    pointTablesSnapshot = dumpPointTablesConfigLocked();
  }

  if (needResaveLinks) {
    auto status = saveLinksConfig(linksSnapshot);
    if (!status.ok()) {
      LOG_ERROR("DLT645 清理链路持久化配置失败: 原因={}", status.error_message());
    }
  }
  if (needResavePointTables) {
    LOG_WARNING("DLT645 本次恢复将回写点表持久化配置: 恢复后链路数={}, 回写点表记录数={}", linksSnapshot.links_size(), pointTablesSnapshot.point_tables_size());
    auto status = savePointTablesConfig(pointTablesSnapshot);
    if (!status.ok()) {
      LOG_ERROR("DLT645 清理点表持久化配置失败: 原因={}", status.error_message());
    }
  }

  LOG_INFO("DLT645 本地持久化配置加载完成: 链路数={}", linksSnapshot.links_size());
  TryAutoStartReadyLinks("持久化恢复完成后");
}

void LinkManager::setDataCenterServerAddress(std::string address) {
  dataCenter_.setServerAddress(std::move(address));
}

void LinkManager::setDataCenterStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub) {
  dataCenter_.setStub(std::move(stub));
}

void LinkManager::setMqttStub(std::shared_ptr<MQTTManagerProto::MQTTManagerService::StubInterface> stub) {
  mqttClient_.setStub(std::move(stub));
  LOG_INFO("DLT645 已设置 MQTT Stub");
}

grpc::Status LinkManager::UpdateConfig(const DLT645Proto::UpdateConfigRequest &request, DLT645Proto::UpdateConfigResponse *response) {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
  }
  if (!request.has_mqtt()) {
    response->set_ok(false);
    response->set_message("MQTT 配置为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "MQTT 配置为空");
  }
  const auto &mqtt = request.mqtt();
  if (mqtt.host().empty() || mqtt.port() == 0 || mqtt.client_id().empty()) {
    response->set_ok(false);
    response->set_message("MQTT 连接参数不完整");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "MQTT 连接参数不完整");
  }
  mqttClient_.setConfig(mqtt);
  auto status = mqttStore_.Save(mqtt);
  if (!status.ok()) {
    response->set_ok(false);
    response->set_message("MQTT 配置落盘失败");
    LOG_ERROR("DLT645 MQTT 配置落盘失败: host={}, port={}, client_id={}, 原因={}", mqtt.host(), mqtt.port(), mqtt.client_id(), status.error_message());
    return status;
  }
  LOG_INFO("DLT645 MQTT 配置已落盘: host={}, port={}, client_id={}", mqtt.host(), mqtt.port(), mqtt.client_id());
  response->set_ok(true);
  response->set_message("MQTT 配置更新成功");
  autoStartEligibleLinks("MQTT 配置更新后");
  return grpc::Status::OK;
}

grpc::Status LinkManager::UpsertLink(const DLT645Proto::UpsertLinkRequest &request, DLT645Proto::LinkInfo *out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
  }
  if (!request.has_config()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "config 不能为空");
  }
  DLT645Proto::LinkConfig normalized;
  auto status = normalizeLinkConfig(request.config(), &normalized);
  if (!status.ok()) {
    return status;
  }

  const auto connName = normalized.conn_name();
  LOG_INFO("DLT645 开始创建或更新链路配置: conn_name={}, create_only={}", connName, request.create_only());

  DLT645Proto::LinksConfig linksConfig;
  DLT645Proto::PointTablesConfig pointTablesConfig;
  DLT645Proto::LinkInfo info;
  bool created = false;
  std::jthread archiveRetryThread;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it != linksByName_.end()) {
      if (request.create_only()) {
        LOG_WARNING("DLT645 创建链路配置失败: conn_name={}, 原因=连接已存在", connName);
        return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "连接已存在");
      }
      if (it->second->state == DLT645Proto::LINK_STATE_RUNNING) {
        LOG_WARNING("DLT645 更新链路配置失败: conn_name={}, 原因=更新配置前请先停止链路", connName);
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "更新配置前请先停止链路");
      }
      if (it->second->state == DLT645Proto::LINK_STATE_PENDING_DELETE) {
        LOG_WARNING("DLT645 更新链路配置失败: conn_name={}, 原因=连接处于待删除状态", connName);
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "连接处于待删除状态");
      }

      stopArchiveRetryLocked(it->second.get(), &archiveRetryThread);
      stopMqttSubscribeLocked(it->second.get());
      it->second->config = normalized;
      it->second->lastError.clear();
      linksConfig = dumpLinksConfigLocked();
      pointTablesConfig = dumpPointTablesConfigLocked();
      status = fillLinkInfoLocked(*it->second, &info);
      if (!status.ok()) {
        return status;
      }
    } else {
      if (pendingCreateByName_.contains(connName)) {
        LOG_WARNING("DLT645 创建链路配置失败: conn_name={}, 原因=连接创建中", connName);
        return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "连接已存在");
      }
      pendingCreateByName_.insert(connName);
    }
  }
  if (archiveRetryThread.joinable()) {
    archiveRetryThread.join();
  }

  if (info.has_config()) {
    status = saveLinksConfig(linksConfig);
    if (!status.ok()) {
      return status;
    }
    status = savePointTablesConfig(pointTablesConfig);
    if (!status.ok()) {
      return status;
    }
    (void)maybeAutoStartLink(connName, "链路配置更新后");
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = linksByName_.find(connName);
      if (it == linksByName_.end() || !it->second) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接不存在");
      }
      status = fillLinkInfoLocked(*it->second, out);
      if (!status.ok()) {
        return status;
      }
    }
    LOG_INFO("DLT645 更新链路配置成功: conn_name={}, conn_id={}", connName, out->conn_id());
    return grpc::Status::OK;
  }

  auto rollbackPendingCreate = [this, &connName]() {
    std::lock_guard<std::mutex> lock(mu_);
    pendingCreateByName_.erase(connName);
  };

  if (request.create_only()) {
    bool exists = false;
    status = dataCenter_.ConnectionExists(connName, &exists);
    if (!status.ok()) {
      rollbackPendingCreate();
      LOG_ERROR("DLT645 查询 DataCenter 连接失败: conn_name={}, 原因={}", connName, status.error_message());
      return status;
    }
    if (exists) {
      rollbackPendingCreate();
      LOG_WARNING("DLT645 创建链路配置失败: conn_name={}, 原因=DataCenter 中已存在同名连接", connName);
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "连接已存在");
    }
  }

  DataCenterProto::ConnectionInfo connInfo;
  status = dataCenter_.GetOrCreateConnection(connName, &connInfo);
  if (!status.ok()) {
    rollbackPendingCreate();
    LOG_ERROR("DLT645 获取 DataCenter 连接失败: conn_name={}, 原因={}", connName, status.error_message());
    return status;
  }
  if (connInfo.conn_id() == 0) {
    rollbackPendingCreate();
    LOG_ERROR("DLT645 获取到的 DataCenter conn_id 无效: conn_name={}", connName);
    return grpc::Status(grpc::StatusCode::INTERNAL, "DataCenter 返回 conn_id=0");
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    pendingCreateByName_.erase(connName);
    auto [it, inserted] = linksByName_.try_emplace(connName);
    if (!inserted) {
      if (request.create_only()) {
        LOG_WARNING("DLT645 创建链路配置失败: conn_name={}, 原因=连接已存在", connName);
        return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "连接已存在");
      }
      if (it->second->state == DLT645Proto::LINK_STATE_RUNNING) {
        LOG_WARNING("DLT645 更新链路配置失败: conn_name={}, 原因=更新配置前请先停止链路", connName);
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "更新配置前请先停止链路");
      }
      if (it->second->state == DLT645Proto::LINK_STATE_PENDING_DELETE) {
        LOG_WARNING("DLT645 更新链路配置失败: conn_name={}, 原因=连接处于待删除状态", connName);
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "连接处于待删除状态");
      }

      stopArchiveRetryLocked(it->second.get(), &archiveRetryThread);
      stopMqttSubscribeLocked(it->second.get());
      it->second->config = normalized;
      it->second->lastError.clear();
      linksConfig = dumpLinksConfigLocked();
      pointTablesConfig = dumpPointTablesConfigLocked();
      status = fillLinkInfoLocked(*it->second, out);
      if (!status.ok()) {
        return status;
      }
    } else {
      it->second = std::make_shared<LinkRuntime>();
      it->second->config = normalized;
      it->second->connId = connInfo.conn_id();
      it->second->state = DLT645Proto::LINK_STATE_STOPPED;
      created = true;
      linksConfig = dumpLinksConfigLocked();
      pointTablesConfig = dumpPointTablesConfigLocked();
      status = fillLinkInfoLocked(*it->second, out);
      if (!status.ok()) {
        return status;
      }
    }
  }
  if (archiveRetryThread.joinable()) {
    archiveRetryThread.join();
  }

  status = saveLinksConfig(linksConfig);
  if (!status.ok()) {
    return status;
  }
  status = savePointTablesConfig(pointTablesConfig);
  if (!status.ok()) {
    return status;
  }
  (void)maybeAutoStartLink(connName, created ? "链路配置创建后" : "链路配置更新后");
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end() || !it->second) {
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接不存在");
    }
    status = fillLinkInfoLocked(*it->second, out);
    if (!status.ok()) {
      return status;
    }
  }
  if (created) {
    LOG_INFO("DLT645 创建链路配置成功: conn_name={}, conn_id={}", connName, out->conn_id());
  } else {
    LOG_INFO("DLT645 更新链路配置成功: conn_name={}, conn_id={}", connName, out->conn_id());
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::GetLink(const std::string &connName, DLT645Proto::LinkInfo *out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
  }
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }
  std::lock_guard<std::mutex> lock(mu_);
  auto it = linksByName_.find(connName);
  if (it == linksByName_.end()) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接不存在");
  }
  return fillLinkInfoLocked(*it->second, out);
}

grpc::Status LinkManager::ListLinks(DLT645Proto::ListLinksResponse *out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
  }
  std::lock_guard<std::mutex> lock(mu_);
  out->Clear();
  for (const auto &item : linksByName_) {
    auto *info = out->add_links();
    fillLinkInfoLocked(*item.second, info);
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::StartLink(const std::string &connName) {
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }
  LOG_INFO("DLT645 开始启动连接功能: conn_name={}", connName);

  std::shared_ptr<LinkRuntime> link;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接不存在");
    }
    link = it->second;
  }
  if (!link) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接不存在");
  }

  std::unique_lock<std::mutex> reqLock(link->requestMutex);
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end() || it->second.get() != link.get()) {
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接不存在");
    }
    if (link->state == DLT645Proto::LINK_STATE_RUNNING) {
      LOG_INFO("DLT645 启动连接功能跳过: conn_name={}, 原因=连接已在运行", connName);
      return grpc::Status::OK;
    }
    if (link->archiveRetrying) {
      LOG_INFO("DLT645 启动连接功能跳过: conn_name={}, 原因=正在后台重试档案添加", connName);
      return grpc::Status::OK;
    }
    std::string reason;
    if (!isLinkAutoStartReadyLocked(*link, &reason)) {
      link->lastError = reason;
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, reason);
    }
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    startMqttSubscribeLocked(connName, link);
  }

  const bool useArchive = useArchiveManagement(link->config.comm_mode());
  const std::string archiveKey = useArchive ? makeArchiveKey(link->config) : std::string();
  bool holdArchiveRef = false;
  bool needAddArchive = false;

  if (useArchive) {
    {
      std::unique_lock<std::mutex> lock(mu_);
      while (true) {
        archiveStateCv_.wait(lock, [this, &archiveKey]() {
          return archiveAddInFlightByKey_.count(archiveKey) == 0 &&
              archiveDelInFlightByKey_.count(archiveKey) == 0;
        });
        auto it = archiveRefCountByKey_.find(archiveKey);
        if (it != archiveRefCountByKey_.end() && it->second > 0) {
          it->second += 1;
          holdArchiveRef = true;
          LOG_INFO("DLT645 启动连接功能复用档案引用: conn_name={}, meter_addr={}, 引用计数={}", connName, link->config.meter_addr(), it->second);
          break;
        }
        archiveAddInFlightByKey_.insert(archiveKey);
        needAddArchive = true;
        LOG_INFO("DLT645 启动连接功能获取档案添加资格: conn_name={}, meter_addr={}", connName, link->config.meter_addr());
        break;
      }
    }

    if (needAddArchive) {
      bool archiveAlreadyExists = false;
      LOG_INFO("DLT645 启动连接功能进入档案添加阶段: conn_name={}, comm_mode={}", connName, DLT645Proto::CommMode_Name(link->config.comm_mode()));
      status = sendAddSlaveNode(link.get(), &archiveAlreadyExists);
      {
        std::lock_guard<std::mutex> lock(mu_);
        archiveAddInFlightByKey_.erase(archiveKey);
        if (status.ok()) {
          archiveRefCountByKey_[archiveKey] = 1;
          holdArchiveRef = true;
          LOG_INFO("DLT645 启动连接功能档案引用建立成功: conn_name={}, meter_addr={}, 引用计数=1", connName, link->config.meter_addr());
        } else {
          archiveRefCountByKey_.erase(archiveKey);
        }
      }
      archiveStateCv_.notify_all();
      if (!status.ok()) {
        LOG_ERROR("DLT645 启动连接功能档案添加失败: conn_name={}, 原因={}", connName, status.error_message());
        {
          std::lock_guard<std::mutex> lock(mu_);
          link->lastError = status.error_message();
          launchArchiveRetryLocked(connName, link, archiveKey);
        }
        LOG_WARNING("DLT645 启动连接功能将在后台持续重试档案添加: conn_name={}, 重试间隔=5000ms", connName);
        return status;
      }
      if (archiveAlreadyExists) {
        LOG_INFO("DLT645 启动连接功能识别到档案已存在: conn_name={}, 后续不再重复下发档案，继续启动连接功能",
                 connName);
      } else {
        LOG_INFO("DLT645 启动连接功能档案添加成功: conn_name={}", connName);
      }
    } else {
      LOG_INFO("DLT645 启动连接功能跳过档案添加: conn_name={}, meter_addr={}, 原因=同地址档案已存在", connName, link->config.meter_addr());
    }
  } else {
    LOG_INFO("DLT645 启动连接功能跳过档案管理: conn_name={}, comm_mode={}", connName, DLT645Proto::CommMode_Name(link->config.comm_mode()));
  }

  bool linkMissing = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end() || it->second.get() != link.get()) {
      stopMqttSubscribeLocked(link.get());
      linkMissing = true;
    } else {
      startPollingLocked(connName, link);
      startDataCenterSubscribeLocked(connName, link);
      link->state = DLT645Proto::LINK_STATE_RUNNING;
      link->lastError.clear();
    }
  }
  if (linkMissing) {
    if (useArchive && holdArchiveRef) {
      releaseArchiveRefOnStartAbort(connName, link, archiveKey);
    }
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接不存在");
  }
  LOG_INFO("DLT645 启动连接功能完成: conn_name={}", connName);
  return grpc::Status::OK;
}

grpc::Status LinkManager::StopLink(const std::string &connName) {
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }
  LOG_INFO("DLT645 开始停止连接功能: conn_name={}", connName);
  std::shared_ptr<LinkRuntime> link;
  std::jthread archiveRetryThread;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接不存在");
    }
    link = it->second;
    stopArchiveRetryLocked(link.get(), &archiveRetryThread);
  }
  if (!link) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接不存在");
  }
  if (archiveRetryThread.joinable()) {
    archiveRetryThread.join();
  }

  std::unique_lock<std::mutex> reqLock(link->requestMutex);
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end() || it->second.get() != link.get()) {
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接不存在");
    }
    if (link->state == DLT645Proto::LINK_STATE_STOPPED) {
      stopMqttSubscribeLocked(link.get());
      LOG_INFO("DLT645 停止连接功能跳过: conn_name={}, 原因=连接已停止", connName);
      return grpc::Status::OK;
    }

    stopPollingLocked(link.get());
    stopDataCenterSubscribeLocked(link.get());
  }

  const bool useArchive = useArchiveManagement(link->config.comm_mode());
  const std::string archiveKey = useArchive ? makeArchiveKey(link->config) : std::string();
  bool needDeleteArchive = false;
  bool pendingDelete = false;
  if (useArchive) {
    {
      std::unique_lock<std::mutex> lock(mu_);
      while (true) {
        archiveStateCv_.wait(lock, [this, &archiveKey]() {
          return archiveAddInFlightByKey_.count(archiveKey) == 0 &&
              archiveDelInFlightByKey_.count(archiveKey) == 0;
        });
        auto it = archiveRefCountByKey_.find(archiveKey);
        if (it == archiveRefCountByKey_.end()) {
          LOG_WARNING("DLT645 停止连接功能未找到档案引用，跳过档案删除: conn_name={}, meter_addr={}", connName, link->config.meter_addr());
          break;
        }
        if (it->second == 0) {
          LOG_WARNING("DLT645 停止连接功能发现档案引用计数异常(0)，跳过档案删除: conn_name={}, meter_addr={}", connName, link->config.meter_addr());
          archiveRefCountByKey_.erase(it);
          break;
        }
        if (it->second > 1) {
          it->second -= 1;
          LOG_INFO("DLT645 停止连接功能复用档案引用，跳过档案删除: conn_name={}, meter_addr={}, 剩余引用计数={}", connName, link->config.meter_addr(), it->second);
          break;
        }
        archiveDelInFlightByKey_.insert(archiveKey);
        needDeleteArchive = true;
        LOG_INFO("DLT645 停止连接功能获取档案删除资格: conn_name={}, meter_addr={}", connName, link->config.meter_addr());
        break;
      }
    }

    if (needDeleteArchive) {
      LOG_INFO("DLT645 停止连接功能进入档案删除阶段: conn_name={}, comm_mode={}", connName, DLT645Proto::CommMode_Name(link->config.comm_mode()));
      auto delStatus = sendDelSlaveNode(link.get());
      {
        std::lock_guard<std::mutex> lock(mu_);
        archiveDelInFlightByKey_.erase(archiveKey);
        auto it = archiveRefCountByKey_.find(archiveKey);
        if (it != archiveRefCountByKey_.end()) {
          if (it->second > 0) {
            it->second -= 1;
          }
          const uint32_t leftRef = it->second;
          if (leftRef == 0) {
            archiveRefCountByKey_.erase(it);
          }
          LOG_INFO("DLT645 停止连接功能更新档案引用状态: conn_name={}, meter_addr={}, 剩余引用计数={}", connName, link->config.meter_addr(), leftRef);
        } else {
          LOG_WARNING("DLT645 停止连接功能未找到档案引用状态: conn_name={}, meter_addr={}", connName, link->config.meter_addr());
        }
      }
      archiveStateCv_.notify_all();
      if (!delStatus.ok()) {
        LOG_WARNING("DLT645 停止连接功能档案删除失败: conn_name={}, 原因={}", connName, delStatus.error_message());
        std::lock_guard<std::mutex> lock(mu_);
        link->lastError = delStatus.error_message();
      } else {
        LOG_INFO("DLT645 停止连接功能档案删除成功: conn_name={}", connName);
      }
    } else {
      LOG_INFO("DLT645 停止连接功能跳过档案删除: conn_name={}, meter_addr={}, 原因=同地址仍有运行连接或无档案引用", connName, link->config.meter_addr());
    }
  } else {
    LOG_INFO("DLT645 停止连接功能跳过档案管理: conn_name={}, comm_mode={}", connName, DLT645Proto::CommMode_Name(link->config.comm_mode()));
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end() || it->second.get() != link.get()) {
      return grpc::Status::OK;
    }
    pendingDelete = (link->state == DLT645Proto::LINK_STATE_PENDING_DELETE);
    stopMqttSubscribeLocked(link.get());
    link->state = pendingDelete ? DLT645Proto::LINK_STATE_PENDING_DELETE : DLT645Proto::LINK_STATE_STOPPED;
  }
  if (pendingDelete) {
    LOG_INFO("DLT645 停止连接功能完成并保持待删除状态: conn_name={}", connName);
  } else {
    LOG_INFO("DLT645 停止连接功能完成: conn_name={}", connName);
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::DeleteLink(const std::string &connName) {
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }

  status = StopLink(connName);
  if (!status.ok() && status.error_code() != grpc::StatusCode::NOT_FOUND) {
    return status;
  }

  auto dc = dataCenter_.DeleteConnection(connName);
  if (!dc.ok() && dc.error_code() != grpc::StatusCode::NOT_FOUND) {
    DLT645Proto::LinksConfig linksConfig;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = linksByName_.find(connName);
      if (it != linksByName_.end()) {
        it->second->state = DLT645Proto::LINK_STATE_PENDING_DELETE;
        it->second->lastError = dc.error_message();
        linksConfig = dumpLinksConfigLocked();
      }
    }
    if (linksConfig.links_size() > 0) {
      auto saveStatus = saveLinksConfig(linksConfig);
      if (!saveStatus.ok()) {
        LOG_ERROR("DLT645 待删除链路配置落盘失败: conn_name={}, 原因={}", connName, saveStatus.error_message());
        return saveStatus;
      }
    }
    LOG_WARNING("DLT645 删除连接失败，已标记待删除: conn_name={}, 原因={}", connName, dc.error_message());
    return dc;
  }

  DLT645Proto::LinksConfig linksConfig;
  DLT645Proto::PointTablesConfig pointTablesConfig;
  {
    std::lock_guard<std::mutex> lock(mu_);
    linksByName_.erase(connName);
    linksConfig = dumpLinksConfigLocked();
    pointTablesConfig = dumpPointTablesConfigLocked();
  }
  status = saveLinksConfig(linksConfig);
  if (!status.ok()) {
    return status;
  }
  status = savePointTablesConfig(pointTablesConfig);
  if (!status.ok()) {
    return status;
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::UpsertPointTable(const DLT645Proto::UpsertPointTableRequest &request) {
  auto status = validateConnName(request.conn_name());
  if (!status.ok()) {
    return status;
  }

  PointTable current;
  uint32_t connId = 0;
  std::jthread archiveRetryThread;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(request.conn_name());
    if (it == linksByName_.end()) {
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接不存在");
    }
    if (it->second->state == DLT645Proto::LINK_STATE_RUNNING) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "更新点表前请先停止链路");
    }
    if (it->second->state == DLT645Proto::LINK_STATE_PENDING_DELETE) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "连接处于待删除状态");
    }
    stopArchiveRetryLocked(it->second.get(), &archiveRetryThread);
    stopMqttSubscribeLocked(it->second.get());
    connId = it->second->connId;
    current = it->second->pointTable;
  }
  if (archiveRetryThread.joinable()) {
    archiveRetryThread.join();
  }

  PointTable next = current;
  status = next.Upsert(request.points(), request.blocks(), request.replace());
  if (!status.ok()) {
    return status;
  }

  auto tags = next.Tags();
  status = dataCenter_.UpsertConnTags(connId, tags, true);
  if (!status.ok()) {
    return status;
  }

  DLT645Proto::PointTablesConfig pointTablesConfig;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(request.conn_name());
    if (it == linksByName_.end()) {
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接不存在");
    }
    it->second->pointTable = std::move(next);
    it->second->pointTableConfigured =
        !it->second->pointTable.Points().empty() || !it->second->pointTable.Blocks().empty();
    pointTablesConfig = dumpPointTablesConfigLocked();
  }
  status = savePointTablesConfig(pointTablesConfig);
  if (!status.ok()) {
    return status;
  }
  (void)maybeAutoStartLink(request.conn_name(), "点表配置更新后");
  return grpc::Status::OK;
}

grpc::Status LinkManager::GetPointTable(const std::string &connName, DLT645Proto::PointTable *out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
  }
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }
  std::lock_guard<std::mutex> lock(mu_);
  auto it = linksByName_.find(connName);
  if (it == linksByName_.end()) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接不存在");
  }
  it->second->pointTable.ToProto(connName, out);
  return grpc::Status::OK;
}

grpc::Status LinkManager::validateConnName(const std::string &connName) const {
  if (connName.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_name 不能为空");
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::normalizeLinkConfig(const DLT645Proto::LinkConfig &config, DLT645Proto::LinkConfig *out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "config 为空");
  }
  if (config.conn_name().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_name 不能为空");
  }
  if (config.protocol_variant() == DLT645Proto::PROTOCOL_VARIANT_UNSPECIFIED) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "protocol_variant 不能为空");
  }
  if (config.comm_mode() == DLT645Proto::COMM_MODE_UNSPECIFIED) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "comm_mode 不能为空");
  }
  if (config.meter_addr().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "meter_addr 不能为空");
  }
  if (config.meter_addr().size() != 12 || !isDigits(config.meter_addr())) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "meter_addr 必须为 12 位数字字符串");
  }
  if (config.protocol_variant() == DLT645Proto::PROTOCOL_VARIANT_DLT645_PCD) {
    if (config.device_no().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "PCD 模式需要 device_no");
    }
    if (config.device_no().size() != 2 || !isHex(config.device_no())) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "device_no 必须为 2 位十六进制字符串");
    }
  }
  if (config.transport_type() == DLT645Proto::TRANSPORT_UNSPECIFIED ||
      config.transport_type() == DLT645Proto::TRANSPORT_MQTT) {
    *out = config;
    if (out->transport_type() == DLT645Proto::TRANSPORT_UNSPECIFIED) {
      out->set_transport_type(DLT645Proto::TRANSPORT_MQTT);
    }
    if (out->poll_interval_ms() == 0) {
      out->set_poll_interval_ms(kDefaultPollIntervalMs);
    }
    if (out->poll_item_interval_ms() == 0) {
      out->set_poll_item_interval_ms(kDefaultPollItemIntervalMs);
    }
    if (out->request_timeout_ms() == 0) {
      out->set_request_timeout_ms(kDefaultRequestTimeoutMs);
    }
    if (out->comm_mode() == DLT645Proto::COMM_MODE_SERIAL) {
      if (out->serial_port().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "串口模式 serial_port 不能为空");
      }
      if (out->serial_baud_rate() == 0) {
        out->set_serial_baud_rate(kDefaultSerialBaudRate);
      }
      if (out->serial_data_bits() == 0) {
        out->set_serial_data_bits(kDefaultSerialDataBits);
      }
      if (out->serial_data_bits() < 5 || out->serial_data_bits() > 8) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "serial_data_bits 仅支持 5..8");
      }
      if (out->serial_parity() == DLT645Proto::SERIAL_PARITY_UNSPECIFIED) {
        out->set_serial_parity(DLT645Proto::SERIAL_PARITY_NONE);
      }
      if (out->serial_stop_bits() == DLT645Proto::SERIAL_STOP_BITS_UNSPECIFIED) {
        out->set_serial_stop_bits(DLT645Proto::SERIAL_STOP_BITS_ONE);
      }
      if (out->serial_stop_bits() != DLT645Proto::SERIAL_STOP_BITS_ONE &&
          out->serial_stop_bits() != DLT645Proto::SERIAL_STOP_BITS_TWO) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "serial_stop_bits 仅支持 1 或 2");
      }
      if (out->serial_byte_timeout_ms() == 0) {
        out->set_serial_byte_timeout_ms(kDefaultSerialByteTimeoutMs);
      }
      if (out->serial_frame_timeout_ms() == 0) {
        out->set_serial_frame_timeout_ms(kDefaultSerialFrameTimeoutMs);
      }
      if (out->serial_est_size() == 0) {
        out->set_serial_est_size(kDefaultSerialEstSize);
      }
    }
    return grpc::Status::OK;
  }
  return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "暂不支持该传输类型");
}

grpc::Status LinkManager::fillLinkInfoLocked(const LinkRuntime &link, DLT645Proto::LinkInfo *out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
  }
  *out->mutable_config() = link.config;
  out->set_conn_id(link.connId);
  out->set_state(link.state);
  out->set_last_error(link.lastError);
  return grpc::Status::OK;
}

DLT645Proto::LinksConfig LinkManager::dumpLinksConfigLocked() const {
  DLT645Proto::LinksConfig config;
  std::vector<std::string> connNames;
  connNames.reserve(linksByName_.size());
  for (const auto &[connName, _] : linksByName_) {
    connNames.push_back(connName);
  }
  std::sort(connNames.begin(), connNames.end());

  for (const auto &connName : connNames) {
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end() || !it->second) {
      continue;
    }
    auto *link = config.add_links();
    *link->mutable_config() = it->second->config;
    link->set_conn_id(it->second->connId);
    link->set_pending_delete(it->second->state == DLT645Proto::LINK_STATE_PENDING_DELETE);
  }
  return config;
}

DLT645Proto::PointTablesConfig LinkManager::dumpPointTablesConfigLocked() const {
  DLT645Proto::PointTablesConfig config;
  std::vector<std::string> connNames;
  connNames.reserve(linksByName_.size());
  for (const auto &[connName, _] : linksByName_) {
    connNames.push_back(connName);
  }
  std::sort(connNames.begin(), connNames.end());

  for (const auto &connName : connNames) {
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end() || !it->second) {
      continue;
    }
    DLT645Proto::PointTable table;
    it->second->pointTable.ToProto(connName, &table);
    if (table.points().empty() && table.blocks().empty()) {
      continue;
    }
    *config.add_point_tables() = std::move(table);
  }
  return config;
}

grpc::Status LinkManager::saveLinksConfig(const DLT645Proto::LinksConfig &config) {
  auto status = linkStore_.Save(config);
  if (!status.ok()) {
    LOG_ERROR("DLT645 链路配置落盘失败: 链路数={}, 原因={}", config.links_size(), status.error_message());
    return status;
  }
  LOG_INFO("DLT645 链路配置已落盘: 链路数={}", config.links_size());
  return grpc::Status::OK;
}

grpc::Status LinkManager::savePointTablesConfig(const DLT645Proto::PointTablesConfig &config) {
  auto status = pointTableStore_.Save(config);
  if (!status.ok()) {
    LOG_ERROR("DLT645 点表配置落盘失败: 链路数={}, 原因={}", config.point_tables_size(), status.error_message());
    return status;
  }
  LOG_INFO("DLT645 点表配置已落盘: 链路数={}", config.point_tables_size());
  return grpc::Status::OK;
}

bool LinkManager::isLinkAutoStartReadyLocked(const LinkRuntime &link, std::string *reason) const {
  auto setReason = [reason](std::string text) {
    if (reason != nullptr) {
      *reason = std::move(text);
    }
    return false;
  };

  if (link.state == DLT645Proto::LINK_STATE_RUNNING) {
    return setReason("连接已在运行");
  }
  if (link.state == DLT645Proto::LINK_STATE_PENDING_DELETE) {
    return setReason("连接处于待删除状态");
  }
  if (link.connId == 0) {
    return setReason("conn_id 无效");
  }
  if (!link.pointTableConfigured) {
    return setReason("链路点表未就绪，当前规则要求链路配置和点表都成功恢复或下发后才启动连接功能");
  }
  if (!mqttClient_.hasConfig()) {
    return setReason("MQTT 全局配置未就绪，当前规则要求 MQTT 配置成功恢复或下发后才启动连接功能");
  }
  return true;
}

grpc::Status LinkManager::maybeAutoStartLink(const std::string &connName, std::string_view trigger) {
  std::string reason;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end() || !it->second) {
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "连接不存在");
    }
    if (!isLinkAutoStartReadyLocked(*it->second, &reason)) {
      it->second->lastError = reason;
      LOG_INFO("DLT645 暂不自动启动连接: conn_name={}, 触发来源={}, 原因={}", connName, trigger, reason);
      return grpc::Status::OK;
    }
  }

  LOG_INFO("DLT645 检测到连接已满足最小可运行条件，准备自动启动: conn_name={}, 触发来源={}", connName, trigger);
  auto status = StartLink(connName);
  if (!status.ok()) {
    LOG_WARNING("DLT645 自动启动连接失败: conn_name={}, 触发来源={}, 原因={}", connName, trigger, status.error_message());
    return status;
  }
  LOG_INFO("DLT645 自动启动连接完成: conn_name={}, 触发来源={}", connName, trigger);
  return grpc::Status::OK;
}

void LinkManager::autoStartEligibleLinks(std::string_view trigger) {
  std::vector<std::string> connNames;
  {
    std::lock_guard<std::mutex> lock(mu_);
    connNames.reserve(linksByName_.size());
    for (const auto &[connName, _] : linksByName_) {
      connNames.emplace_back(connName);
    }
  }
  std::sort(connNames.begin(), connNames.end());

  struct BatchGroup {
    std::string archiveKey;
    std::vector<std::string> connNames;
    std::vector<std::shared_ptr<LinkRuntime>> links;
  };

  std::unordered_map<std::string, BatchGroup> batchGroupsByKey;
  std::vector<std::string> batchGroupOrder;
  std::unordered_set<std::string> handledConnNames;

  {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto &connName : connNames) {
      auto it = linksByName_.find(connName);
      if (it == linksByName_.end() || !it->second) {
        continue;
      }
      auto link = it->second;
      if (link->config.comm_mode() != DLT645Proto::COMM_MODE_LORA) {
        continue;
      }
      if (link->archiveRetrying) {
        continue;
      }
      std::string reason;
      if (!isLinkAutoStartReadyLocked(*link, &reason)) {
        continue;
      }
      const auto archiveKey = makeArchiveKey(link->config);
      auto refIt = archiveRefCountByKey_.find(archiveKey);
      if (refIt != archiveRefCountByKey_.end() && refIt->second > 0) {
        continue;
      }
      if (archiveAddInFlightByKey_.contains(archiveKey) || archiveDelInFlightByKey_.contains(archiveKey)) {
        continue;
      }

      auto [groupIt, inserted] = batchGroupsByKey.try_emplace(archiveKey);
      if (inserted) {
        groupIt->second.archiveKey = archiveKey;
        batchGroupOrder.push_back(archiveKey);
      }
      groupIt->second.connNames.push_back(connName);
      groupIt->second.links.push_back(std::move(link));
    }

    if (batchGroupOrder.size() > 1) {
      for (const auto &archiveKey : batchGroupOrder) {
        archiveAddInFlightByKey_.insert(archiveKey);
      }
    } else {
      batchGroupsByKey.clear();
      batchGroupOrder.clear();
    }
  }

  if (!batchGroupOrder.empty()) {
    std::vector<LinkRuntime *> requestLinks;
    requestLinks.reserve(batchGroupOrder.size());
    for (const auto &archiveKey : batchGroupOrder) {
      requestLinks.push_back(batchGroupsByKey.at(archiveKey).links.front().get());
    }

    bool archiveAlreadyExists = false;
    auto batchStatus = sendAddSlaveNodes(requestLinks, &archiveAlreadyExists);

    {
      std::lock_guard<std::mutex> lock(mu_);
      for (const auto &archiveKey : batchGroupOrder) {
        archiveAddInFlightByKey_.erase(archiveKey);
      }

      if (batchStatus.ok()) {
        for (const auto &archiveKey : batchGroupOrder) {
          auto groupIt = batchGroupsByKey.find(archiveKey);
          if (groupIt == batchGroupsByKey.end()) {
            continue;
          }

          const auto &group = groupIt->second;
          uint32_t startedCount = 0;
          for (size_t i = 0; i < group.connNames.size(); ++i) {
            const auto &connName = group.connNames[i];
            const auto &link = group.links[i];
            auto liveIt = linksByName_.find(connName);
            if (liveIt == linksByName_.end() || liveIt->second != link) {
              LOG_WARNING("DLT645 批量自动启动跳过失效链路: conn_name={}", connName);
              continue;
            }
            if (link->state == DLT645Proto::LINK_STATE_PENDING_DELETE) {
              LOG_WARNING("DLT645 批量自动启动跳过待删除链路: conn_name={}", connName);
              continue;
            }

            startMqttSubscribeLocked(connName, link);
            startPollingLocked(connName, link);
            startDataCenterSubscribeLocked(connName, link);
            link->state = DLT645Proto::LINK_STATE_RUNNING;
            link->lastError.clear();
            handledConnNames.insert(connName);
            ++startedCount;
            LOG_INFO("DLT645 批量自动启动连接成功: conn_name={}, meter_addr={}", connName, link->config.meter_addr());
          }

          if (startedCount > 0) {
            archiveRefCountByKey_[archiveKey] = startedCount;
            LOG_INFO("DLT645 批量档案引用建立成功: meter_addr={}, 引用计数={}, 档案已存在={}",
                     group.links.front()->config.meter_addr(),
                     startedCount,
                     archiveAlreadyExists ? "是" : "否");
          } else {
            archiveRefCountByKey_.erase(archiveKey);
            LOG_WARNING("DLT645 批量档案下发后未能启动任何链路，已回滚本地引用计数: meter_addr={}",
                        group.links.front()->config.meter_addr());
          }
        }
      } else {
        for (const auto &archiveKey : batchGroupOrder) {
          auto groupIt = batchGroupsByKey.find(archiveKey);
          if (groupIt == batchGroupsByKey.end()) {
            continue;
          }

          const auto &group = groupIt->second;
          archiveRefCountByKey_.erase(archiveKey);
          for (size_t i = 0; i < group.connNames.size(); ++i) {
            const auto &connName = group.connNames[i];
            const auto &link = group.links[i];
            auto liveIt = linksByName_.find(connName);
            if (liveIt == linksByName_.end() || liveIt->second != link) {
              continue;
            }
            link->lastError = batchStatus.error_message();
            handledConnNames.insert(connName);
          }

          const auto &repConnName = group.connNames.front();
          const auto &repLink = group.links.front();
          auto repIt = linksByName_.find(repConnName);
          if (repIt != linksByName_.end() && repIt->second == repLink) {
            launchArchiveRetryLocked(repConnName, repLink, archiveKey);
            LOG_WARNING("DLT645 批量档案下发失败，已切换到后台重试: conn_name={}, meter_addr={}, 原因={}",
                        repConnName,
                        repLink->config.meter_addr(),
                        batchStatus.error_message());
          }
        }
      }
    }

    archiveStateCv_.notify_all();
    if (batchStatus.ok()) {
      LOG_INFO("DLT645 批量档案下发完成: 触发来源={}, 档案数={}, 档案已存在={}",
               trigger,
               requestLinks.size(),
               archiveAlreadyExists ? "是" : "否");
    } else {
      LOG_WARNING("DLT645 批量档案下发失败: 触发来源={}, 档案数={}, 原因={}",
                  trigger,
                  requestLinks.size(),
                  batchStatus.error_message());
    }
  }

  for (const auto &connName : connNames) {
    if (handledConnNames.contains(connName)) {
      continue;
    }
    (void)maybeAutoStartLink(connName, trigger);
  }
}

void LinkManager::TryAutoStartReadyLinks(std::string_view trigger) {
  autoStartEligibleLinks(trigger);
}

void LinkManager::stopArchiveRetryLocked(LinkRuntime *link, std::jthread *outThread) {
  if (outThread != nullptr) {
    *outThread = std::jthread();
  }
  if (link == nullptr) {
    return;
  }
  if (link->archiveRetryThread.joinable()) {
    link->archiveRetryThread.request_stop();
    if (outThread != nullptr) {
      *outThread = std::move(link->archiveRetryThread);
    }
  }
  link->archiveRetrying = false;
}

void LinkManager::releaseArchiveRefOnStartAbort(const std::string &connName, const std::shared_ptr<LinkRuntime> &link, const std::string &archiveKey) {
  if (!link || archiveKey.empty()) {
    return;
  }

  bool needDeleteArchive = false;
  {
    std::unique_lock<std::mutex> lock(mu_);
    while (true) {
      archiveStateCv_.wait(lock, [this, &archiveKey]() {
        return archiveAddInFlightByKey_.count(archiveKey) == 0 &&
            archiveDelInFlightByKey_.count(archiveKey) == 0;
      });
      auto it = archiveRefCountByKey_.find(archiveKey);
      if (it == archiveRefCountByKey_.end()) {
        LOG_WARNING("DLT645 启动连接功能回滚未找到档案引用: conn_name={}, meter_addr={}", connName, link->config.meter_addr());
        break;
      }
      if (it->second > 1) {
        it->second -= 1;
        LOG_INFO("DLT645 启动连接功能回滚已归还档案引用: conn_name={}, meter_addr={}, 剩余引用计数={}", connName, link->config.meter_addr(), it->second);
        break;
      }
      archiveDelInFlightByKey_.insert(archiveKey);
      needDeleteArchive = true;
      LOG_INFO("DLT645 启动连接功能回滚进入档案删除阶段: conn_name={}, meter_addr={}", connName, link->config.meter_addr());
      break;
    }
  }
  if (!needDeleteArchive) {
    return;
  }

  auto delStatus = sendDelSlaveNode(link.get());
  {
    std::lock_guard<std::mutex> lock(mu_);
    archiveDelInFlightByKey_.erase(archiveKey);
    auto it = archiveRefCountByKey_.find(archiveKey);
    if (it != archiveRefCountByKey_.end()) {
      if (it->second > 0) {
        it->second -= 1;
      }
      const uint32_t leftRef = it->second;
      if (leftRef == 0) {
        archiveRefCountByKey_.erase(it);
      }
      LOG_INFO("DLT645 启动连接功能回滚更新档案引用状态: conn_name={}, meter_addr={}, 剩余引用计数={}", connName, link->config.meter_addr(), leftRef);
    } else {
      LOG_WARNING("DLT645 启动连接功能回滚未找到档案引用状态: conn_name={}, meter_addr={}", connName, link->config.meter_addr());
    }
  }
  archiveStateCv_.notify_all();
  if (!delStatus.ok()) {
    LOG_WARNING("DLT645 启动连接功能回滚档案删除失败: conn_name={}, 原因={}", connName, delStatus.error_message());
  }
}

void LinkManager::launchArchiveRetryLocked(const std::string &connName, const std::shared_ptr<LinkRuntime> &link, const std::string &archiveKey) {
  if (!link) {
    return;
  }
  if (link->archiveRetrying) {
    LOG_INFO("DLT645 启动连接功能已在后台重试档案添加: conn_name={}", connName);
    return;
  }
  link->archiveRetrying = true;
  link->archiveRetryThread = ModuleManager::StartModuleThread(
      moduleName_,
      [this, connName, link, archiveKey](std::stop_token st) {
        runArchiveRetryLoop(connName, link, archiveKey, st);
      });
  LOG_INFO("DLT645 启动连接功能已进入档案后台重试: conn_name={}, 重试间隔=5000ms", connName);
}

void LinkManager::runArchiveRetryLoop(std::string connName, std::shared_ptr<LinkRuntime> link, std::string archiveKey, std::stop_token st) {
  constexpr auto kRetryInterval = std::chrono::seconds(5);
  size_t retryCount = 0;
  while (!st.stop_requested()) {
    if (!sleepWithStop(st, kRetryInterval)) {
      break;
    }
    ++retryCount;
    LOG_INFO("DLT645 启动连接功能开始重试档案添加: conn_name={}, 第{}次重试", connName, retryCount);

    bool holdArchiveRef = false;
    bool needAddArchive = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = linksByName_.find(connName);
      if (it == linksByName_.end() || it->second.get() != link.get()) {
        break;
      }
      if (link->state == DLT645Proto::LINK_STATE_RUNNING) {
        link->archiveRetrying = false;
        LOG_INFO("DLT645 档案后台重试结束: conn_name={}, 原因=连接功能已在运行", connName);
        return;
      }
      if (link->state == DLT645Proto::LINK_STATE_PENDING_DELETE) {
        link->archiveRetrying = false;
        LOG_INFO("DLT645 档案后台重试结束: conn_name={}, 原因=连接处于待删除状态", connName);
        return;
      }
      auto refIt = archiveRefCountByKey_.find(archiveKey);
      if (refIt != archiveRefCountByKey_.end() && refIt->second > 0) {
        refIt->second += 1;
        holdArchiveRef = true;
        LOG_INFO("DLT645 启动连接功能后台重试复用档案引用: conn_name={}, meter_addr={}, 引用计数={}", connName, link->config.meter_addr(), refIt->second);
      } else if (archiveAddInFlightByKey_.count(archiveKey) == 0 && archiveDelInFlightByKey_.count(archiveKey) == 0) {
        archiveAddInFlightByKey_.insert(archiveKey);
        needAddArchive = true;
        LOG_INFO("DLT645 启动连接功能后台重试获取档案添加资格: conn_name={}, meter_addr={}", connName, link->config.meter_addr());
      } else {
        LOG_INFO("DLT645 启动连接功能后台重试暂不发起档案添加: conn_name={}, meter_addr={}, 原因=同地址档案操作进行中", connName, link->config.meter_addr());
      }
    }

    if (st.stop_requested()) {
      if (holdArchiveRef) {
        releaseArchiveRefOnStartAbort(connName, link, archiveKey);
      }
      break;
    }
    if (!holdArchiveRef && !needAddArchive) {
      continue;
    }

    grpc::Status status = grpc::Status::OK;
    if (needAddArchive) {
      bool archiveAlreadyExists = false;
      status = sendAddSlaveNode(link.get(), &archiveAlreadyExists);
      {
        std::lock_guard<std::mutex> lock(mu_);
        archiveAddInFlightByKey_.erase(archiveKey);
        if (status.ok()) {
          archiveRefCountByKey_[archiveKey] = 1;
          holdArchiveRef = true;
          LOG_INFO("DLT645 启动连接功能后台重试建立档案引用成功: conn_name={}, meter_addr={}, 引用计数=1", connName, link->config.meter_addr());
        } else {
          archiveRefCountByKey_.erase(archiveKey);
          link->lastError = status.error_message();
        }
      }
      archiveStateCv_.notify_all();
      if (!status.ok()) {
        LOG_WARNING("DLT645 启动连接功能后台重试档案添加失败: conn_name={}, 第{}次重试, 原因={}, 5秒后继续重试", connName, retryCount, status.error_message());
        continue;
      }
      if (archiveAlreadyExists) {
        LOG_INFO("DLT645 启动连接功能后台重试识别到档案已存在: conn_name={}, 停止继续重试并恢复连接功能", connName);
      }
    }

    bool started = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = linksByName_.find(connName);
      if (!st.stop_requested() && it != linksByName_.end() && it->second.get() == link.get() &&
          link->state != DLT645Proto::LINK_STATE_PENDING_DELETE) {
        startPollingLocked(connName, link);
        startDataCenterSubscribeLocked(connName, link);
        link->state = DLT645Proto::LINK_STATE_RUNNING;
        link->lastError.clear();
        link->archiveRetrying = false;
        started = true;
      } else {
        link->archiveRetrying = false;
      }
    }
    if (started) {
      LOG_INFO("DLT645 启动连接功能后台重试成功并已启动连接功能: conn_name={}, 第{}次重试", connName, retryCount);
      return;
    }
    if (holdArchiveRef) {
      releaseArchiveRefOnStartAbort(connName, link, archiveKey);
    }
    break;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it != linksByName_.end() && it->second.get() == link.get()) {
      it->second->archiveRetrying = false;
    }
  }
  LOG_INFO("DLT645 档案后台重试结束: conn_name={}", connName);
}

void LinkManager::startPollingLocked(const std::string &connName, const std::shared_ptr<LinkRuntime> &link) {
  if (!link) {
    return;
  }
  stopPollingLocked(link.get());

  const auto roundInterval = std::chrono::milliseconds(link->config.poll_interval_ms());
  const auto itemInterval = std::chrono::milliseconds(link->config.poll_item_interval_ms());
  LOG_INFO("DLT645 启动轮询线程: conn_name={}, 整轮间隔={}ms, 单次收发间隔={}ms", connName, link->config.poll_interval_ms(), link->config.poll_item_interval_ms());
  link->pollThread = ModuleManager::StartModuleThread(
      moduleName_,
      [this, connName, link, roundInterval, itemInterval](std::stop_token st) {
        bool needGapBeforeNextRequest = false;
        auto waitBeforeNextRequest = [&needGapBeforeNextRequest, itemInterval, st]() {
          if (!needGapBeforeNextRequest || itemInterval <= std::chrono::milliseconds::zero()) {
            return !st.stop_requested();
          }
          return sleepWithStop(st, itemInterval);
        };
        while (!st.stop_requested()) {
          const auto &blocks = link->pointTable.Blocks();
          for (const auto &block : blocks) {
            if (st.stop_requested()) {
              break;
            }
            if (!waitBeforeNextRequest()) {
              break;
            }
            std::vector<uint8_t> di = encodeDiBytes(block.diBytes, link->config);
            std::vector<uint8_t> data = di;
            addOffset33(&data);
            auto frame = buildFrame(encodeAddress(link->config.meter_addr()), kReadControl, data);
            LOG_INFO("DLT645 发送数据块读请求: conn_name={}, block_di={}, frame={}", connName, block.diText, formatHex(frame));

            std::string payloadBase64;
            int32_t status = 0;
            auto sendStatus = sendMonitorRequest(link.get(), frame, &payloadBase64, &status);
            needGapBeforeNextRequest = true;
            if (!sendStatus.ok()) {
              LOG_WARNING("DLT645 数据块读请求失败: conn_name={}, block_di={}, 原因={}", connName, block.diText, sendStatus.error_message());
              continue;
            }
            if (status != 0) {
              LOG_WARNING("DLT645 数据块读请求返回失败: conn_name={}, block_di={}, 状态码={}", connName, block.diText, status);
              continue;
            }
            std::string error;
            sendStatus = handleMonitorResponse(link.get(), payloadBase64, block, &error);
            if (!sendStatus.ok()) {
              LOG_WARNING("DLT645 数据块解析响应失败: conn_name={}, block_di={}, 原因={}", connName, block.diText, error);
              continue;
            }
          }

          if (st.stop_requested()) {
            break;
          }
          const auto points = link->pointTable.Points();
          const auto &blockTags = link->pointTable.BlockTags();
          for (const auto &point : points) {
            if (st.stop_requested()) {
              break;
            }
            if (point.access == DLT645Proto::ACCESS_WRITE_ONLY) {
              continue;
            }
            if (blockTags.find(point.tag) != blockTags.end()) {
              continue;
            }
            if (!waitBeforeNextRequest()) {
              break;
            }
            std::vector<uint8_t> di = encodeDi(point, link->config);
            std::vector<uint8_t> data = di;
            addOffset33(&data);
            auto frame = buildFrame(encodeAddress(link->config.meter_addr()), kReadControl, data);
            LOG_INFO("DLT645 发送读请求: conn_name={}, tag={}, frame={}", connName, point.tag, formatHex(frame));

            std::string payloadBase64;
            int32_t status = 0;
            auto sendStatus = sendMonitorRequest(link.get(), frame, &payloadBase64, &status);
            needGapBeforeNextRequest = true;
            if (!sendStatus.ok()) {
              LOG_WARNING("DLT645 读请求失败: conn_name={}, tag={}, 原因={}", connName, point.tag, sendStatus.error_message());
              continue;
            }
            if (status != 0) {
              LOG_WARNING("DLT645 读请求返回失败: conn_name={}, tag={}, 状态码={}", connName, point.tag, status);
              continue;
            }
            std::string error;
            sendStatus = handleMonitorResponse(link.get(), payloadBase64, point, &error);
            if (!sendStatus.ok()) {
              LOG_WARNING("DLT645 解析响应失败: conn_name={}, tag={}, 原因={}", connName, point.tag, error);
              continue;
            }
          }
          if (st.stop_requested()) {
            break;
          }
          if (!sleepWithStop(st, roundInterval)) {
            break;
          }
          needGapBeforeNextRequest = false;
        }
      });
}

void LinkManager::stopPollingLocked(LinkRuntime *link) {
  if (link == nullptr) {
    return;
  }
  if (link->pollThread.joinable()) {
    link->pollThread.request_stop();
    link->pollThread.join();
  }
}

void LinkManager::startMqttSubscribeLocked(const std::string &connName, const std::shared_ptr<LinkRuntime> &link) {
  if (!link) {
    return;
  }
  stopMqttSubscribeLocked(link.get());

  std::vector<MQTTManagerProto::TopicFilter> topics;
  const auto monitorResp = makeMonitorResponseTopic(link->config);
  if (!monitorResp.empty()) {
    MQTTManagerProto::TopicFilter filter;
    filter.set_topic(monitorResp);
    filter.set_qos(kDefaultQos);
    topics.push_back(filter);
  }
  const auto addResp = makeAddSlaveResponseTopic(link->config.comm_mode());
  if (!addResp.empty() && addResp != monitorResp) {
    MQTTManagerProto::TopicFilter filter;
    filter.set_topic(addResp);
    filter.set_qos(kDefaultQos);
    topics.push_back(filter);
  }
  const auto delResp = makeDelSlaveResponseTopic(link->config.comm_mode());
  if (!delResp.empty() && delResp != monitorResp && delResp != addResp) {
    MQTTManagerProto::TopicFilter filter;
    filter.set_topic(delResp);
    filter.set_qos(kDefaultQos);
    topics.push_back(filter);
  }
  if (topics.empty()) {
    return;
  }

  link->mqttSubscribeContext = std::make_shared<grpc::ClientContext>();
  auto ctx = link->mqttSubscribeContext;
  link->mqttSubscribeThread = ModuleManager::StartModuleThread(
      moduleName_,
      [this, connName, ctx, topics](std::stop_token st) {
        std::stop_callback cb(st, [&ctx]() { ctx->TryCancel(); });
        auto reader = mqttClient_.Subscribe(ctx.get(), topics);
        if (!reader) {
          LOG_ERROR("DLT645 创建 MQTT 订阅失败: conn_name={}", connName);
          return;
        }
        MQTTManagerProto::SubscribeResponse response;
        while (reader->Read(&response)) {
          const auto payload = response.payload();
          if (payload.empty()) {
            continue;
          }
          LOG_INFO("DLT645 收到 MQTT 响应: conn_name={}, topic={}, payload={}", connName, response.topic(), payload);
          boost::system::error_code ec;
          auto parsed = boost::json::parse(payload, ec);
          if (ec || !parsed.is_object()) {
            LOG_WARNING("DLT645 MQTT 响应解析失败: conn_name={}, topic={}, 原因={}", connName, response.topic(), ec.message());
            continue;
          }
          const auto &obj = parsed.as_object();
          auto it = obj.find("token");
          if (it == obj.end()) {
            LOG_WARNING("DLT645 MQTT 响应缺少 token: conn_name={}, topic={}", connName, response.topic());
            continue;
          }
          std::string token;
          if (!parseTokenString(it->value(), &token)) {
            LOG_WARNING("DLT645 MQTT 响应 token 非法，要求为非空字符串: conn_name={}, topic={}", connName, response.topic());
            continue;
          }

          std::shared_ptr<LinkRuntime> link;
          {
            std::lock_guard<std::mutex> lock(mu_);
            auto linkIt = linksByName_.find(connName);
            if (linkIt == linksByName_.end()) {
              continue;
            }
            link = linkIt->second;
          }
          if (!link) {
            continue;
          }
          std::shared_ptr<PendingResponse> pending;
          {
            std::lock_guard<std::mutex> lock(link->pendingMutex);
            auto pendIt = link->pending.find(token);
            if (pendIt != link->pending.end()) {
              pending = pendIt->second;
            }
          }
          if (!pending) {
            LOG_DEBUG("DLT645 MQTT 响应未匹配请求: conn_name={}, token={}", connName, token);
            continue;
          }

          int32_t status = 0;
          auto statusIt = obj.find("status");
          if (statusIt != obj.end()) {
            if (statusIt->value().is_int64()) {
              status = static_cast<int32_t>(statusIt->value().as_int64());
            } else if (statusIt->value().is_uint64()) {
              status = static_cast<int32_t>(statusIt->value().as_uint64());
            } else if (statusIt->value().is_string()) {
              status = std::atoi(statusIt->value().as_string().c_str());
            }
          }

          std::string payloadBase64;
          auto dataIt = obj.find("data");
          if (dataIt != obj.end() && dataIt->value().is_string()) {
            payloadBase64 = dataIt->value().as_string().c_str();
          }

          {
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->done = true;
            pending->ok = status == 0;
            pending->status = status;
            pending->payloadBase64 = payloadBase64;
          }
          pending->cv.notify_all();
        }
      });
}

void LinkManager::stopMqttSubscribeLocked(LinkRuntime *link) {
  if (link == nullptr) {
    return;
  }
  if (link->mqttSubscribeThread.joinable()) {
    link->mqttSubscribeThread.request_stop();
    link->mqttSubscribeThread.join();
  }
  link->mqttSubscribeContext.reset();
  {
    std::lock_guard<std::mutex> lock(link->pendingMutex);
    link->pending.clear();
  }
}

void LinkManager::startDataCenterSubscribeLocked(const std::string &connName, const std::shared_ptr<LinkRuntime> &link) {
  if (!link) {
    return;
  }
  stopDataCenterSubscribeLocked(link.get());

  std::vector<PointTable::Point> points;
  for (const auto &point : link->pointTable.Points()) {
    if (point.access == DLT645Proto::ACCESS_WRITE_ONLY ||
        point.access == DLT645Proto::ACCESS_READ_WRITE) {
      points.push_back(point);
    }
  }
  if (points.empty()) {
    return;
  }
  std::vector<std::string> tags;
  tags.reserve(points.size());
  std::unordered_map<std::string, PointTable::Point> pointByTag;
  for (const auto &point : points) {
    tags.push_back(point.tag);
    pointByTag.emplace(point.tag, point);
  }

  link->dcSubscribeContext = std::make_shared<grpc::ClientContext>();
  auto ctx = link->dcSubscribeContext;
  auto connId = link->connId;
  link->dcSubscribeThread = ModuleManager::StartModuleThread(
      moduleName_,
      [this, connName, ctx, connId, tags, pointByTag, link](std::stop_token st) {
        std::stop_callback cb(st, [&ctx]() { ctx->TryCancel(); });
        auto reader = dataCenter_.Subscribe(ctx.get(), connId, tags, false);
        if (!reader) {
          LOG_ERROR("DLT645 创建 DataCenter 订阅失败: conn_name={}, conn_id={}", connName, connId);
          return;
        }
        DataCenterProto::PointUpdate update;
        while (reader->Read(&update)) {
          auto it = pointByTag.find(update.dst_tag());
          if (it == pointByTag.end()) {
            continue;
          }
          std::string error;
          auto payload = encodeData(it->second, update.value(), &error);
          if (payload.empty()) {
            LOG_WARNING("DLT645 写入编码失败: conn_name={}, tag={}, 原因={}", connName, update.dst_tag(), error);
            continue;
          }

          std::vector<uint8_t> data = encodeDi(it->second, link->config);
          data.insert(data.end(), 4, 0x00);
          data.insert(data.end(), 4, 0x00);
          data.insert(data.end(), payload.begin(), payload.end());
          addOffset33(&data);

          auto addr = encodeAddress(link->config.meter_addr());
          std::vector<uint8_t> frame;
          frame.reserve(32 + data.size());
          frame = buildFrame(addr, kWriteControl, data);
          LOG_INFO("DLT645 发送写请求: conn_name={}, tag={}, frame={}", connName, update.dst_tag(), formatHex(frame));

          auto sendStatus = sendWriteRequest(link.get(), frame);
          if (!sendStatus.ok()) {
            LOG_WARNING("DLT645 写请求失败: conn_name={}, tag={}, 原因={}", connName, update.dst_tag(), sendStatus.error_message());
          }
        }
      });
}

void LinkManager::stopDataCenterSubscribeLocked(LinkRuntime *link) {
  if (link == nullptr) {
    return;
  }
  if (link->dcSubscribeThread.joinable()) {
    link->dcSubscribeThread.request_stop();
    link->dcSubscribeThread.join();
  }
  link->dcSubscribeContext.reset();
}

grpc::Status LinkManager::runLoraSerialized(LinkRuntime *link, const char *operation, const std::string &topic, const std::function<grpc::Status()> &action) {
  if (link == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "链路为空");
  }
  if (!action) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "操作为空");
  }
  if (link->config.comm_mode() != DLT645Proto::COMM_MODE_LORA) {
    return action();
  }

  const char *opText = operation != nullptr ? operation : "未知操作";
  LOG_INFO("DLT645 LoRa 串行通道等待: conn_name={}, 操作={}, topic={}", link->config.conn_name(), opText, topic);
  std::unique_lock<std::mutex> lock(loraRequestMutex_);
  LOG_INFO("DLT645 LoRa 串行通道开始执行: conn_name={}, 操作={}, topic={}", link->config.conn_name(), opText, topic);
  auto status = action();
  if (status.ok()) {
    LOG_INFO("DLT645 LoRa 串行通道执行完成: conn_name={}, 操作={}, topic={}", link->config.conn_name(), opText, topic);
  } else {
    LOG_WARNING("DLT645 LoRa 串行通道执行失败: conn_name={}, 操作={}, topic={}, 原因={}", link->config.conn_name(), opText, topic, status.error_message());
  }
  return status;
}

grpc::Status LinkManager::sendAddSlaveNode(LinkRuntime *link, bool *outArchiveExists) {
  if (link == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "链路为空");
  }
  return sendAddSlaveNodes({link}, outArchiveExists);
}

grpc::Status LinkManager::sendAddSlaveNodes(const std::vector<LinkRuntime *> &links, bool *outArchiveExists) {
  if (links.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "链路列表为空");
  }
  if (outArchiveExists != nullptr) {
    *outArchiveExists = false;
  }
  auto *firstLink = links.front();
  if (firstLink == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "链路为空");
  }

  const auto topic = makeAddSlaveRequestTopic(firstLink->config.comm_mode());
  const auto respTopic = makeAddSlaveResponseTopic(firstLink->config.comm_mode());
  if (topic.empty() || respTopic.empty()) {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "当前通信方式不支持档案管理");
  }

  boost::json::object obj;
  obj["token"] = nextToken();
  obj["timestamp"] = formatTimestamp();
  obj["prio"] = 1;
  boost::json::array body;
  std::unordered_set<std::string> seenAddrs;
  uint32_t timeoutMs = 0;
  for (auto *link : links) {
    if (link == nullptr) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "链路为空");
    }
    if (link->config.comm_mode() != firstLink->config.comm_mode()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "批量档案下发要求通信方式一致");
    }
    timeoutMs = std::max(timeoutMs, link->config.request_timeout_ms());
    if (!seenAddrs.insert(link->config.meter_addr()).second) {
      continue;
    }

    boost::json::object item;
    item["addr"] = link->config.meter_addr();
    item["proType"] = 2;
    body.push_back(std::move(item));
  }
  if (body.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "档案列表为空");
  }
  obj["body"] = std::move(body);

  const auto json = boost::json::serialize(obj);
  LOG_INFO("DLT645 发送档案添加请求: conn_name={}, topic={}, response_topic={}, 档案数={}, payload={}",
           firstLink->config.conn_name(),
           topic,
           respTopic,
           seenAddrs.size(),
           json);

  std::string responsePayload;
  std::string error;
  auto statusRet = runLoraSerialized(
      firstLink,
      "档案添加请求",
      topic,
      [this, &topic, &respTopic, &json, timeoutMs, &responsePayload, &error]() {
        return mqttClient_.RequestAndWait(topic, respTopic, json, timeoutMs, 0, 0, "token", &responsePayload, &error);
      });
  if (!statusRet.ok()) {
    LOG_ERROR("DLT645 档案添加请求失败: conn_name={}, 原因={}", firstLink->config.conn_name(), error);
    return statusRet;
  }
  if (responsePayload.empty()) {
    LOG_ERROR("DLT645 档案添加响应为空: conn_name={}, topic={}", firstLink->config.conn_name(), respTopic);
    return grpc::Status(grpc::StatusCode::INTERNAL, "档案添加响应为空");
  }
  LOG_INFO("DLT645 收到档案添加响应: conn_name={}, topic={}, payload={}", firstLink->config.conn_name(), respTopic, responsePayload);

  boost::system::error_code ec;
  auto parsed = boost::json::parse(responsePayload, ec);
  if (ec || !parsed.is_object()) {
    LOG_ERROR("DLT645 档案添加响应解析失败: conn_name={}, 原因={}", firstLink->config.conn_name(), ec.message());
    return grpc::Status(grpc::StatusCode::INTERNAL, "档案添加响应解析失败");
  }
  const auto &respObj = parsed.as_object();
  auto statusIt = respObj.find("status");
  if (statusIt == respObj.end()) {
    LOG_ERROR("DLT645 档案添加响应缺少状态字段: conn_name={}, topic={}", firstLink->config.conn_name(), respTopic);
    return grpc::Status(grpc::StatusCode::INTERNAL, "档案添加响应缺少状态字段");
  }
  const auto statusResult = parseArchiveAddStatus(statusIt->value());
  if (!statusResult.ok) {
    LOG_ERROR("DLT645 档案添加响应状态类型异常: conn_name={}, topic={}", firstLink->config.conn_name(), respTopic);
    return grpc::Status(grpc::StatusCode::INTERNAL, "档案添加响应状态类型异常");
  }
  if (statusResult.archiveExists) {
    if (outArchiveExists != nullptr) {
      *outArchiveExists = true;
    }
    LOG_INFO(
        "DLT645 档案添加响应表明档案已存在: conn_name={}, topic={}, status=2, 跳过后续档案重试并继续后续抄表流程",
        firstLink->config.conn_name(),
        respTopic);
    return grpc::Status::OK;
  }
  const int32_t status = statusResult.status;
  if (status != 0) {
    const auto reason = loraStatusToMessage(status);
    LOG_ERROR("DLT645 档案添加响应失败: conn_name={}, topic={}, status={}, 描述={}", firstLink->config.conn_name(), respTopic, status, reason);
    return grpc::Status(grpc::StatusCode::INTERNAL, std::format("档案添加失败: {}", reason));
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::sendDelSlaveNode(LinkRuntime *link) {
  if (link == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "链路为空");
  }
  const auto topic = makeDelSlaveRequestTopic(link->config.comm_mode());
  const auto respTopic = makeDelSlaveResponseTopic(link->config.comm_mode());
  if (topic.empty() || respTopic.empty()) {
    return grpc::Status::OK;
  }

  boost::json::object obj;
  obj["token"] = nextToken();
  obj["timestamp"] = formatTimestamp();
  boost::json::array body;
  boost::json::object item;
  item["addr"] = link->config.meter_addr();
  body.push_back(item);
  obj["body"] = std::move(body);

  const auto json = boost::json::serialize(obj);
  LOG_INFO("DLT645 发送档案删除请求: conn_name={}, topic={}, response_topic={}, payload={}", link->config.conn_name(), topic, respTopic, json);

  std::string responsePayload;
  std::string error;
  auto statusRet = runLoraSerialized(
      link,
      "档案删除请求",
      topic,
      [this, &topic, &respTopic, &json, &link, &responsePayload, &error]() {
        return mqttClient_.RequestAndWait(topic, respTopic, json, link->config.request_timeout_ms(), 0, 0, "token", &responsePayload, &error);
      });
  if (!statusRet.ok()) {
    LOG_ERROR("DLT645 档案删除请求失败: conn_name={}, 原因={}", link->config.conn_name(), error);
    return statusRet;
  }
  if (responsePayload.empty()) {
    LOG_ERROR("DLT645 档案删除响应为空: conn_name={}, topic={}", link->config.conn_name(), respTopic);
    return grpc::Status(grpc::StatusCode::INTERNAL, "档案删除响应为空");
  }
  LOG_INFO("DLT645 收到档案删除响应: conn_name={}, topic={}, payload={}", link->config.conn_name(), respTopic, responsePayload);

  boost::system::error_code ec;
  auto parsed = boost::json::parse(responsePayload, ec);
  if (ec || !parsed.is_object()) {
    LOG_ERROR("DLT645 档案删除响应解析失败: conn_name={}, 原因={}", link->config.conn_name(), ec.message());
    return grpc::Status(grpc::StatusCode::INTERNAL, "档案删除响应解析失败");
  }
  const auto &respObj = parsed.as_object();
  auto statusIt = respObj.find("status");
  if (statusIt == respObj.end()) {
    LOG_ERROR("DLT645 档案删除响应缺少状态字段: conn_name={}, topic={}", link->config.conn_name(), respTopic);
    return grpc::Status(grpc::StatusCode::INTERNAL, "档案删除响应缺少状态字段");
  }
  bool parsedStatus = false;
  const int32_t status = parseStatusCode(statusIt->value(), &parsedStatus);
  if (!parsedStatus) {
    LOG_ERROR("DLT645 档案删除响应状态类型异常: conn_name={}, topic={}", link->config.conn_name(), respTopic);
    return grpc::Status(grpc::StatusCode::INTERNAL, "档案删除响应状态类型异常");
  }
  if (status != 0) {
    const auto reason = loraStatusToMessage(status);
    LOG_ERROR("DLT645 档案删除响应失败: conn_name={}, topic={}, status={}, 描述={}", link->config.conn_name(), respTopic, status, reason);
    return grpc::Status(grpc::StatusCode::INTERNAL, std::format("档案删除失败: {}", reason));
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::sendMonitorRequest(LinkRuntime *link, const std::vector<uint8_t> &frame, std::string *outPayloadBase64, int32_t *outStatus) {
  if (link == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "链路为空");
  }
  const auto topic = makeMonitorRequestTopic(link->config);
  const auto respTopic = makeMonitorResponseTopic(link->config);
  if (topic.empty() || respTopic.empty()) {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "当前通信方式不支持点抄");
  }

  boost::json::object obj;
  obj["token"] = nextToken();
  obj["timestamp"] = formatTimestamp();
  obj["prio"] = 1;
  obj["data"] = base64Encode(frame);
  if (link->config.comm_mode() == DLT645Proto::COMM_MODE_SERIAL) {
    obj["port"] = link->config.serial_port();
    obj["prm"] = 1;
    obj["byteTimeout"] = link->config.serial_byte_timeout_ms();
    obj["frameTimeout"] = link->config.serial_frame_timeout_ms();
    obj["taskTimeout"] = link->config.request_timeout_ms();
    obj["estSize"] = link->config.serial_est_size();
    boost::json::object param;
    param["baudRate"] = link->config.serial_baud_rate();
    param["byteSize"] = link->config.serial_data_bits();
    param["parity"] = serialParityToText(link->config.serial_parity());
    param["stopBits"] = serialStopBitsToNumber(link->config.serial_stop_bits());
    obj["param"] = std::move(param);
  } else {
    obj["acqAddr"] = link->config.meter_addr();
  }

  return sendMonitorRequest(link, topic, respTopic, obj, link->config.request_timeout_ms(), outStatus, outPayloadBase64);
}

grpc::Status LinkManager::sendWriteRequest(LinkRuntime *link, const std::vector<uint8_t> &frame) {
  int32_t status = 0;
  std::string payload;
  auto ret = sendMonitorRequest(link, frame, &payload, &status);
  if (!ret.ok()) {
    return ret;
  }
  if (status != 0) {
    return grpc::Status(grpc::StatusCode::INTERNAL, "写入响应失败");
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::sendMonitorRequest(LinkRuntime *link, const std::string &requestTopic, const std::string &responseTopic, const boost::json::object &obj, uint32_t timeoutMs, int32_t *outStatus, std::string *outPayloadBase64) {
  if (link == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "链路为空");
  }
  if (outStatus != nullptr) {
    *outStatus = -1;
  }
  if (outPayloadBase64 != nullptr) {
    outPayloadBase64->clear();
  }

  std::string error;
  auto json = boost::json::serialize(obj);
  LOG_INFO("DLT645 发送 MQTT 请求: conn_name={}, topic={}, response_topic={}, payload={}", link->config.conn_name(), requestTopic, responseTopic, json);

  std::string responsePayload;
  const auto waitMs = timeoutMs > 0 ? timeoutMs : kDefaultRequestTimeoutMs;
  auto status = runLoraSerialized(
      link,
      "点抄写请求",
      requestTopic,
      [this, &requestTopic, &responseTopic, &json, waitMs, &responsePayload, &error]() {
        return mqttClient_.RequestAndWait(requestTopic, responseTopic, json, waitMs, 0, 0, "token", &responsePayload, &error);
      });
  if (!status.ok()) {
    LOG_ERROR("DLT645 MQTT 请求响应失败: conn_name={}, topic={}, 原因={}", link->config.conn_name(), requestTopic, error);
    return status;
  }
  if (responsePayload.empty()) {
    LOG_ERROR("DLT645 MQTT 响应为空: conn_name={}, topic={}", link->config.conn_name(), responseTopic);
    return grpc::Status(grpc::StatusCode::INTERNAL, "MQTT 响应为空");
  }
  LOG_INFO("DLT645 收到 MQTT 响应: conn_name={}, topic={}, payload={}", link->config.conn_name(), responseTopic, responsePayload);

  boost::system::error_code ec;
  auto parsed = boost::json::parse(responsePayload, ec);
  if (ec || !parsed.is_object()) {
    LOG_ERROR("DLT645 MQTT 响应解析失败: conn_name={}, topic={}, 原因={}", link->config.conn_name(), responseTopic, ec.message());
    return grpc::Status(grpc::StatusCode::INTERNAL, "MQTT 响应解析失败");
  }
  const auto &respObj = parsed.as_object();
  int32_t statusCode = 0;
  auto statusIt = respObj.find("status");
  if (statusIt != respObj.end()) {
    bool parsedStatus = false;
    statusCode = parseStatusCode(statusIt->value(), &parsedStatus);
    if (!parsedStatus) {
      LOG_WARNING("DLT645 MQTT 响应状态类型异常: conn_name={}, topic={}", link->config.conn_name(), responseTopic);
    }
  } else {
    LOG_WARNING("DLT645 MQTT 响应缺少状态字段: conn_name={}, topic={}", link->config.conn_name(), responseTopic);
  }
  if (outStatus != nullptr) {
    *outStatus = statusCode;
  }

  std::string payloadBase64;
  auto dataIt = respObj.find("data");
  if (dataIt != respObj.end() && dataIt->value().is_string()) {
    payloadBase64 = dataIt->value().as_string().c_str();
  }
  if (outPayloadBase64 != nullptr) {
    *outPayloadBase64 = payloadBase64;
  }
  if (statusCode == 0 && payloadBase64.empty()) {
    LOG_ERROR("DLT645 MQTT 响应缺少数据: conn_name={}, topic={}", link->config.conn_name(), responseTopic);
    return grpc::Status(grpc::StatusCode::INTERNAL, "MQTT 响应缺少数据");
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::decodeAndPublish(LinkRuntime *link, const PointTable::Point &point, const std::vector<uint8_t> &payload, int64_t tsMs, bool trimRightSpace) {
  if (link == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "链路为空");
  }
  if (payload.size() < point.dataLen) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应数据长度不足");
  }
  bool allFF = true;
  for (size_t i = 0; i < point.dataLen; ++i) {
    if (payload[i] != 0xFF) {
      allFF = false;
      break;
    }
  }
  if (allFF) {
    LOG_WARNING("DLT645 数据域全 FF，判定设备未接入: conn_name={}, tag={}, payload={}", link->config.conn_name(), point.tag, formatHex(payload));
    if (point.type == DLT645Proto::DATA_TYPE_BOOL) {
      return dataCenter_.PublishBool(link->connId, point.tag, false, DataCenterProto::QUALITY_BAD, tsMs);
    }
    if (point.type == DLT645Proto::DATA_TYPE_STRING) {
      return dataCenter_.PublishString(link->connId, point.tag, "", DataCenterProto::QUALITY_BAD, tsMs);
    }
    return dataCenter_.PublishDouble(link->connId, point.tag, 0.0, DataCenterProto::QUALITY_BAD, tsMs);
  }

  if (point.type == DLT645Proto::DATA_TYPE_BOOL) {
    const bool value = payload[0] != 0;
    return dataCenter_.PublishBool(link->connId, point.tag, value, DataCenterProto::QUALITY_GOOD, tsMs);
  }

  if (point.type == DLT645Proto::DATA_TYPE_STRING) {
    std::string text(payload.begin(), payload.begin() + point.dataLen);
    if (trimRightSpace) {
      while (!text.empty() && text.back() == ' ') {
        text.pop_back();
      }
    }
    return dataCenter_.PublishString(link->connId, point.tag, text, DataCenterProto::QUALITY_GOOD, tsMs);
  }

  double raw = 0.0;
  if (point.type == DLT645Proto::DATA_TYPE_UINT16 && point.dataLen >= 2) {
    raw = static_cast<double>(payload[0] | (payload[1] << 8));
  } else if (point.type == DLT645Proto::DATA_TYPE_UINT32 && point.dataLen >= 4) {
    raw = static_cast<double>(payload[0] | (payload[1] << 8) | (payload[2] << 16) | (payload[3] << 24));
  } else if (point.type == DLT645Proto::DATA_TYPE_FLOAT && point.dataLen >= 4) {
    uint32_t temp = payload[0] |
        (payload[1] << 8) |
        (payload[2] << 16) |
        (payload[3] << 24);
    float f = 0.0f;
    std::memcpy(&f, &temp, sizeof(float));
    raw = static_cast<double>(f);
  } else if (point.type == DLT645Proto::DATA_TYPE_BCD) {
    std::string digits;
    digits.reserve(point.dataLen * 2);
    for (size_t i = 0; i < point.dataLen; ++i) {
      uint8_t b = payload[point.dataLen - 1 - i];
      uint8_t low = b & 0x0F;
      uint8_t high = (b >> 4) & 0x0F;
      if (low > 9 || high > 9) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "BCD 数据格式非法");
      }
      digits.push_back(static_cast<char>('0' + high));
      digits.push_back(static_cast<char>('0' + low));
    }
    try {
      raw = std::stod(digits);
    } catch (const std::exception &) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "BCD 数值解析失败");
    }
  } else {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "不支持的数据类型");
  }

  const double scale = point.scale == 0.0 ? 1.0 : point.scale;
  const double value = raw * scale + point.offset;
  if (point.deadband > 0) {
    auto lastIt = link->lastReportedByTag.find(point.tag);
    if (lastIt != link->lastReportedByTag.end()) {
      if (std::abs(value - lastIt->second) <= point.deadband) {
        LOG_DEBUG("DLT645 死区过滤: tag={}, value={}, last={}, deadband={}", point.tag, value, lastIt->second, point.deadband);
        return grpc::Status::OK;
      }
    }
  }
  auto status = dataCenter_.PublishDouble(link->connId, point.tag, value, DataCenterProto::QUALITY_GOOD, tsMs);
  if (status.ok()) {
    link->lastReportedByTag[point.tag] = value;
  }
  return status;
}

std::string LinkManager::makeMonitorRequestTopic(const DLT645Proto::LinkConfig &config) {
  const auto mode = config.comm_mode();
  if (mode == DLT645Proto::COMM_MODE_LORA) {
    return std::string(kAppName) + "/" + kAppTypeLora + "/JSON/action/request/monitorNode";
  }
  if (mode == DLT645Proto::COMM_MODE_CARRIER) {
    return std::string(kAppName) + "/" + kAppTypeCarrier + "/JSON/action/request/monitorNode";
  }
  if (mode == DLT645Proto::COMM_MODE_SERIAL && !config.serial_port().empty()) {
    return std::format("{}/{}/JSON/transparant/notification/{}/data", kAppName, kAppTypeUart, config.serial_port());
  }
  return "";
}

std::string LinkManager::makeMonitorResponseTopic(const DLT645Proto::LinkConfig &config) {
  const auto mode = config.comm_mode();
  if (mode == DLT645Proto::COMM_MODE_LORA) {
    return std::string(kAppTypeLora) + "/" + kAppName + "/JSON/action/response/monitorNode";
  }
  if (mode == DLT645Proto::COMM_MODE_CARRIER) {
    return std::string(kAppTypeCarrier) + "/" + kAppName + "/JSON/action/response/monitorNode";
  }
  if (mode == DLT645Proto::COMM_MODE_SERIAL && !config.serial_port().empty()) {
    return std::format("{}/{}/JSON/transparant/notification/{}/data", kAppTypeUart, kAppName, config.serial_port());
  }
  return "";
}

std::string LinkManager::makeAddSlaveRequestTopic(DLT645Proto::CommMode mode) {
  if (mode == DLT645Proto::COMM_MODE_LORA) {
    return std::string(kAppName) + "/" + kAppTypeLora + "/JSON/action/request/addslaveNode";
  }
  if (mode == DLT645Proto::COMM_MODE_CARRIER) {
    return std::string(kAppName) + "/" + kAppTypeCarrier + "/JSON/action/request/addslaveNode";
  }
  return "";
}

std::string LinkManager::makeAddSlaveResponseTopic(DLT645Proto::CommMode mode) {
  if (mode == DLT645Proto::COMM_MODE_LORA) {
    return std::string(kAppTypeLora) + "/" + kAppName + "/JSON/action/response/addslaveNode";
  }
  if (mode == DLT645Proto::COMM_MODE_CARRIER) {
    return std::string(kAppTypeCarrier) + "/" + kAppName + "/JSON/action/response/addslaveNode";
  }
  return "";
}

std::string LinkManager::makeDelSlaveRequestTopic(DLT645Proto::CommMode mode) {
  if (mode == DLT645Proto::COMM_MODE_LORA) {
    return std::string(kAppName) + "/" + kAppTypeLora + "/JSON/action/request/delslaveNode";
  }
  if (mode == DLT645Proto::COMM_MODE_CARRIER) {
    return std::string(kAppName) + "/" + kAppTypeCarrier + "/JSON/action/request/delslaveNode";
  }
  return "";
}

std::string LinkManager::makeDelSlaveResponseTopic(DLT645Proto::CommMode mode) {
  if (mode == DLT645Proto::COMM_MODE_LORA) {
    return std::string(kAppTypeLora) + "/" + kAppName + "/JSON/action/response/delslaveNode";
  }
  if (mode == DLT645Proto::COMM_MODE_CARRIER) {
    return std::string(kAppTypeCarrier) + "/" + kAppName + "/JSON/action/response/delslaveNode";
  }
  return "";
}

std::string LinkManager::formatHex(const std::vector<uint8_t> &data) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (uint8_t b : data) {
    oss << std::setw(2) << static_cast<int>(b);
  }
  return oss.str();
}

std::string LinkManager::formatTimestamp() {
  const auto ms = nowMs();
  const auto sec = ms / 1000;
  const auto milli = ms % 1000;
  std::time_t t = static_cast<std::time_t>(sec);
  std::tm tm = *std::gmtime(&t);
  return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}+0000", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, milli);
}

uint64_t LinkManager::nowMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count());
}

bool LinkManager::parseHexByte(const std::string &text, uint8_t *out) {
  if (out == nullptr || text.size() != 2 || !isHex(text)) {
    return false;
  }
  char *end = nullptr;
  auto value = std::strtoul(text.c_str(), &end, 16);
  if (end == nullptr || *end != '\0') {
    return false;
  }
  *out = static_cast<uint8_t>(value);
  return true;
}

bool LinkManager::decodeHexString(const std::string &text, std::vector<uint8_t> *out) {
  if (out == nullptr || text.size() % 2 != 0 || !isHex(text)) {
    return false;
  }
  out->clear();
  out->reserve(text.size() / 2);
  for (size_t i = 0; i < text.size(); i += 2) {
    uint8_t value = 0;
    if (!parseHexByte(text.substr(i, 2), &value)) {
      return false;
    }
    out->push_back(value);
  }
  return true;
}

std::vector<uint8_t> LinkManager::encodeBcd(const std::string &digits) {
  std::string data = digits;
  if (data.size() % 2 != 0) {
    data.insert(data.begin(), '0');
  }
  std::vector<uint8_t> out;
  out.reserve(data.size() / 2);
  for (size_t i = 0; i < data.size(); i += 2) {
    uint8_t high = static_cast<uint8_t>(data[i] - '0');
    uint8_t low = static_cast<uint8_t>(data[i + 1] - '0');
    out.push_back(static_cast<uint8_t>((high << 4) | low));
  }
  return out;
}

std::vector<uint8_t> LinkManager::encodeAddress(const std::string &addr) {
  std::vector<uint8_t> out;
  out.reserve(6);
  for (int i = 0; i < 6; ++i) {
    const auto part = addr.substr(i * 2, 2);
    uint8_t value = 0;
    parseHexByte(part, &value);
    out.push_back(value);
  }
  std::reverse(out.begin(), out.end());
  return out;
}

std::vector<uint8_t> LinkManager::encodeDiBytes(const std::array<uint8_t, 4> &diBytes, const DLT645Proto::LinkConfig &config) {
  std::vector<uint8_t> out(diBytes.begin(), diBytes.end());
  if (config.protocol_variant() == DLT645Proto::PROTOCOL_VARIANT_DLT645_PCD) {
    uint8_t deviceNo = 0;
    parseHexByte(config.device_no(), &deviceNo);
    out.push_back(deviceNo);
  }
  return out;
}

std::vector<uint8_t> LinkManager::encodeDi(const PointTable::Point &point, const DLT645Proto::LinkConfig &config) {
  return encodeDiBytes(point.diBytes, config);
}

std::vector<uint8_t> LinkManager::encodeData(const PointTable::Point &point, const DataCenterProto::PointValue &value, std::string *error) {
  std::vector<uint8_t> out;
  out.reserve(point.dataLen);

  if (point.type == DLT645Proto::DATA_TYPE_BOOL) {
    bool v = false;
    if (!pointValueToBool(value, &v)) {
      if (error != nullptr) {
        *error = "点值类型不匹配";
      }
      return {};
    }
    out.push_back(v ? 1 : 0);
    return out;
  }

  if (point.type == DLT645Proto::DATA_TYPE_STRING) {
    std::string text;
    if (!pointValueToString(value, &text)) {
      if (error != nullptr) {
        *error = "点值类型不匹配";
      }
      return {};
    }
    if (text.size() > point.dataLen) {
      text.resize(point.dataLen);
    }
    out.assign(text.begin(), text.end());
    while (out.size() < point.dataLen) {
      out.push_back(0);
    }
    return out;
  }

  double val = 0.0;
  if (!pointValueToDouble(value, &val)) {
    if (error != nullptr) {
      *error = "点值类型不匹配";
    }
    return {};
  }

  double raw = 0.0;
  if (!reverseScale(val, point.scale, point.offset, &raw)) {
    if (error != nullptr) {
      *error = "反向缩放失败";
    }
    return {};
  }

  if (point.type == DLT645Proto::DATA_TYPE_UINT16 && point.dataLen >= 2) {
    if (raw < 0 || raw > 65535) {
      if (error != nullptr) {
        *error = "UINT16 超出范围";
      }
      return {};
    }
    uint16_t v = static_cast<uint16_t>(std::llround(raw));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    return out;
  }
  if (point.type == DLT645Proto::DATA_TYPE_UINT32 && point.dataLen >= 4) {
    if (raw < 0 || raw > 4294967295.0) {
      if (error != nullptr) {
        *error = "UINT32 超出范围";
      }
      return {};
    }
    uint32_t v = static_cast<uint32_t>(std::llround(raw));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    return out;
  }
  if (point.type == DLT645Proto::DATA_TYPE_FLOAT && point.dataLen >= 4) {
    float f = static_cast<float>(raw);
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(float));
    out.push_back(static_cast<uint8_t>(u & 0xFF));
    out.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((u >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((u >> 24) & 0xFF));
    return out;
  }
  if (point.type == DLT645Proto::DATA_TYPE_BCD) {
    if (raw < 0) {
      if (error != nullptr) {
        *error = "BCD 不支持负数";
      }
      return {};
    }
    const auto digits = std::to_string(static_cast<uint64_t>(std::llround(raw)));
    auto bcd = encodeBcd(digits);
    if (bcd.size() > point.dataLen) {
      if (error != nullptr) {
        *error = "BCD 数据过长";
      }
      return {};
    }
    std::reverse(bcd.begin(), bcd.end());
    while (bcd.size() < point.dataLen) {
      bcd.push_back(0);
    }
    return bcd;
  }

  if (error != nullptr) {
    *error = "不支持的数据类型";
  }
  return {};
}

bool LinkManager::decodeFrame(const std::vector<uint8_t> &data, Frame *out, std::string *error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "响应为空";
    }
    return false;
  }
  if (data.size() < kMinFrameSize) {
    if (error != nullptr) {
      *error = "帧长度不足";
    }
    return false;
  }
  if (data.front() != kFrameStart || data.back() != kFrameEnd) {
    if (error != nullptr) {
      *error = "帧起止符异常";
    }
    return false;
  }
  if (data[7] != kFrameStart) {
    if (error != nullptr) {
      *error = "帧结构异常";
    }
    return false;
  }
  uint8_t len = data[9];
  const size_t expected = 10 + len + 2;
  if (data.size() != expected) {
    if (error != nullptr) {
      *error = "帧长度与数据长度不匹配";
    }
    return false;
  }
  const uint8_t cs = data[10 + len];
  const std::vector<uint8_t> csData(data.begin(), data.begin() + 10 + len);
  if (checksum(csData) != cs) {
    if (error != nullptr) {
      *error = "校验失败";
    }
    return false;
  }

  out->address.assign(data.begin() + 1, data.begin() + 7);
  out->control = data[8];
  out->data.assign(data.begin() + 10, data.begin() + 10 + len);
  return true;
}

std::vector<uint8_t> LinkManager::buildFrame(const std::vector<uint8_t> &addr, uint8_t control, const std::vector<uint8_t> &data) {
  std::vector<uint8_t> frame;
  frame.reserve(12 + data.size());
  frame.push_back(kFrameStart);
  frame.insert(frame.end(), addr.begin(), addr.end());
  frame.push_back(kFrameStart);
  frame.push_back(control);
  frame.push_back(static_cast<uint8_t>(data.size()));
  frame.insert(frame.end(), data.begin(), data.end());
  const uint8_t cs = checksum(frame);
  frame.push_back(cs);
  frame.push_back(kFrameEnd);
  return frame;
}

uint8_t LinkManager::checksum(const std::vector<uint8_t> &data) {
  uint32_t sum = 0;
  for (uint8_t b : data) {
    sum += b;
  }
  return static_cast<uint8_t>(sum & 0xFF);
}

void LinkManager::addOffset33(std::vector<uint8_t> *data) {
  if (!data) {
    return;
  }
  for (auto &b : *data) {
    b = static_cast<uint8_t>(b + 0x33);
  }
}

void LinkManager::subOffset33(std::vector<uint8_t> *data) {
  if (!data) {
    return;
  }
  for (auto &b : *data) {
    b = static_cast<uint8_t>(b - 0x33);
  }
}

grpc::Status LinkManager::handleMonitorResponse(LinkRuntime *link, const std::string &payloadBase64, const PointTable::Point &point, std::string *error) {
  Frame frame;
  auto status = parseResponsePayload(payloadBase64, &frame, error);
  if (!status.ok()) {
    return status;
  }
  const auto expectedAddr = encodeAddress(link->config.meter_addr());
  if (frame.address != expectedAddr) {
    if (error != nullptr) {
      *error = "地址不匹配";
    }
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "地址不匹配");
  }
  if ((frame.control & 0x40) != 0) {
    if (error != nullptr) {
      *error = "响应返回异常状态";
    }
    return grpc::Status(grpc::StatusCode::INTERNAL, "响应返回异常状态");
  }
  if ((frame.control & 0x80) == 0) {
    if (error != nullptr) {
      *error = "响应控制码异常";
    }
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应控制码异常");
  }
  subOffset33(&frame.data);

  const size_t diLen = link->config.protocol_variant() == DLT645Proto::PROTOCOL_VARIANT_DLT645_PCD ? 5 : 4;
  if (frame.data.size() < diLen + point.dataLen) {
    if (error != nullptr) {
      *error = "响应数据长度不足";
    }
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应数据长度不足");
  }
  for (size_t i = 0; i < 4; ++i) {
    if (frame.data[i] != point.diBytes[i]) {
      if (error != nullptr) {
        *error = "DI 不匹配";
      }
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "DI 不匹配");
    }
  }
  if (diLen == 5) {
    uint8_t deviceNo = 0;
    parseHexByte(link->config.device_no(), &deviceNo);
    if (frame.data[4] != deviceNo) {
      if (error != nullptr) {
        *error = "设备序号不匹配";
      }
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "设备序号不匹配");
    }
  }
  std::vector<uint8_t> payload(frame.data.begin() + diLen, frame.data.begin() + diLen + point.dataLen);
  LOG_INFO("DLT645 收到响应: conn_name={}, tag={}, payload={}", link->config.conn_name(), point.tag, formatHex(payload));
  return decodeAndPublish(link, point, payload, nowMs(), false);
}

grpc::Status LinkManager::handleMonitorResponse(LinkRuntime *link, const std::string &payloadBase64, const PointTable::Block &block, std::string *error) {
  Frame frame;
  auto status = parseResponsePayload(payloadBase64, &frame, error);
  if (!status.ok()) {
    return status;
  }
  const auto expectedAddr = encodeAddress(link->config.meter_addr());
  if (frame.address != expectedAddr) {
    if (error != nullptr) {
      *error = "地址不匹配";
    }
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "地址不匹配");
  }
  if ((frame.control & 0x40) != 0) {
    if (error != nullptr) {
      *error = "响应返回异常状态";
    }
    return grpc::Status(grpc::StatusCode::INTERNAL, "响应返回异常状态");
  }
  if ((frame.control & 0x80) == 0) {
    if (error != nullptr) {
      *error = "响应控制码异常";
    }
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应控制码异常");
  }
  subOffset33(&frame.data);

  const size_t diLen = link->config.protocol_variant() == DLT645Proto::PROTOCOL_VARIANT_DLT645_PCD ? 5 : 4;
  if (frame.data.size() < diLen + block.dataLen) {
    if (error != nullptr) {
      *error = "响应数据长度不足";
    }
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应数据长度不足");
  }
  for (size_t i = 0; i < 4; ++i) {
    if (frame.data[i] != block.diBytes[i]) {
      if (error != nullptr) {
        *error = "DI 不匹配";
      }
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "DI 不匹配");
    }
  }
  if (diLen == 5) {
    uint8_t deviceNo = 0;
    parseHexByte(link->config.device_no(), &deviceNo);
    if (frame.data[4] != deviceNo) {
      if (error != nullptr) {
        *error = "设备序号不匹配";
      }
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "设备序号不匹配");
    }
  }
  std::vector<uint8_t> payload(frame.data.begin() + diLen, frame.data.begin() + diLen + block.dataLen);
  LOG_INFO("DLT645 收到数据块响应: conn_name={}, block_di={}, payload={}", link->config.conn_name(), block.diText, formatHex(payload));

  const auto tsMs = nowMs();
  bool hasError = false;
  std::string lastError;
  for (const auto &item : block.items) {
    if (item.point.access == DLT645Proto::ACCESS_WRITE_ONLY) {
      continue;
    }
    if (item.offset + item.point.dataLen > payload.size()) {
      hasError = true;
      lastError = "数据块子项超出范围";
      LOG_WARNING("DLT645 数据块子项超出范围: conn_name={}, block_di={}, tag={}, offset={}, data_len={}", link->config.conn_name(), block.diText, item.point.tag, item.offset, item.point.dataLen);
      continue;
    }
    std::vector<uint8_t> itemPayload(payload.begin() + item.offset, payload.begin() + item.offset + item.point.dataLen);
    auto itemStatus = decodeAndPublish(link, item.point, itemPayload, tsMs, item.trimRightSpace);
    if (!itemStatus.ok()) {
      hasError = true;
      lastError = itemStatus.error_message();
      LOG_WARNING("DLT645 数据块子项解析失败: conn_name={}, block_di={}, tag={}, 原因={}", link->config.conn_name(), block.diText, item.point.tag, itemStatus.error_message());
    }
  }
  if (hasError) {
    if (error != nullptr) {
      *error = lastError.empty() ? "数据块子项解析失败" : lastError;
    }
    return grpc::Status(grpc::StatusCode::INTERNAL, "数据块子项解析失败");
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::parseResponsePayload(const std::string &payloadBase64, Frame *outFrame, std::string *error) {
  std::vector<uint8_t> raw;
  if (!base64Decode(payloadBase64, &raw)) {
    if (error != nullptr) {
      *error = "Base64 解码失败";
    }
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Base64 解码失败");
  }
  LOG_INFO("DLT645 收到响应帧: {}", formatHex(raw));
  std::string firstError;
  if (decodeFrame(raw, outFrame, &firstError)) {
    return grpc::Status::OK;
  }
  LOG_WARNING("DLT645 响应帧解析失败，尝试从报文流中提取有效帧: 原因={}", firstError);
  for (size_t i = 0; i + kMinFrameSize <= raw.size(); ++i) {
    if (raw[i] != kFrameStart) {
      continue;
    }
    if (i + 7 >= raw.size() || raw[i + 7] != kFrameStart) {
      continue;
    }
    if (i + 9 >= raw.size()) {
      continue;
    }
    const uint8_t len = raw[i + 9];
    const size_t expected = 10 + len + 2;
    if (i + expected > raw.size()) {
      continue;
    }
    std::vector<uint8_t> candidate(raw.begin() + static_cast<std::vector<uint8_t>::difference_type>(i), raw.begin() + static_cast<std::vector<uint8_t>::difference_type>(i + expected));
    std::string candidateError;
    if (!decodeFrame(candidate, outFrame, &candidateError)) {
      continue;
    }
    LOG_INFO("DLT645 从报文流中提取到有效帧: offset={}, len={}, frame={}", i, candidate.size(), formatHex(candidate));
    return grpc::Status::OK;
  }
  if (error != nullptr) {
    *error = "帧解析失败且未找到有效帧";
  }
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "帧解析失败且未找到有效帧");
}

bool LinkManager::pointValueToDouble(const DataCenterProto::PointValue &value, double *out) {
  if (out == nullptr) {
    return false;
  }
  if (value.has_double_value()) {
    *out = value.double_value();
    return true;
  }
  if (value.has_int_value()) {
    *out = static_cast<double>(value.int_value());
    return true;
  }
  return false;
}

bool LinkManager::pointValueToBool(const DataCenterProto::PointValue &value, bool *out) {
  if (out == nullptr) {
    return false;
  }
  if (value.has_bool_value()) {
    *out = value.bool_value();
    return true;
  }
  if (value.has_int_value()) {
    *out = value.int_value() != 0;
    return true;
  }
  return false;
}

bool LinkManager::pointValueToString(const DataCenterProto::PointValue &value, std::string *out) {
  if (out == nullptr) {
    return false;
  }
  if (value.has_string_value()) {
    *out = value.string_value();
    return true;
  }
  return false;
}

bool LinkManager::reverseScale(double value, double scale, double offset, double *out) {
  if (out == nullptr) {
    return false;
  }
  const double s = scale == 0.0 ? 1.0 : scale;
  *out = (value - offset) / s;
  return true;
}

std::string LinkManager::nextToken() {
  const auto token = std::to_string(tokenCounter_.fetch_add(1) + 1);
  LOG_DEBUG("DLT645 生成 MQTT token 字符串: token={}", token);
  return token;
}

}  // namespace DLT645
