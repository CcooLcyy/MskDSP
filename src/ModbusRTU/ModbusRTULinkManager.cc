#include "ModbusRTULinkManager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Logger.h"
#include "ModbusRTULibInfo.h"
#include "ThreadUtil.hpp"

namespace ModbusRTU {
namespace {
constexpr uint32_t kDefaultBaudRate = 9600;
constexpr uint32_t kDefaultDataBits = 8;
constexpr uint32_t kDefaultReadTimeoutMs = 1000;
constexpr uint32_t kDefaultPollIntervalMs = 1000;
constexpr uint32_t kDefaultRequestTimeoutMs = 3000;
constexpr uint32_t kDefaultSerialByteTimeoutMs = 100;
constexpr uint32_t kDefaultSerialFrameTimeoutMs = 100;
constexpr uint32_t kDefaultSerialEstSize = 256;
constexpr uint16_t kMaxReadHoldingRegistersQuantity = 125;
constexpr uint16_t kMaxReadInputRegistersQuantity = 125;
constexpr uint16_t kMaxWriteMultipleRegistersQuantity = 123;
constexpr uint8_t kFunctionReadCoils = 0x01;
constexpr uint8_t kFunctionReadHoldingRegisters = 0x03;
constexpr uint8_t kFunctionReadInputRegisters = 0x04;

bool is16BitRegisterType(ModbusRTUProto::DataType type) {
  return type == ModbusRTUProto::DATA_TYPE_UINT16 || type == ModbusRTUProto::DATA_TYPE_INT16;
}

bool is32BitRegisterType(ModbusRTUProto::DataType type) {
  return type == ModbusRTUProto::DATA_TYPE_UINT32 || type == ModbusRTUProto::DATA_TYPE_INT32;
}

bool isReadRegisterFunction(ModbusRTUProto::FunctionCode function) {
  return function == ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS ||
      function == ModbusRTUProto::FUNCTION_READ_INPUT_REGISTERS;
}

bool isWriteSingleRegisterFunction(ModbusRTUProto::FunctionCode function) {
  return function == ModbusRTUProto::FUNCTION_WRITE_SINGLE_REGISTER;
}

bool isWriteSingleCoilFunction(ModbusRTUProto::FunctionCode function) {
  return function == ModbusRTUProto::FUNCTION_WRITE_SINGLE_COIL;
}

bool isWriteMultipleRegistersFunction(ModbusRTUProto::FunctionCode function) {
  return function == ModbusRTUProto::FUNCTION_WRITE_MULTIPLE_REGISTERS;
}

bool isWriteRegisterFunction(ModbusRTUProto::FunctionCode function) {
  return isWriteSingleRegisterFunction(function) || isWriteMultipleRegistersFunction(function);
}

bool isWriteFunction(ModbusRTUProto::FunctionCode function) {
  return isWriteRegisterFunction(function) || isWriteSingleCoilFunction(function);
}

bool isRegisterBitPoint(const PointTable::Point& point) {
  return point.type == ModbusRTUProto::DATA_TYPE_BOOL && isReadRegisterFunction(point.function) && point.bitIndex.has_value();
}

uint16_t swapWordBytes(uint16_t value) {
  return static_cast<uint16_t>((value << 8) | (value >> 8));
}

int16_t decodeInt16(uint16_t value, ModbusRTUProto::ByteOrder byteOrder) {
  if (byteOrder == ModbusRTUProto::BYTE_ORDER_BA) {
    value = swapWordBytes(value);
  }
  return static_cast<int16_t>(value);
}

uint32_t decodeUint32(uint16_t first,
                      uint16_t second,
                      ModbusRTUProto::WordOrder wordOrder,
                      ModbusRTUProto::ByteOrder byteOrder) {
  if (byteOrder == ModbusRTUProto::BYTE_ORDER_BA) {
    first = swapWordBytes(first);
    second = swapWordBytes(second);
  }
  if (wordOrder == ModbusRTUProto::WORD_ORDER_LH) {
    std::swap(first, second);
  }
  return (static_cast<uint32_t>(first) << 16) | static_cast<uint32_t>(second);
}

int32_t decodeInt32(uint16_t first,
                    uint16_t second,
                    ModbusRTUProto::WordOrder wordOrder,
                    ModbusRTUProto::ByteOrder byteOrder) {
  return static_cast<int32_t>(decodeUint32(first, second, wordOrder, byteOrder));
}

std::array<uint16_t, 2> encodeUint32(uint32_t value,
                                     ModbusRTUProto::WordOrder wordOrder,
                                     ModbusRTUProto::ByteOrder byteOrder) {
  uint16_t high = static_cast<uint16_t>((value >> 16) & 0xFFFF);
  uint16_t low = static_cast<uint16_t>(value & 0xFFFF);
  uint16_t first = (wordOrder == ModbusRTUProto::WORD_ORDER_LH) ? low : high;
  uint16_t second = (wordOrder == ModbusRTUProto::WORD_ORDER_LH) ? high : low;
  if (byteOrder == ModbusRTUProto::BYTE_ORDER_BA) {
    first = swapWordBytes(first);
    second = swapWordBytes(second);
  }
  return {first, second};
}

bool shouldReport(double value, double deadband, const std::optional<double>& last) {
  if (deadband <= 0 || !last.has_value()) {
    return true;
  }
  return std::fabs(value - last.value()) >= deadband;
}

bool hasUsableMqttConfig(const ModbusRTUProto::MqttConfig& config) {
  return !config.host().empty() && config.port() != 0 && !config.client_id().empty();
}

bool pointValueToDouble(const DataCenterProto::PointValue& value, double* out) {
  if (out == nullptr) {
    return false;
  }
  switch (value.kind_case()) {
    case DataCenterProto::PointValue::kDoubleValue:
      *out = value.double_value();
      return true;
    case DataCenterProto::PointValue::kIntValue:
      *out = static_cast<double>(value.int_value());
      return true;
    case DataCenterProto::PointValue::kBoolValue:
      *out = value.bool_value() ? 1.0 : 0.0;
      return true;
    default:
      return false;
  }
}

bool reverseScale(double eng, double scale, double offset, double* out) {
  if (out == nullptr) {
    return false;
  }
  if (scale == 0.0) {
    scale = 1.0;
  }
  const double raw = (eng - offset) / scale;
  if (!std::isfinite(raw)) {
    return false;
  }
  *out = raw;
  return true;
}

grpc::Status makeNotFound(const std::string& connName) {
  return grpc::Status(grpc::StatusCode::NOT_FOUND, std::format("未找到链路: {}", connName));
}

bool renamePersistedLinkConfig(ModbusRTUProto::LinksConfig* config,
                               const std::string& oldConnName,
                               const std::string& newConnName) {
  if (config == nullptr) {
    return false;
  }
  for (auto& link : *config->mutable_links()) {
    if (link.config().conn_name() != oldConnName) {
      continue;
    }
    link.mutable_config()->set_conn_name(newConnName);
    return true;
  }
  return false;
}

void renamePersistedPointTableConfig(ModbusRTUProto::PointTablesConfig* config,
                                     const std::string& oldConnName,
                                     const std::string& newConnName) {
  if (config == nullptr) {
    return;
  }
  for (auto& table : *config->mutable_point_tables()) {
    if (table.conn_name() == oldConnName) {
      table.set_conn_name(newConnName);
      return;
    }
  }
}

std::string formatRegisterWords(const std::vector<uint16_t>& values) {
  if (values.empty()) {
    return {};
  }
  std::string out;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      out.append(" ");
    }
    out.append(std::format("0x{:04X}", values[i]));
  }
  return out;
}

grpc::Status encodeWriteRegisters(const PointTable::Point& point, double engValue, std::vector<uint16_t>* outValues) {
  if (outValues == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "outValues 为空");
  }
  if (!isWriteRegisterFunction(point.function)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "点位不是写寄存器功能码");
  }

  outValues->clear();
  if (point.type == ModbusRTUProto::DATA_TYPE_BOOL) {
    if (point.function != ModbusRTUProto::FUNCTION_WRITE_SINGLE_REGISTER) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "BOOL 仅支持写单寄存器");
    }
    outValues->push_back(engValue != 0.0 ? 1u : 0u);
    return grpc::Status::OK;
  }

  double rawValue = 0.0;
  if (!reverseScale(engValue, point.scale, point.offset, &rawValue)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "工程量反向缩放失败");
  }
  if (point.type == ModbusRTUProto::DATA_TYPE_UINT16) {
    const double minValue = 0.0;
    const double maxValue = static_cast<double>(std::numeric_limits<uint16_t>::max());
    if (rawValue < minValue || rawValue > maxValue) {
      return grpc::Status(grpc::StatusCode::OUT_OF_RANGE, "UINT16 写入值超出范围");
    }
    auto word = static_cast<uint16_t>(std::llround(rawValue));
    if (point.byteOrder == ModbusRTUProto::BYTE_ORDER_BA) {
      word = swapWordBytes(word);
    }
    outValues->push_back(word);
    return grpc::Status::OK;
  }
  if (point.type == ModbusRTUProto::DATA_TYPE_INT16) {
    const double minValue = static_cast<double>(std::numeric_limits<int16_t>::min());
    const double maxValue = static_cast<double>(std::numeric_limits<int16_t>::max());
    if (rawValue < minValue || rawValue > maxValue) {
      return grpc::Status(grpc::StatusCode::OUT_OF_RANGE, "INT16 写入值超出范围");
    }
    auto signedWord = static_cast<int16_t>(std::llround(rawValue));
    auto word = static_cast<uint16_t>(signedWord);
    if (point.byteOrder == ModbusRTUProto::BYTE_ORDER_BA) {
      word = swapWordBytes(word);
    }
    outValues->push_back(word);
    return grpc::Status::OK;
  }
  if (point.type == ModbusRTUProto::DATA_TYPE_UINT32) {
    const double minValue = 0.0;
    const double maxValue = static_cast<double>(std::numeric_limits<uint32_t>::max());
    if (rawValue < minValue || rawValue > maxValue) {
      return grpc::Status(grpc::StatusCode::OUT_OF_RANGE, "UINT32 写入值超出范围");
    }
    const auto raw = static_cast<uint32_t>(std::llround(rawValue));
    auto words = encodeUint32(raw, point.wordOrder, point.byteOrder);
    outValues->push_back(words[0]);
    outValues->push_back(words[1]);
    return grpc::Status::OK;
  }
  if (point.type == ModbusRTUProto::DATA_TYPE_INT32) {
    const double minValue = static_cast<double>(std::numeric_limits<int32_t>::min());
    const double maxValue = static_cast<double>(std::numeric_limits<int32_t>::max());
    if (rawValue < minValue || rawValue > maxValue) {
      return grpc::Status(grpc::StatusCode::OUT_OF_RANGE, "INT32 写入值超出范围");
    }
    const auto signedValue = static_cast<int32_t>(std::llround(rawValue));
    auto words = encodeUint32(static_cast<uint32_t>(signedValue), point.wordOrder, point.byteOrder);
    outValues->push_back(words[0]);
    outValues->push_back(words[1]);
    return grpc::Status::OK;
  }
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "写寄存器点位类型不支持");
}
}  // namespace

LinkManager::LinkManager(std::string moduleName,
                         std::filesystem::path configDbPath) :
  dataCenter_(moduleName),
  mqttClient_(std::move(moduleName)),
  mqttStore_(configDbPath),
  linkStore_(configDbPath),
  pointTableStore_(std::move(configDbPath)) {}

void LinkManager::LoadPersistedConfig() {
  LOG_INFO("ModbusRTU 开始加载本地持久化配置");

  {
    ModbusRTUProto::MqttConfig mqttConfig;
    auto status = mqttStore_.Load(&mqttConfig);
    if (!status.ok()) {
      LOG_ERROR("ModbusRTU MQTT 持久化配置加载失败: 原因={}", status.error_message());
    } else if (hasUsableMqttConfig(mqttConfig)) {
      mqttClient_.setConfig(mqttConfig);
      LOG_INFO("ModbusRTU 已加载 MQTT 持久化配置: host={}, port={}, client_id={}",
               mqttConfig.host(),
               mqttConfig.port(),
               mqttConfig.client_id());
    } else {
      LOG_INFO("ModbusRTU 未找到 MQTT 持久化配置");
    }
  }

  ModbusRTUProto::LinksConfig linksConfig;
  auto linksStatus = linkStore_.Load(&linksConfig);
  if (!linksStatus.ok()) {
    LOG_ERROR("ModbusRTU 链路持久化配置加载失败: 原因={}", linksStatus.error_message());
    return;
  }

  ModbusRTUProto::PointTablesConfig pointTablesConfig;
  auto pointTablesStatus = pointTableStore_.Load(&pointTablesConfig);
  if (!pointTablesStatus.ok()) {
    LOG_ERROR("ModbusRTU 点表持久化配置加载失败: 原因={}", pointTablesStatus.error_message());
    return;
  }
  LOG_INFO("ModbusRTU 持久化配置载入摘要: 链路记录数={}, 点表记录数={}",
           linksConfig.links_size(),
           pointTablesConfig.point_tables_size());

  std::unordered_map<std::string, ModbusRTUProto::PointTable> pointTablesByConn;
  pointTablesByConn.reserve(static_cast<size_t>(pointTablesConfig.point_tables_size()));
  for (const auto& table : pointTablesConfig.point_tables()) {
    pointTablesByConn.emplace(table.conn_name(), table);
  }

  if (linksConfig.links_size() == 0) {
    if (!pointTablesByConn.empty()) {
      LOG_WARNING("ModbusRTU 未找到链路持久化配置，但存在 {} 条点表持久化配置，准备清理孤立点表",
                  pointTablesByConn.size());
      auto saveStatus = savePointTablesConfig(ModbusRTUProto::PointTablesConfig());
      if (!saveStatus.ok()) {
        LOG_ERROR("ModbusRTU 清理孤立点表持久化配置失败: 原因={}", saveStatus.error_message());
      }
    } else {
      LOG_INFO("ModbusRTU 未找到链路持久化配置");
    }
    return;
  }

  std::unordered_map<std::string, LinkRuntime> restoredLinks;
  restoredLinks.reserve(static_cast<size_t>(linksConfig.links_size()));
  bool needResaveLinks = false;
  bool needResavePointTables = false;
  std::unordered_set<std::string> pointTablesLeftByDataCenterFailure;

  for (const auto& persistedLink : linksConfig.links()) {
    if (!persistedLink.has_config()) {
      LOG_WARNING("ModbusRTU 跳过空链路持久化记录");
      needResaveLinks = true;
      continue;
    }

    ModbusRTUProto::LinkConfig normalized;
    auto status = normalizeLinkConfig(persistedLink.config(), &normalized);
    if (!status.ok()) {
      LOG_ERROR("ModbusRTU 链路持久化配置非法，已跳过: conn_name={}, 原因={}",
                persistedLink.config().conn_name(),
                status.error_message());
      needResaveLinks = true;
      if (!persistedLink.config().conn_name().empty() &&
          pointTablesByConn.erase(persistedLink.config().conn_name()) > 0) {
        needResavePointTables = true;
      }
      continue;
    }

    size_t persistedPointCount = 0;
    if (auto persistedTableIt = pointTablesByConn.find(normalized.conn_name());
        persistedTableIt != pointTablesByConn.end()) {
      persistedPointCount = static_cast<size_t>(persistedTableIt->second.points_size());
    }
    LOG_INFO("ModbusRTU 开始恢复链路持久化记录: conn_name={}, 持久化conn_id={}, 待删除={}, 持久化点数={}",
             normalized.conn_name(),
             persistedLink.conn_id(),
             persistedLink.pending_delete(),
             persistedPointCount);

    DataCenterProto::ConnectionInfo connInfo;
    status = dataCenter_.GetOrCreateConnection(normalized.conn_name(), &connInfo);
    if (!status.ok()) {
      if (persistedPointCount > 0) {
        pointTablesLeftByDataCenterFailure.emplace(normalized.conn_name());
      }
      LOG_ERROR("ModbusRTU 恢复链路时获取 DataCenter 连接失败: conn_name={}, 原因={}, 本地点表点数={}",
                normalized.conn_name(),
                status.error_message(),
                persistedPointCount);
      continue;
    }

    LinkRuntime runtime;
    runtime.config = normalized;
    if (normalized.transport_type() == ModbusRTUProto::TRANSPORT_SERIAL) {
      runtime.serialKey = makeSerialKey(normalized.serial());
    } else {
      runtime.mqttKey = makeMqttKey(normalized);
    }
    runtime.connId = connInfo.conn_id();
    runtime.state = persistedLink.pending_delete() ? ModbusRTUProto::LINK_STATE_PENDING_DELETE
                                                   : ModbusRTUProto::LINK_STATE_STOPPED;

    size_t pointCount = 0;
    auto tableIt = pointTablesByConn.find(normalized.conn_name());
    if (tableIt != pointTablesByConn.end()) {
      PointTable pointTable;
      status = pointTable.Upsert(tableIt->second.points(), true);
      if (!status.ok()) {
        LOG_ERROR("ModbusRTU 恢复点表失败，已跳过该点表: conn_name={}, 原因={}",
                  normalized.conn_name(),
                  status.error_message());
        needResavePointTables = true;
      } else {
        runtime.pointTable = std::move(pointTable);
        pointCount = static_cast<size_t>(tableIt->second.points_size());
        runtime.pointTableConfigured = pointCount > 0;
        if (!runtime.pointTableConfigured) {
          LOG_WARNING("ModbusRTU 恢复到空点表，链路将保持已停止等待后续补全: conn_name={}",
                      normalized.conn_name());
        }
      }
      pointTablesByConn.erase(tableIt);
    } else {
      LOG_WARNING("ModbusRTU 恢复链路时未找到对应点表，链路将保持已停止等待后续补全: conn_name={}",
                  normalized.conn_name());
    }

    auto syncStatus = dataCenter_.UpsertConnTags(runtime.connId, runtime.pointTable.Tags(), true);
    if (!syncStatus.ok()) {
      LOG_ERROR("ModbusRTU 恢复链路时同步 DataCenter 连接标签注册表失败: conn_name={}, conn_id={}, 原因={}",
                normalized.conn_name(),
                runtime.connId,
                syncStatus.error_message());
    }

    if (persistedLink.conn_id() != 0 && persistedLink.conn_id() != runtime.connId) {
      LOG_WARNING("ModbusRTU 恢复链路时发现 conn_id 已变化: conn_name={}, 持久化conn_id={}, 当前conn_id={}",
                  normalized.conn_name(),
                  persistedLink.conn_id(),
                  runtime.connId);
      needResaveLinks = true;
    }

    restoredLinks[normalized.conn_name()] = std::move(runtime);
    LOG_INFO("ModbusRTU 已恢复链路配置: conn_name={}, conn_id={}, 点数={}, 状态={}",
             normalized.conn_name(),
             connInfo.conn_id(),
             pointCount,
             persistedLink.pending_delete() ? "待删除" : "已停止");
  }

  for (const auto& [connName, _] : pointTablesByConn) {
    auto tableIt = pointTablesByConn.find(connName);
    const size_t pointCount = tableIt == pointTablesByConn.end() ? 0u : static_cast<size_t>(tableIt->second.points_size());
    const bool leftByDataCenterFailure = pointTablesLeftByDataCenterFailure.contains(connName);
    LOG_WARNING("ModbusRTU 点表持久化配置未进入本次恢复快照: conn_name={}, 点数={}, 原因={}",
                connName,
                pointCount,
                leftByDataCenterFailure ? "链路恢复阶段获取 DataCenter 连接失败" : "未找到对应链路");
    if (leftByDataCenterFailure) {
      LOG_WARNING("ModbusRTU 因 DataCenter 未就绪保留点表目标配置，禁止本次恢复回写空快照: conn_name={}", connName);
      continue;
    }
    needResavePointTables = true;
  }

  ModbusRTUProto::LinksConfig linksSnapshot;
  ModbusRTUProto::PointTablesConfig pointTablesSnapshot;
  {
    std::lock_guard<std::mutex> lock(mu_);
    linksByName_ = std::move(restoredLinks);
    buses_.clear();
    mqttBuses_.clear();
    pendingCreateByName_.clear();
    linksSnapshot = dumpLinksConfigLocked();
    pointTablesSnapshot = dumpPointTablesConfigLocked();
  }

  if (needResaveLinks) {
    auto status = saveLinksConfig(linksSnapshot);
    if (!status.ok()) {
      LOG_ERROR("ModbusRTU 清理链路持久化配置失败: 原因={}", status.error_message());
    }
  }
  if (needResavePointTables) {
    LOG_WARNING("ModbusRTU 本次恢复将回写点表持久化配置: 恢复后链路数={}, 回写点表记录数={}",
                linksSnapshot.links_size(),
                pointTablesSnapshot.point_tables_size());
    auto status = savePointTablesConfig(pointTablesSnapshot);
    if (!status.ok()) {
      LOG_ERROR("ModbusRTU 清理点表持久化配置失败: 原因={}", status.error_message());
    }
  }

  LOG_INFO("ModbusRTU 本地持久化配置加载完成: 链路数={}", linksSnapshot.links_size());
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
  LOG_INFO("ModbusRTU 已设置 MQTT Stub");
}

grpc::Status LinkManager::UpdateConfig(const ModbusRTUProto::UpdateConfigRequest& request,
                                       ModbusRTUProto::UpdateConfigResponse* response) {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
  }
  if (!request.has_mqtt()) {
    response->set_ok(false);
    response->set_message("MQTT 配置为空");
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "MQTT 配置为空");
  }
  const auto& mqtt = request.mqtt();
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
    LOG_ERROR("ModbusRTU MQTT 配置落盘失败: host={}, port={}, client_id={}, 原因={}",
              mqtt.host(),
              mqtt.port(),
              mqtt.client_id(),
              status.error_message());
    return status;
  }
  LOG_INFO("ModbusRTU MQTT 配置已落盘: host={}, port={}, client_id={}",
           mqtt.host(),
           mqtt.port(),
           mqtt.client_id());
  response->set_ok(true);
  response->set_message("MQTT 配置更新成功");
  LOG_INFO("ModbusRTU MQTT 配置更新成功，当前不会自动启动链路连接功能，等待显式调用 StartLink");
  return grpc::Status::OK;
}

grpc::Status LinkManager::GetConfig(ModbusRTUProto::GetConfigResponse* response) const {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "响应为空");
  }
  response->Clear();

  ModbusRTUProto::MqttConfig mqtt;
  if (!mqttClient_.getConfig(&mqtt)) {
    response->set_configured(false);
    response->set_message("MQTT 配置未配置");
    LOG_INFO("ModbusRTU MQTT 配置查询结果: configured=false, 原因=未配置");
    return grpc::Status::OK;
  }
  if (!hasUsableMqttConfig(mqtt)) {
    response->set_configured(false);
    response->set_message("MQTT 配置不完整");
    LOG_WARNING("ModbusRTU MQTT 配置查询结果: configured=false, 原因=配置不完整");
    return grpc::Status::OK;
  }

  *response->mutable_mqtt() = mqtt;
  response->set_configured(true);
  response->set_message("MQTT 配置已配置");
  LOG_INFO("ModbusRTU MQTT 配置查询成功: host={}, port={}, client_id={}",
           mqtt.host(),
           mqtt.port(),
           mqtt.client_id());
  return grpc::Status::OK;
}

size_t LinkManager::SerialKeyHash::operator()(const SerialKey& key) const {
  std::hash<std::string> strHash;
  size_t seed = strHash(key.device);
  seed ^= static_cast<size_t>(key.baudRate) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  seed ^= static_cast<size_t>(key.dataBits) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  seed ^= static_cast<size_t>(key.parity) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  seed ^= static_cast<size_t>(key.stopBits) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  seed ^= static_cast<size_t>(key.readTimeoutMs) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  return seed;
}

size_t LinkManager::MqttKeyHash::operator()(const MqttKey& key) const {
  std::hash<std::string> strHash;
  size_t seed = strHash(key.serialPort);
  seed ^= static_cast<size_t>(key.baudRate) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  seed ^= static_cast<size_t>(key.dataBits) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  seed ^= static_cast<size_t>(key.parity) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  seed ^= static_cast<size_t>(key.stopBits) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  seed ^= static_cast<size_t>(key.requestTimeoutMs) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  seed ^= static_cast<size_t>(key.byteTimeoutMs) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  seed ^= static_cast<size_t>(key.frameTimeoutMs) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  seed ^= static_cast<size_t>(key.estSize) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  return seed;
}

grpc::Status LinkManager::validateConnName(const std::string& connName) {
  if (connName.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_name 不能为空");
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::normalizeLinkConfig(const ModbusRTUProto::LinkConfig& config, ModbusRTUProto::LinkConfig* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  if (config.conn_name().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_name 不能为空");
  }
  if (!config.has_serial()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "serial 配置不能为空");
  }
  *out = config;
  auto* serial = out->mutable_serial();
  if (out->transport_type() == ModbusRTUProto::TRANSPORT_UNSPECIFIED) {
    out->set_transport_type(ModbusRTUProto::TRANSPORT_SERIAL);
  }
  if (serial->baud_rate() == 0) {
    serial->set_baud_rate(kDefaultBaudRate);
  }
  if (serial->data_bits() == 0) {
    serial->set_data_bits(kDefaultDataBits);
  }
  if (serial->parity() == ModbusRTUProto::PARITY_UNSPECIFIED) {
    serial->set_parity(ModbusRTUProto::PARITY_NONE);
  }
  if (serial->stop_bits() == ModbusRTUProto::STOP_BITS_UNSPECIFIED) {
    serial->set_stop_bits(ModbusRTUProto::STOP_BITS_ONE);
  }
  if (out->poll_interval_ms() == 0) {
    out->set_poll_interval_ms(kDefaultPollIntervalMs);
  }
  if (out->address_base() == ModbusRTUProto::ADDRESS_BASE_UNSPECIFIED) {
    out->set_address_base(ModbusRTUProto::ADDRESS_BASE_ZERO);
  }
  if (out->transport_type() == ModbusRTUProto::TRANSPORT_SERIAL) {
    if (serial->device().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "TRANSPORT_SERIAL 要求 serial.device 不能为空");
    }
    if (serial->read_timeout_ms() == 0) {
      serial->set_read_timeout_ms(kDefaultReadTimeoutMs);
    }
  } else if (out->transport_type() == ModbusRTUProto::TRANSPORT_MQTT_UART ||
             out->transport_type() == ModbusRTUProto::TRANSPORT_MQTT) {
    out->set_transport_type(ModbusRTUProto::TRANSPORT_MQTT_UART);
    if (out->serial_port().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "TRANSPORT_MQTT_UART 要求 serial_port 不能为空");
    }
    if (out->request_timeout_ms() == 0) {
      out->set_request_timeout_ms(kDefaultRequestTimeoutMs);
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
  } else {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "暂不支持该传输类型");
  }

  if (serial->data_bits() < 5 || serial->data_bits() > 8) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "serial.data_bits 必须在 [5,8] 范围内");
  }
  if (serial->parity() != ModbusRTUProto::PARITY_NONE &&
      serial->parity() != ModbusRTUProto::PARITY_ODD &&
      serial->parity() != ModbusRTUProto::PARITY_EVEN) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "serial.parity 非法");
  }
  if (serial->stop_bits() != ModbusRTUProto::STOP_BITS_ONE &&
      serial->stop_bits() != ModbusRTUProto::STOP_BITS_TWO) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "serial.stop_bits 非法");
  }
  if (out->transport_type() == ModbusRTUProto::TRANSPORT_SERIAL && serial->read_timeout_ms() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "serial.read_timeout_ms 不能为空");
  }
  if (out->poll_interval_ms() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "poll_interval_ms 不能为空");
  }
  if (out->address_base() != ModbusRTUProto::ADDRESS_BASE_ZERO &&
      out->address_base() != ModbusRTUProto::ADDRESS_BASE_ONE) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "address_base 非法");
  }
  if (out->device_id() == 0 || out->device_id() > 247) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "设备地址(device_id) 必须在 [1,247] 范围内");
  }
  if (out->has_read_plan()) {
    auto *plan = out->mutable_read_plan();
    if (plan->mode() == ModbusRTUProto::READ_PLAN_MODE_UNSPECIFIED) {
      plan->set_mode(plan->blocks_size() > 0 ? ModbusRTUProto::READ_PLAN_MODE_EXPLICIT
                                             : ModbusRTUProto::READ_PLAN_MODE_POINT);
    }
    if (plan->mode() == ModbusRTUProto::READ_PLAN_MODE_EXPLICIT) {
      if (plan->blocks_size() == 0) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "read_plan.blocks 不能为空");
      }
    } else if (plan->mode() == ModbusRTUProto::READ_PLAN_MODE_POINT) {
      if (plan->blocks_size() > 0) {
        LOG_INFO("ModbusRTU 逐点抄读保留显式区间配置但不会使用: conn_name={}, blocks={}",
                 out->conn_name(),
                 plan->blocks_size());
      }
    } else {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "read_plan.mode 非法");
    }

    // POINT 模式也校验并保留区间，避免切回 EXPLICIT 模式时才发现配置无效。
    for (const auto &block : plan->blocks()) {
      if (block.function() == ModbusRTUProto::FUNCTION_UNSPECIFIED) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "read_plan.blocks.function 不能为空");
      }
      if (block.function() != ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS &&
          block.function() != ModbusRTUProto::FUNCTION_READ_INPUT_REGISTERS) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "read_plan 仅支持读保持寄存器(0x03)或输入寄存器(0x04)");
      }
      if (block.quantity() == 0) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "read_plan.blocks.quantity 不能为空");
      }
      const auto maxQuantity =
          block.function() == ModbusRTUProto::FUNCTION_READ_INPUT_REGISTERS ? kMaxReadInputRegistersQuantity
                                                                            : kMaxReadHoldingRegistersQuantity;
      if (block.quantity() > maxQuantity) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "read_plan.blocks.quantity 超出上限(125)");
      }
      uint32_t start = block.start();
      if (out->address_base() == ModbusRTUProto::ADDRESS_BASE_ONE) {
        if (start == 0) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "read_plan.blocks.start 不能为 0（address_base=ONE）");
        }
        start -= 1;
      }
      if (start > 0xFFFFu || start + block.quantity() - 1 > 0xFFFFu) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "read_plan.blocks 超出地址范围");
      }
    }
  }
  return grpc::Status::OK;
}

LinkManager::SerialKey LinkManager::makeSerialKey(const ModbusRTUProto::SerialConfig& serial) {
  SerialKey key;
  key.device = serial.device();
  key.baudRate = serial.baud_rate();
  key.dataBits = serial.data_bits();
  key.parity = serial.parity();
  key.stopBits = serial.stop_bits();
  key.readTimeoutMs = serial.read_timeout_ms();
  return key;
}

LinkManager::MqttKey LinkManager::makeMqttKey(const ModbusRTUProto::LinkConfig& config) {
  MqttKey key;
  key.serialPort = config.serial_port();
  key.baudRate = config.serial().baud_rate();
  key.dataBits = config.serial().data_bits();
  key.parity = config.serial().parity();
  key.stopBits = config.serial().stop_bits();
  key.requestTimeoutMs = config.request_timeout_ms();
  key.byteTimeoutMs = config.serial_byte_timeout_ms();
  key.frameTimeoutMs = config.serial_frame_timeout_ms();
  key.estSize = config.serial_est_size();
  return key;
}

grpc::Status LinkManager::fillLinkInfoLocked(const LinkRuntime& link, ModbusRTUProto::LinkInfo* out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  out->Clear();
  *out->mutable_config() = link.config;
  out->set_conn_id(link.connId);
  out->set_state(link.state);
  out->set_last_error(link.lastError);
  return grpc::Status::OK;
}

ModbusRTUProto::LinksConfig LinkManager::dumpLinksConfigLocked() const {
  ModbusRTUProto::LinksConfig config;
  std::vector<std::string> connNames;
  connNames.reserve(linksByName_.size());
  for (const auto& [connName, _] : linksByName_) {
    connNames.push_back(connName);
  }
  std::sort(connNames.begin(), connNames.end());

  for (const auto& connName : connNames) {
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      continue;
    }
    auto* item = config.add_links();
    *item->mutable_config() = it->second.config;
    item->set_conn_id(it->second.connId);
    item->set_pending_delete(it->second.state == ModbusRTUProto::LINK_STATE_PENDING_DELETE);
  }
  return config;
}

ModbusRTUProto::PointTablesConfig LinkManager::dumpPointTablesConfigLocked() const {
  ModbusRTUProto::PointTablesConfig config;
  std::vector<std::string> connNames;
  connNames.reserve(linksByName_.size());
  for (const auto& [connName, _] : linksByName_) {
    connNames.push_back(connName);
  }
  std::sort(connNames.begin(), connNames.end());

  for (const auto& connName : connNames) {
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      continue;
    }
    ModbusRTUProto::PointTable table;
    it->second.pointTable.ToProto(connName, &table);
    if (table.points_size() == 0) {
      continue;
    }
    *config.add_point_tables() = std::move(table);
  }
  return config;
}

grpc::Status LinkManager::saveLinksConfig(const ModbusRTUProto::LinksConfig& config) {
  auto status = linkStore_.Save(config);
  if (!status.ok()) {
    LOG_ERROR("ModbusRTU 链路配置落盘失败: 链路数={}, 原因={}",
              config.links_size(),
              status.error_message());
    return status;
  }
  LOG_INFO("ModbusRTU 链路配置已落盘: 链路数={}", config.links_size());
  return grpc::Status::OK;
}

grpc::Status LinkManager::savePointTablesConfig(const ModbusRTUProto::PointTablesConfig& config) {
  auto status = pointTableStore_.Save(config);
  if (!status.ok()) {
    LOG_ERROR("ModbusRTU 点表配置落盘失败: 链路数={}, 原因={}",
              config.point_tables_size(),
              status.error_message());
    return status;
  }
  LOG_INFO("ModbusRTU 点表配置已落盘: 链路数={}", config.point_tables_size());
  return grpc::Status::OK;
}

bool LinkManager::isLinkAutoStartReadyLocked(const LinkRuntime& link, std::string* reason) const {
  auto setReason = [reason](std::string text) {
    if (reason != nullptr) {
      *reason = std::move(text);
    }
    return false;
  };

  if (link.state == ModbusRTUProto::LINK_STATE_RUNNING) {
    return setReason("链路已在运行");
  }
  if (link.state == ModbusRTUProto::LINK_STATE_PENDING_DELETE) {
    return setReason("链路处于待删除状态");
  }
  if (link.connId == 0) {
    return setReason("conn_id 无效");
  }
  if (!link.pointTableConfigured) {
    return setReason("链路点表未就绪，当前规则要求链路配置和点表都成功恢复或下发后才启动连接功能");
  }
  if (link.config.transport_type() == ModbusRTUProto::TRANSPORT_MQTT_UART && !mqttClient_.hasConfig()) {
    return setReason("MQTT 全局配置未就绪，当前规则要求 MQTT 配置成功恢复或下发后才启动连接功能");
  }
  if (link.config.address_base() == ModbusRTUProto::ADDRESS_BASE_ONE) {
    for (const auto& point : link.pointTable.Points()) {
      if (point.address == 0) {
        return setReason("address_base=ONE 但点表存在 address=0，当前规则不允许启动连接功能");
      }
    }
  }
  return true;
}

grpc::Status LinkManager::maybeAutoStartLink(const std::string& connName, std::string_view trigger) {
  std::string reason;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      return makeNotFound(connName);
    }
    if (!isLinkAutoStartReadyLocked(it->second, &reason)) {
      setLastErrorLocked(&it->second, reason, LastErrorSource::kLifecycle);
      LOG_INFO("ModbusRTU 暂不自动启动链路: conn_name={}, 触发来源={}, 原因={}", connName, trigger, reason);
      return grpc::Status::OK;
    }
  }

  LOG_INFO("ModbusRTU 检测到链路已满足最小可运行条件，准备自动启动: conn_name={}, 触发来源={}",
           connName,
           trigger);
  auto status = StartLink(connName);
  if (!status.ok()) {
    LOG_WARNING("ModbusRTU 自动启动链路失败: conn_name={}, 触发来源={}, 原因={}",
                connName,
                trigger,
                status.error_message());
    return status;
  }
  LOG_INFO("ModbusRTU 自动启动链路完成: conn_name={}, 触发来源={}", connName, trigger);
  return grpc::Status::OK;
}

void LinkManager::autoStartEligibleLinks(std::string_view trigger) {
  std::vector<std::string> connNames;
  {
    std::lock_guard<std::mutex> lock(mu_);
    connNames.reserve(linksByName_.size());
    for (const auto& [connName, _] : linksByName_) {
      connNames.emplace_back(connName);
    }
  }
  std::sort(connNames.begin(), connNames.end());
  for (const auto& connName : connNames) {
    (void)maybeAutoStartLink(connName, trigger);
  }
}

void LinkManager::TryAutoStartReadyLinks(std::string_view trigger) {
  autoStartEligibleLinks(trigger);
}

grpc::Status LinkManager::ensureSerialCompatibleLocked(const SerialKey& key, const std::string& connName) const {
  for (const auto& [name, link] : linksByName_) {
    if (name == connName) {
      continue;
    }
    if (link.config.transport_type() != ModbusRTUProto::TRANSPORT_SERIAL) {
      continue;
    }
    if (link.serialKey.device != key.device) {
      continue;
    }
    if (!(link.serialKey == key)) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "串口配置与现有链路冲突");
    }
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::ensureMqttCompatibleLocked(const MqttKey& key, const std::string& connName) const {
  for (const auto& [name, link] : linksByName_) {
    if (name == connName) {
      continue;
    }
    if (link.config.transport_type() != ModbusRTUProto::TRANSPORT_MQTT_UART) {
      continue;
    }
    if (link.mqttKey.serialPort != key.serialPort) {
      continue;
    }
    if (!(link.mqttKey == key)) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "远端串口配置与现有链路冲突");
    }
  }
  return grpc::Status::OK;
}

std::shared_ptr<Bus> LinkManager::acquireSerialBusLocked(const SerialKey& key, const ModbusRTUProto::SerialConfig& serial) {
  auto it = buses_.find(key);
  if (it != buses_.end()) {
    it->second.refCount += 1;
    return it->second.bus;
  }
  auto bus = std::make_shared<SerialBus>(serial);
  BusEntry entry;
  entry.bus = bus;
  entry.refCount = 1;
  buses_.emplace(key, std::move(entry));
  return bus;
}

std::shared_ptr<Bus> LinkManager::releaseSerialBusLocked(const SerialKey& key) {
  auto it = buses_.find(key);
  if (it == buses_.end()) {
    return nullptr;
  }
  if (it->second.refCount > 0) {
    it->second.refCount -= 1;
  }
  if (it->second.refCount == 0) {
    auto bus = it->second.bus;
    buses_.erase(it);
    return bus;
  }
  return nullptr;
}

std::shared_ptr<Bus> LinkManager::acquireMqttBusLocked(const MqttKey& key, const ModbusRTUProto::LinkConfig& config) {
  auto it = mqttBuses_.find(key);
  if (it != mqttBuses_.end()) {
    it->second.refCount += 1;
    return it->second.bus;
  }
  auto bus = std::make_shared<MqttBus>(config, &mqttClient_);
  BusEntry entry;
  entry.bus = bus;
  entry.refCount = 1;
  mqttBuses_.emplace(key, std::move(entry));
  return bus;
}

std::shared_ptr<Bus> LinkManager::releaseMqttBusLocked(const MqttKey& key) {
  auto it = mqttBuses_.find(key);
  if (it == mqttBuses_.end()) {
    return nullptr;
  }
  if (it->second.refCount > 0) {
    it->second.refCount -= 1;
  }
  if (it->second.refCount == 0) {
    auto bus = it->second.bus;
    mqttBuses_.erase(it);
    return bus;
  }
  return nullptr;
}

void LinkManager::stopCommandSubscribeLocked(LinkRuntime* link) {
  if (link == nullptr) {
    return;
  }
  if (link->dcCommandContext) {
    link->dcCommandContext->TryCancel();
  }
  if (link->dcCommandThread.joinable()) {
    LOG_INFO("ModbusRTU 停止 DataCenter 命令订阅: conn_name={}", link->config.conn_name());
    link->dcCommandThread.request_stop();
    link->dcCommandThread.join();
  }
  link->dcCommandContext.reset();
}

void LinkManager::startCommandSubscribeLocked(const std::string& connName, LinkRuntime* link) {
  if (link == nullptr || !link->bus) {
    return;
  }
  stopCommandSubscribeLocked(link);

  std::vector<PointTable::Point> writePoints;
  for (const auto& point : link->pointTable.Points()) {
    if (isWriteFunction(point.function)) {
      writePoints.push_back(point);
    }
  }
  if (writePoints.empty()) {
    LOG_INFO("ModbusRTU 命令订阅无可用写点: conn_name={}", connName);
    return;
  }

  std::vector<std::string> tags;
  tags.reserve(writePoints.size());
  std::unordered_map<std::string, PointTable::Point> pointByTag;
  pointByTag.reserve(writePoints.size());
  for (const auto& point : writePoints) {
    tags.push_back(point.tag);
    pointByTag.emplace(point.tag, point);
  }

  const auto connId = link->connId;
  const auto config = link->config;
  auto bus = link->bus;
  LOG_INFO("ModbusRTU 启动 DataCenter 命令订阅: conn_name={}, conn_id={}, tags={}",
           connName,
           connId,
           tags.size());

  link->dcCommandContext = std::make_shared<grpc::ClientContext>();
  auto ctx = link->dcCommandContext;
  link->dcCommandThread = ModuleManager::StartModuleThread(
      ModbusRTULibInfo.LIB_NAME,
      [this, connName, ctx, connId, tags, pointByTag, config, bus](std::stop_token st) {
        std::stop_callback cb(st, [&ctx]() { ctx->TryCancel(); });

        auto reader = dataCenter_.Subscribe(ctx.get(), connId, tags, false);
        if (!reader) {
          LOG_ERROR("ModbusRTU 创建 DataCenter 命令订阅失败: conn_name={}, conn_id={}, tags={}",
                    connName,
                    connId,
                    tags.size());
          return;
        }

        DataCenterProto::PointUpdate update;
        while (reader->Read(&update)) {
          if (update.src_conn_id() == connId) {
            continue;
          }
          auto it = pointByTag.find(update.dst_tag());
          if (it == pointByTag.end()) {
            continue;
          }
          auto status = executeWriteCommand(connName, config, it->second, bus, update);
          if (!status.ok()) {
            updateLastError(connName, status.error_message(), LastErrorSource::kCommand);
            LOG_WARNING("ModbusRTU 写点失败: conn_name={}, tag={}, 原因={}",
                        connName,
                        update.dst_tag(),
                        status.error_message());
          } else {
            clearLastError(connName, LastErrorSource::kCommand);
          }
        }

        auto finishStatus = reader->Finish();
        if (!finishStatus.ok() && !st.stop_requested()) {
          LOG_WARNING("ModbusRTU DataCenter 命令订阅异常结束: conn_name={}, conn_id={}, 错误={}",
                      connName,
                      connId,
                      finishStatus.error_message());
          updateLastError(connName, finishStatus.error_message(), LastErrorSource::kCommand);
        }
      });
}

grpc::Status LinkManager::executeWriteCommand(const std::string& connName,
                                              const ModbusRTUProto::LinkConfig& config,
                                              const PointTable::Point& point,
                                              std::shared_ptr<Bus> bus,
                                              const DataCenterProto::PointUpdate& update) {
  if (!bus) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "总线未就绪");
  }
  if (!isWriteFunction(point.function)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "点位不是 ModbusRTU 可写功能码");
  }

  uint32_t address = point.address;
  if (config.address_base() == ModbusRTUProto::ADDRESS_BASE_ONE) {
    if (address == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "address_base=ONE 但 address 为 0");
    }
    address -= 1;
  }
  if (address > 0xFFFFu) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "写点地址超出范围");
  }
  if (point.regCount > 1 && address + point.regCount - 1 > 0xFFFFu) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "写点地址范围超出限制");
  }

  double engValue = 0.0;
  if (!pointValueToDouble(update.value(), &engValue)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "命令点值类型不支持");
  }
  if (!std::isfinite(engValue)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "命令点值必须为有限数值");
  }

  std::vector<uint16_t> values;
  auto status = grpc::Status::OK;
  const auto deviceId = static_cast<uint8_t>(config.device_id());
  if (isWriteSingleCoilFunction(point.function)) {
    const bool coilValue = engValue != 0.0;
    LOG_INFO("ModbusRTU 触发写单线圈: conn_name={}, tag={}, device_id={}, address={}, value={}",
             connName, point.tag, config.device_id(), address, coilValue);
    status = bus->WriteSingleCoil(deviceId, static_cast<uint16_t>(address), coilValue);
    if (status.ok()) {
      LOG_INFO("ModbusRTU 写单线圈成功: conn_name={}, tag={}, address={}, value={}",
               connName, point.tag, address, coilValue);
    }
    return status;
  }
  status = encodeWriteRegisters(point, engValue, &values);
  if (!status.ok()) {
    return status;
  }
  if (values.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "写寄存器编码结果为空");
  }
  if (point.function == ModbusRTUProto::FUNCTION_WRITE_MULTIPLE_REGISTERS &&
      values.size() > kMaxWriteMultipleRegistersQuantity) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "写多寄存器数量不能超过 123");
  }

  if (point.function == ModbusRTUProto::FUNCTION_WRITE_SINGLE_REGISTER) {
    if (values.size() != 1) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "写单寄存器编码数量异常");
    }
    LOG_INFO("ModbusRTU 触发写单寄存器: conn_name={}, tag={}, device_id={}, address={}, value={}, raw={}",
             connName,
             point.tag,
             config.device_id(),
             address,
             engValue,
             formatRegisterWords(values));
    status = bus->WriteSingleRegister(deviceId, static_cast<uint16_t>(address), values.front());
    if (status.ok()) {
      LOG_INFO("ModbusRTU 写单寄存器成功: conn_name={}, tag={}, address={}",
               connName,
               point.tag,
               address);
    }
    return status;
  }

  LOG_INFO("ModbusRTU 触发写多寄存器: conn_name={}, tag={}, device_id={}, address={}, quantity={}, value={}, raw={}",
           connName,
           point.tag,
           config.device_id(),
           address,
           values.size(),
           engValue,
           formatRegisterWords(values));
  status = bus->WriteMultipleRegisters(deviceId, static_cast<uint16_t>(address), values);
  if (status.ok()) {
    LOG_INFO("ModbusRTU 写多寄存器成功: conn_name={}, tag={}, address={}, quantity={}",
             connName,
             point.tag,
             address,
             values.size());
  }
  return status;
}

grpc::Status LinkManager::ExecuteCommand(
    const DataCenterProto::ExecuteCommandRequest& request,
    DataCenterProto::ExecuteCommandResponse* response) {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "response 为空");
  }
  response->Clear();
  if (!request.has_dst()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "dst 不能为空");
  }
  if (request.dst().conn_id() == 0 && request.dst().conn_name().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "dst.conn_id 和 dst.conn_name 不能同时为空");
  }
  if (request.dst().tag().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "dst.tag 不能为空");
  }
  if (!request.dst().module_name().empty() && request.dst().module_name() != ModbusRTULibInfo.LIB_NAME) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "dst.module_name 不是 ModbusRTU");
  }
  if (request.value().kind_case() == DataCenterProto::PointValue::KIND_NOT_SET) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "value 不能为空");
  }
  *response->mutable_dst() = request.dst();

  double requestedValue = 0.0;
  if (!pointValueToDouble(request.value(), &requestedValue)) {
    response->set_status(DataCenterProto::COMMAND_REJECTED);
    response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSUPPORTED_POINT);
    response->set_reason("命令点值类型不支持");
    return grpc::Status::OK;
  }
  response->set_requested_value(requestedValue);

  std::string connName;
  ModbusRTUProto::LinkConfig config;
  PointTable::Point point;
  std::shared_ptr<Bus> bus;
  std::shared_ptr<CommandGate> commandGate;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto linkIt = linksByName_.end();
    if (!request.dst().conn_name().empty()) {
      linkIt = linksByName_.find(request.dst().conn_name());
    } else {
      linkIt = std::find_if(linksByName_.begin(), linksByName_.end(), [&](const auto& entry) {
        return entry.second.connId == request.dst().conn_id();
      });
    }
    if (linkIt == linksByName_.end()) {
      response->set_status(DataCenterProto::COMMAND_TARGET_UNAVAILABLE);
      response->set_reason("未找到目的 ModbusRTU 链路");
      return grpc::Status::OK;
    }

    auto& link = linkIt->second;
    connName = linkIt->first;
    if (request.dst().conn_id() != 0 && request.dst().conn_id() != link.connId) {
      response->set_status(DataCenterProto::COMMAND_REJECTED);
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_BAD_CONFIG);
      response->set_reason("目的连接名称与 conn_id 不一致");
      return grpc::Status::OK;
    }
    response->mutable_dst()->set_conn_id(link.connId);
    response->mutable_dst()->set_conn_name(connName);
    response->mutable_dst()->set_module_name("ModbusRTU");
    if (link.state != ModbusRTUProto::LINK_STATE_RUNNING || !link.bus) {
      response->set_status(DataCenterProto::COMMAND_TARGET_UNAVAILABLE);
      response->set_reason("ModbusRTU 链路未运行");
      return grpc::Status::OK;
    }

    auto pointOpt = link.pointTable.FindByTag(request.dst().tag());
    if (!pointOpt.has_value() || !isWriteFunction(pointOpt->function)) {
      response->set_status(DataCenterProto::COMMAND_REJECTED);
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSUPPORTED_POINT);
      response->set_reason("目的点不存在或不是 ModbusRTU 可写点");
      return grpc::Status::OK;
    }
    config = link.config;
    point = *pointOpt;
    bus = link.bus;
    std::lock_guard<std::mutex> gateLock(link.commandGate->mu);
    if (!link.commandGate->accepting) {
      response->set_status(DataCenterProto::COMMAND_TARGET_UNAVAILABLE);
      response->set_reason("ModbusRTU 链路正在停止");
      return grpc::Status::OK;
    }
    link.commandGate->active += 1;
    commandGate = link.commandGate;
  }

  struct CommandCompletion {
    std::shared_ptr<CommandGate> gate;
    ~CommandCompletion() {
      if (!gate) {
        return;
      }
      std::lock_guard<std::mutex> lock(gate->mu);
      if (gate->active > 0) {
        gate->active -= 1;
      }
      if (gate->active == 0) {
        gate->cv.notify_all();
      }
    }
  } completion{commandGate};

  DataCenterProto::PointUpdate update;
  update.set_src_conn_id(request.src().conn_id());
  update.set_src_tag(request.src().tag());
  update.set_dst_conn_id(response->dst().conn_id());
  update.set_dst_tag(request.dst().tag());
  update.mutable_value()->CopyFrom(request.value());
  update.set_ts_ms(request.ts_ms());
  update.set_quality(request.quality());

  LOG_INFO("ModbusRTU 收到同步写命令: conn_name={}, conn_id={}, tag={}, value={}, request_id={}",
           connName,
           response->dst().conn_id(),
           point.tag,
           requestedValue,
           request.request_id());
  auto status = executeWriteCommand(connName, config, point, bus, update);
  if (!status.ok()) {
    updateLastError(connName, status.error_message(), LastErrorSource::kCommand);
    if (status.error_code() == grpc::StatusCode::INVALID_ARGUMENT ||
        status.error_code() == grpc::StatusCode::OUT_OF_RANGE) {
      response->set_status(DataCenterProto::COMMAND_REJECTED);
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSUPPORTED_POINT);
      response->set_reason("ModbusRTU 写命令参数非法: " + status.error_message());
    } else if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
      response->set_status(DataCenterProto::COMMAND_TIMEOUT);
      response->set_reason("ModbusRTU 写命令执行超时: " + status.error_message());
    } else if (status.error_code() == grpc::StatusCode::UNAVAILABLE) {
      response->set_status(DataCenterProto::COMMAND_TARGET_UNAVAILABLE);
      response->set_reason("ModbusRTU 写命令通信失败: " + status.error_message());
    } else if (status.error_code() == grpc::StatusCode::FAILED_PRECONDITION) {
      response->set_status(DataCenterProto::COMMAND_REJECTED);
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_BAD_CONFIG);
      response->set_reason("ModbusRTU 从站拒绝写命令: " + status.error_message());
    } else {
      response->set_status(DataCenterProto::COMMAND_INTERNAL_ERROR);
      response->set_reason("ModbusRTU 写命令执行失败: " + status.error_message());
    }
    LOG_WARNING("ModbusRTU 同步写命令失败: conn_name={}, tag={}, status={}, 原因={}",
                connName,
                point.tag,
                static_cast<int>(response->status()),
                status.error_message());
    return grpc::Status::OK;
  }

  clearLastError(connName, LastErrorSource::kCommand);

  response->set_status(DataCenterProto::COMMAND_ACCEPTED);
  response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSPECIFIED);
  response->set_reason("ModbusRTU 同步写命令已执行");
  response->set_accepted_value(requestedValue);
  LOG_INFO("ModbusRTU 同步写命令执行成功: conn_name={}, conn_id={}, tag={}, value={}",
           connName,
           response->dst().conn_id(),
           point.tag,
           requestedValue);
  return grpc::Status::OK;
}

grpc::Status LinkManager::UpsertLink(const ModbusRTUProto::UpsertLinkRequest& request, ModbusRTUProto::LinkInfo* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  if (!request.has_config()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "config 不能为空");
  }
  ModbusRTUProto::LinkConfig normalized;
  auto status = normalizeLinkConfig(request.config(), &normalized);
  if (!status.ok()) {
    return status;
  }

  const auto connName = normalized.conn_name();
  const auto isSerial = normalized.transport_type() == ModbusRTUProto::TRANSPORT_SERIAL;
  const auto serialKey = isSerial ? makeSerialKey(normalized.serial()) : SerialKey{};
  const auto mqttKey = !isSerial ? makeMqttKey(normalized) : MqttKey{};
  ModbusRTUProto::LinksConfig linksConfig;
  ModbusRTUProto::PointTablesConfig pointTablesConfig;
  ModbusRTUProto::LinkInfo info;

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it != linksByName_.end()) {
      if (request.create_only()) {
        return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name 已存在");
      }
      if (it->second.state == ModbusRTUProto::LINK_STATE_RUNNING) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "更新配置前请先停止链路");
      }
      if (it->second.state == ModbusRTUProto::LINK_STATE_PENDING_DELETE) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路处于待删除状态");
      }
      if (isSerial) {
        status = ensureSerialCompatibleLocked(serialKey, connName);
      } else {
        status = ensureMqttCompatibleLocked(mqttKey, connName);
      }
      if (!status.ok()) {
        return status;
      }

      it->second.config = normalized;
      it->second.serialKey = serialKey;
      it->second.mqttKey = mqttKey;
      clearLastErrorLocked(&it->second, LastErrorSource::kLifecycle);
      clearLastErrorLocked(&it->second, LastErrorSource::kCommand);
      clearLastErrorLocked(&it->second, LastErrorSource::kPolling);
      linksConfig = dumpLinksConfigLocked();
      pointTablesConfig = dumpPointTablesConfigLocked();
      status = fillLinkInfoLocked(it->second, &info);
      if (!status.ok()) {
        return status;
      }
    } else {
      if (pendingCreateByName_.contains(connName)) {
        return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name 已存在");
      }
      if (isSerial) {
        status = ensureSerialCompatibleLocked(serialKey, connName);
      } else {
        status = ensureMqttCompatibleLocked(mqttKey, connName);
      }
      if (!status.ok()) {
        return status;
      }
      pendingCreateByName_.insert(connName);
    }
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
    LOG_INFO("ModbusRTU 链路配置更新成功，当前不会自动启动链路连接功能，等待显式调用 StartLink: conn_name={}", connName);
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = linksByName_.find(connName);
      if (it == linksByName_.end()) {
        return makeNotFound(connName);
      }
      status = fillLinkInfoLocked(it->second, out);
      if (!status.ok()) {
        return status;
      }
    }
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
      LOG_ERROR("ModbusRTU 查询 DataCenter 连接失败: conn_name={}, 原因={}", connName, status.error_message());
      return status;
    }
    if (exists) {
      rollbackPendingCreate();
      LOG_WARNING("ModbusRTU 创建连接失败: conn_name={} 已存在于 DataCenter", connName);
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name 已存在");
    }
  }

  DataCenterProto::ConnectionInfo connInfo;
  status = dataCenter_.GetOrCreateConnection(connName, &connInfo);
  if (!status.ok()) {
    rollbackPendingCreate();
    LOG_ERROR("ModbusRTU 获取 DataCenter 连接失败: conn_name={}, 原因={}", connName, status.error_message());
    return status;
  }
  if (connInfo.conn_id() == 0) {
    rollbackPendingCreate();
    LOG_ERROR("ModbusRTU 获取到的 DataCenter conn_id 无效: conn_name={}", connName);
    return grpc::Status(grpc::StatusCode::INTERNAL, "DataCenter 返回 conn_id=0");
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    pendingCreateByName_.erase(connName);
    LinkRuntime link;
    link.config = normalized;
    link.serialKey = serialKey;
    link.mqttKey = mqttKey;
    link.connId = connInfo.conn_id();
    link.state = ModbusRTUProto::LINK_STATE_STOPPED;
    clearLastErrorLocked(&link, LastErrorSource::kLifecycle);
    auto [it, inserted] = linksByName_.emplace(connName, std::move(link));
    if (!inserted) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name 已存在");
    }
    linksConfig = dumpLinksConfigLocked();
    pointTablesConfig = dumpPointTablesConfigLocked();
    status = fillLinkInfoLocked(it->second, out);
    if (!status.ok()) {
      return status;
    }
  }

  status = saveLinksConfig(linksConfig);
  if (!status.ok()) {
    return status;
  }
  status = savePointTablesConfig(pointTablesConfig);
  if (!status.ok()) {
    return status;
  }
  LOG_INFO("ModbusRTU 链路配置创建成功，当前不会自动启动链路连接功能，等待显式调用 StartLink: conn_name={}", connName);
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      return makeNotFound(connName);
    }
    status = fillLinkInfoLocked(it->second, out);
    if (!status.ok()) {
      return status;
    }
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::RenameLink(const std::string& oldConnName,
                                     const std::string& newConnName,
                                     ModbusRTUProto::LinkInfo* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  auto status = validateConnName(oldConnName);
  if (!status.ok()) {
    return status;
  }
  status = validateConnName(newConnName);
  if (!status.ok()) {
    return status;
  }

  uint32_t expectedConnId = 0;
  bool reservedRenameName = false;
  ModbusRTUProto::LinksConfig linksConfig;
  ModbusRTUProto::PointTablesConfig pointTablesConfig;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(oldConnName);
    if (it == linksByName_.end()) {
      return makeNotFound(oldConnName);
    }
    if (oldConnName == newConnName) {
      return fillLinkInfoLocked(it->second, out);
    }
    if (linksByName_.contains(newConnName) || pendingCreateByName_.contains(newConnName)) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name 已存在");
    }
    if (it->second.state == ModbusRTUProto::LINK_STATE_RUNNING) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "更新配置前请先停止链路");
    }
    if (it->second.state == ModbusRTUProto::LINK_STATE_PENDING_DELETE) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路处于待删除状态");
    }

    pendingCreateByName_.insert(newConnName);
    reservedRenameName = true;
    expectedConnId = it->second.connId;
    linksConfig = dumpLinksConfigLocked();
    pointTablesConfig = dumpPointTablesConfigLocked();
  }

  auto releaseRenameReservation = [this, &newConnName, &reservedRenameName]() {
    if (!reservedRenameName) {
      return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    pendingCreateByName_.erase(newConnName);
    reservedRenameName = false;
  };

  if (!renamePersistedLinkConfig(&linksConfig, oldConnName, newConnName)) {
    releaseRenameReservation();
    return grpc::Status(grpc::StatusCode::INTERNAL, "本地链路配置快照缺少待改名连接");
  }
  renamePersistedPointTableConfig(&pointTablesConfig, oldConnName, newConnName);

  DataCenterProto::ConnectionInfo connInfo;
  status = dataCenter_.RenameConnection(oldConnName, newConnName, &connInfo);
  if (!status.ok() && status.error_code() == grpc::StatusCode::NOT_FOUND) {
    bool oldExists = false;
    bool newExists = false;
    auto existsStatus = dataCenter_.ConnectionExists(oldConnName, &oldExists);
    if (!existsStatus.ok()) {
      releaseRenameReservation();
      return existsStatus;
    }
    existsStatus = dataCenter_.ConnectionExists(newConnName, &newExists);
    if (!existsStatus.ok()) {
      releaseRenameReservation();
      return existsStatus;
    }
    if (!oldExists && newExists) {
      auto getStatus = dataCenter_.GetOrCreateConnection(newConnName, &connInfo);
      if (!getStatus.ok()) {
        releaseRenameReservation();
        return getStatus;
      }
      if (connInfo.conn_id() != expectedConnId) {
        releaseRenameReservation();
        return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name 已存在");
      }
      LOG_WARNING("ModbusRTU 检测到 DataCenter 连接已提前完成改名，继续收敛本地配置: old_conn_name={}, new_conn_name={}, conn_id={}",
                  oldConnName,
                  newConnName,
                  connInfo.conn_id());
      status = grpc::Status::OK;
    }
  }
  if (!status.ok()) {
    releaseRenameReservation();
    return status;
  }
  if (connInfo.conn_id() == 0) {
    releaseRenameReservation();
    return grpc::Status(grpc::StatusCode::INTERNAL, "DataCenter 返回 conn_id=0");
  }
  if (connInfo.conn_id() != expectedConnId) {
    releaseRenameReservation();
    return grpc::Status(grpc::StatusCode::INTERNAL, "DataCenter 返回的 conn_id 与本地链路不一致");
  }

  status = saveLinksConfig(linksConfig);
  if (!status.ok()) {
    releaseRenameReservation();
    return status;
  }
  status = savePointTablesConfig(pointTablesConfig);
  if (!status.ok()) {
    releaseRenameReservation();
    return status;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    pendingCreateByName_.erase(newConnName);
    reservedRenameName = false;

    auto oldIt = linksByName_.find(oldConnName);
    if (oldIt == linksByName_.end()) {
      auto newIt = linksByName_.find(newConnName);
      if (newIt == linksByName_.end()) {
        return makeNotFound(oldConnName);
      }
      return fillLinkInfoLocked(newIt->second, out);
    }
    if (linksByName_.contains(newConnName)) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name 已存在");
    }

    auto linkNode = linksByName_.extract(oldConnName);
    linkNode.key() = newConnName;
    linkNode.mapped().config.set_conn_name(newConnName);
    linkNode.mapped().connId = connInfo.conn_id();
    auto insertResult = linksByName_.insert(std::move(linkNode));
    if (!insertResult.inserted) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name 已存在");
    }
    status = fillLinkInfoLocked(insertResult.position->second, out);
    if (!status.ok()) {
      return status;
    }
  }

  LOG_INFO("ModbusRTU 链路改名成功: old_conn_name={}, new_conn_name={}, conn_id={}",
           oldConnName,
           newConnName,
           connInfo.conn_id());
  return grpc::Status::OK;
}

grpc::Status LinkManager::GetLink(const std::string& connName, ModbusRTUProto::LinkInfo* out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto it = linksByName_.find(connName);
  if (it == linksByName_.end()) {
    return makeNotFound(connName);
  }
  return fillLinkInfoLocked(it->second, out);
}

grpc::Status LinkManager::ListLinks(ModbusRTUProto::ListLinksResponse* out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  std::lock_guard<std::mutex> lock(mu_);
  out->Clear();
  for (const auto& [_, link] : linksByName_) {
    auto* elem = out->add_links();
    fillLinkInfoLocked(link, elem);
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::StartLink(const std::string& connName) {
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }
  LOG_INFO("ModbusRTU 开始启动链路功能: conn_name={}", connName);

  ModbusRTUProto::LinkConfig config;
  PointTable pointTable;
  uint32_t connId = 0;
  SerialKey serialKey;
  MqttKey mqttKey;
  std::shared_ptr<Bus> bus;

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      return makeNotFound(connName);
    }
    auto& link = it->second;
    if (link.state == ModbusRTUProto::LINK_STATE_PENDING_DELETE) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路处于待删除状态");
    }
    if (link.state == ModbusRTUProto::LINK_STATE_RUNNING) {
      LOG_INFO("ModbusRTU 启动链路功能跳过: conn_name={}, 原因=链路已在运行", connName);
      return grpc::Status::OK;
    }
    std::string reason;
    if (!isLinkAutoStartReadyLocked(link, &reason)) {
      setLastErrorLocked(&link, reason, LastErrorSource::kLifecycle);
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, reason);
    }
    config = link.config;
    pointTable = link.pointTable;
    connId = link.connId;
    serialKey = link.serialKey;
    mqttKey = link.mqttKey;
  }

  if (config.address_base() == ModbusRTUProto::ADDRESS_BASE_ONE) {
    for (const auto& point : pointTable.Points()) {
      if (point.address == 0) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "address_base=ONE 要求 address >= 1");
      }
    }
  }
  const bool isSerial = config.transport_type() == ModbusRTUProto::TRANSPORT_SERIAL;
  if (!isSerial && !mqttClient_.hasConfig()) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "MQTT 连接参数未配置");
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    if (isSerial) {
      bus = acquireSerialBusLocked(serialKey, config.serial());
    } else {
      bus = acquireMqttBusLocked(mqttKey, config);
    }
  }
  status = bus->Open();
  if (!status.ok()) {
    const auto errorMessage = status.error_message();
    const auto endpoint = isSerial ? config.serial().device() : config.serial_port();
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto released = isSerial ? releaseSerialBusLocked(serialKey) : releaseMqttBusLocked(mqttKey);
      if (released) {
        released->Close();
      }
      auto it = linksByName_.find(connName);
      if (it != linksByName_.end()) {
        setLastErrorLocked(&it->second, errorMessage, LastErrorSource::kLifecycle);
      }
    }
    LOG_ERROR("ModbusRTU 打开链路失败: conn_name={}, 端点={}, 原因={}", connName, endpoint, errorMessage);
    return status;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      auto released = isSerial ? releaseSerialBusLocked(serialKey) : releaseMqttBusLocked(mqttKey);
      if (released) {
        released->Close();
      }
      return makeNotFound(connName);
    }
    auto& link = it->second;
    if (link.state == ModbusRTUProto::LINK_STATE_PENDING_DELETE) {
      auto released = isSerial ? releaseSerialBusLocked(serialKey) : releaseMqttBusLocked(mqttKey);
      if (released) {
        released->Close();
      }
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路处于待删除状态");
    }
    if (link.state == ModbusRTUProto::LINK_STATE_RUNNING) {
      auto released = isSerial ? releaseSerialBusLocked(serialKey) : releaseMqttBusLocked(mqttKey);
      if (released) {
        released->Close();
      }
      LOG_INFO("ModbusRTU 启动链路功能跳过: conn_name={}, 原因=链路已在运行", connName);
      return grpc::Status::OK;
    }
    {
      std::lock_guard<std::mutex> gateLock(link.commandGate->mu);
      link.commandGate->accepting = true;
    }
    link.bus = bus;
    link.state = ModbusRTUProto::LINK_STATE_RUNNING;
    clearLastErrorLocked(&link, LastErrorSource::kLifecycle);
    clearLastErrorLocked(&link, LastErrorSource::kCommand);
    clearLastErrorLocked(&link, LastErrorSource::kPolling);
    link.pollThread = ModuleManager::StartModuleThread(
        ModbusRTULibInfo.LIB_NAME,
        [this, connName, connId, config, pointTable, bus](std::stop_token stopToken) {
          pollLoop(connName, connId, config, pointTable, bus, stopToken);
        });
    startCommandSubscribeLocked(connName, &link);
  }

  if (isSerial) {
    LOG_INFO("ModbusRTU 已启动轮询: conn_name={}, device_id={}, device={}",
             connName,
             config.device_id(),
             config.serial().device());
  } else {
    LOG_INFO("ModbusRTU 已启动 MQTT 透传轮询: conn_name={}, device_id={}, serial_port={}",
             connName,
             config.device_id(),
             config.serial_port());
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::StopLink(const std::string& connName) {
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }

  std::jthread pollThread;
  std::jthread commandThread;
  SerialKey serialKey;
  MqttKey mqttKey;
  std::shared_ptr<Bus> bus;
  std::shared_ptr<grpc::ClientContext> commandContext;
  std::shared_ptr<CommandGate> commandGate;
  bool pendingDelete = false;
  ModbusRTUProto::LinkConfig config;

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      return makeNotFound(connName);
    }
    pendingDelete = (it->second.state == ModbusRTUProto::LINK_STATE_PENDING_DELETE);
    commandThread = std::move(it->second.dcCommandThread);
    commandContext = std::move(it->second.dcCommandContext);
    pollThread = std::move(it->second.pollThread);
    config = it->second.config;
    serialKey = it->second.serialKey;
    mqttKey = it->second.mqttKey;
    bus = it->second.bus;
    commandGate = it->second.commandGate;
    {
      std::lock_guard<std::mutex> gateLock(commandGate->mu);
      commandGate->accepting = false;
    }
    it->second.bus.reset();
    it->second.state = pendingDelete ? ModbusRTUProto::LINK_STATE_PENDING_DELETE : ModbusRTUProto::LINK_STATE_STOPPED;
  }

  if (commandContext) {
    commandContext->TryCancel();
  }
  if (commandThread.joinable()) {
    LOG_INFO("ModbusRTU 停止 DataCenter 命令订阅: conn_name={}", connName);
    commandThread.request_stop();
    commandThread.join();
  }

  if (pollThread.joinable()) {
    pollThread.request_stop();
    pollThread.join();
  }

  if (commandGate) {
    std::unique_lock<std::mutex> gateLock(commandGate->mu);
    commandGate->cv.wait(gateLock, [&commandGate]() { return commandGate->active == 0; });
  }

  if (bus) {
    std::shared_ptr<Bus> released;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (config.transport_type() == ModbusRTUProto::TRANSPORT_SERIAL) {
        released = releaseSerialBusLocked(serialKey);
      } else {
        released = releaseMqttBusLocked(mqttKey);
      }
    }
    if (released) {
      released->Close();
    }
  }

  if (config.transport_type() == ModbusRTUProto::TRANSPORT_SERIAL) {
    LOG_INFO("ModbusRTU 已停止轮询: conn_name={}", connName);
  } else {
    LOG_INFO("ModbusRTU 已停止 MQTT 透传轮询: conn_name={}", connName);
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::DeleteLink(const std::string& connName) {
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }

  status = StopLink(connName);
  if (!status.ok() && status.error_code() != grpc::StatusCode::NOT_FOUND) {
    return status;
  }

  grpc::Status dc = dataCenter_.DeleteConnection(connName);
  if (!dc.ok() && dc.error_code() != grpc::StatusCode::NOT_FOUND) {
    ModbusRTUProto::LinksConfig linksConfig;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = linksByName_.find(connName);
      if (it != linksByName_.end()) {
        it->second.state = ModbusRTUProto::LINK_STATE_PENDING_DELETE;
        setLastErrorLocked(&it->second, dc.error_message(), LastErrorSource::kLifecycle);
        linksConfig = dumpLinksConfigLocked();
      }
    }
    if (linksConfig.links_size() > 0) {
      auto saveStatus = saveLinksConfig(linksConfig);
      if (!saveStatus.ok()) {
        return saveStatus;
      }
    }
    return dc;
  }

  ModbusRTUProto::LinksConfig linksConfig;
  ModbusRTUProto::PointTablesConfig pointTablesConfig;
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

grpc::Status LinkManager::UpsertPointTable(const ModbusRTUProto::UpsertPointTableRequest& request) {
  auto status = validateConnName(request.conn_name());
  if (!status.ok()) {
    return status;
  }

  uint32_t connId = 0;
  PointTable current;
  ModbusRTUProto::PointTablesConfig pointTablesConfig;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(request.conn_name());
    if (it == linksByName_.end()) {
      return makeNotFound(request.conn_name());
    }
    if (it->second.state == ModbusRTUProto::LINK_STATE_RUNNING) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "更新点表前请先停止链路");
    }
    if (it->second.state == ModbusRTUProto::LINK_STATE_PENDING_DELETE) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路处于待删除状态");
    }
    connId = it->second.connId;
    current = it->second.pointTable;
  }

  PointTable next = current;
  status = next.Upsert(request.points(), request.replace());
  if (!status.ok()) {
    return status;
  }

  auto tags = next.Tags();
  status = dataCenter_.UpsertConnTags(connId, tags, true);
  if (!status.ok()) {
    return status;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(request.conn_name());
    if (it == linksByName_.end()) {
      return makeNotFound(request.conn_name());
    }
    it->second.pointTable = std::move(next);
    it->second.pointTableConfigured = !it->second.pointTable.Points().empty();
    pointTablesConfig = dumpPointTablesConfigLocked();
  }
  status = savePointTablesConfig(pointTablesConfig);
  if (!status.ok()) {
    return status;
  }
  LOG_INFO("ModbusRTU 点表配置更新成功，当前不会自动启动链路连接功能，等待显式调用 StartLink: conn_name={}", request.conn_name());
  return grpc::Status::OK;
}

grpc::Status LinkManager::GetPointTable(const std::string& connName, ModbusRTUProto::PointTable* out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto it = linksByName_.find(connName);
  if (it == linksByName_.end()) {
    return makeNotFound(connName);
  }
  it->second.pointTable.ToProto(connName, out);
  return grpc::Status::OK;
}

void LinkManager::pollLoop(std::string connName,
                           uint32_t connId,
                           ModbusRTUProto::LinkConfig config,
                           PointTable pointTable,
                           std::shared_ptr<Bus> bus,
                           std::stop_token stopToken) {
  const auto interval = std::chrono::milliseconds(config.poll_interval_ms());
  const auto points = pointTable.Points();
  std::unordered_map<std::string, double> lastReportedByTag;
  lastReportedByTag.reserve(points.size());
  const auto readPlanMode = config.has_read_plan() ? config.read_plan().mode() : ModbusRTUProto::READ_PLAN_MODE_POINT;
  const bool useExplicitPlan = (readPlanMode == ModbusRTUProto::READ_PLAN_MODE_EXPLICIT &&
                                config.read_plan().blocks_size() > 0);
  if (useExplicitPlan) {
    LOG_INFO("ModbusRTU 轮询使用显式抄读方案: conn_name={}, blocks={}, interval={}ms",
             connName,
             config.read_plan().blocks_size(),
             config.poll_interval_ms());
  }
  LOG_INFO("ModbusRTU 轮询开始: conn_name={}, points={}, interval={}ms", connName, points.size(), config.poll_interval_ms());

  if (!useExplicitPlan) {
    while (!stopToken.stop_requested()) {
      uint64_t pollingErrorRevision = 0;
      {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = linksByName_.find(connName);
        if (it != linksByName_.end()) {
          pollingErrorRevision = it->second.pollingErrorRevision;
        }
      }
      for (const auto& point : points) {
        if (stopToken.stop_requested()) {
          break;
        }
        if (isWriteFunction(point.function)) {
          continue;
        }

        uint32_t address = point.address;
        if (config.address_base() == ModbusRTUProto::ADDRESS_BASE_ONE) {
          if (address == 0) {
            updateLastError(connName, "address_base=ONE 但 address 为 0");
            LOG_WARNING("ModbusRTU 点表地址非法: conn_name={}, tag={}, address=0", connName, point.tag);
            continue;
          }
          address -= 1;
        }

        grpc::Status status;
        if (point.function == ModbusRTUProto::FUNCTION_READ_COILS) {
          bool value = false;
          status = bus->ReadCoil(static_cast<uint8_t>(config.device_id()), static_cast<uint16_t>(address), &value);
          if (status.ok()) {
            auto dc = dataCenter_.PublishBool(connId, point.tag, value, DataCenterProto::QUALITY_GOOD, 0);
            if (!dc.ok()) {
              updateLastError(connName, dc.error_message());
              LOG_ERROR("ModbusRTU 发布点值失败: conn_name={}, tag={}, 原因={}", connName, point.tag, dc.error_message());
            }
          }
        } else if (point.function == ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS ||
                   point.function == ModbusRTUProto::FUNCTION_READ_INPUT_REGISTERS) {
          const bool isInputRegisters = point.function == ModbusRTUProto::FUNCTION_READ_INPUT_REGISTERS;
          std::optional<double> engValue;
          if (isRegisterBitPoint(point)) {
            uint32_t raw = 0;
            if (point.regCount == 1) {
              uint16_t value = 0;
              status = isInputRegisters
                  ? bus->ReadInputRegister(static_cast<uint8_t>(config.device_id()),
                                           static_cast<uint16_t>(address),
                                           &value)
                  : bus->ReadHoldingRegister(static_cast<uint8_t>(config.device_id()),
                                             static_cast<uint16_t>(address),
                                             &value);
              if (status.ok()) {
                if (point.byteOrder == ModbusRTUProto::BYTE_ORDER_BA) {
                  value = swapWordBytes(value);
                }
                raw = value;
              }
            } else if (point.regCount == 2) {
              std::vector<uint16_t> values;
              status = isInputRegisters
                  ? bus->ReadInputRegisters(static_cast<uint8_t>(config.device_id()),
                                            static_cast<uint16_t>(address),
                                            2,
                                            &values)
                  : bus->ReadHoldingRegisters(static_cast<uint8_t>(config.device_id()),
                                              static_cast<uint16_t>(address),
                                              2,
                                              &values);
              if (status.ok()) {
                if (values.size() != 2) {
                  status = grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                        isInputRegisters ? "输入寄存器响应数量异常" : "保持寄存器响应数量异常");
                } else {
                  raw = decodeUint32(values[0], values[1], point.wordOrder, point.byteOrder);
                }
              }
            } else {
              status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "寄存器 BOOL 点位 reg_count 不支持");
            }
            if (status.ok()) {
              const bool bitValue = ((raw >> point.bitIndex.value()) & 0x01u) != 0;
              LOG_DEBUG("ModbusRTU 读取寄存器bit遥信: conn_name={}, tag={}, raw={}, bit_index={}, value={}",
                        connName,
                        point.tag,
                        raw,
                        point.bitIndex.value(),
                        bitValue);
              auto dc = dataCenter_.PublishBool(connId, point.tag, bitValue, DataCenterProto::QUALITY_GOOD, 0);
              if (!dc.ok()) {
                updateLastError(connName, dc.error_message());
                LOG_ERROR("ModbusRTU 发布点值失败: conn_name={}, tag={}, 原因={}", connName, point.tag, dc.error_message());
              }
            }
          } else if (is16BitRegisterType(point.type)) {
            uint16_t value = 0;
            status = isInputRegisters
                ? bus->ReadInputRegister(static_cast<uint8_t>(config.device_id()),
                                         static_cast<uint16_t>(address),
                                         &value)
                : bus->ReadHoldingRegister(static_cast<uint8_t>(config.device_id()),
                                           static_cast<uint16_t>(address),
                                           &value);
            if (status.ok()) {
              if (point.type == ModbusRTUProto::DATA_TYPE_UINT16) {
                if (point.byteOrder == ModbusRTUProto::BYTE_ORDER_BA) {
                  value = swapWordBytes(value);
                }
                LOG_DEBUG("ModbusRTU 读取UINT16寄存器: conn_name={}, tag={}, raw={}, byte_order={}",
                          connName,
                          point.tag,
                          value,
                          static_cast<int>(point.byteOrder));
                engValue = static_cast<double>(value) * point.scale + point.offset;
              } else {
                const int16_t signedValue = decodeInt16(value, point.byteOrder);
                LOG_DEBUG("ModbusRTU 读取INT16寄存器: conn_name={}, tag={}, raw={}, byte_order={}",
                          connName,
                          point.tag,
                          signedValue,
                          static_cast<int>(point.byteOrder));
                engValue = static_cast<double>(signedValue) * point.scale + point.offset;
              }
            }
          } else if (is32BitRegisterType(point.type)) {
            std::vector<uint16_t> values;
            status = isInputRegisters
                ? bus->ReadInputRegisters(static_cast<uint8_t>(config.device_id()),
                                          static_cast<uint16_t>(address),
                                          2,
                                          &values)
                : bus->ReadHoldingRegisters(static_cast<uint8_t>(config.device_id()),
                                            static_cast<uint16_t>(address),
                                            2,
                                            &values);
            if (status.ok()) {
              if (values.size() != 2) {
                status = grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                      isInputRegisters ? "输入寄存器响应数量异常" : "保持寄存器响应数量异常");
              } else {
                if (point.type == ModbusRTUProto::DATA_TYPE_UINT32) {
                  const uint32_t raw = decodeUint32(values[0], values[1], point.wordOrder, point.byteOrder);
                  LOG_DEBUG("ModbusRTU 读取UINT32寄存器: conn_name={}, tag={}, raw={}, reg0={}, reg1={}, word_order={}, byte_order={}",
                            connName,
                            point.tag,
                            raw,
                            values[0],
                            values[1],
                            static_cast<int>(point.wordOrder),
                            static_cast<int>(point.byteOrder));
                  engValue = static_cast<double>(raw) * point.scale + point.offset;
                } else {
                  const int32_t raw = decodeInt32(values[0], values[1], point.wordOrder, point.byteOrder);
                  LOG_DEBUG("ModbusRTU 读取INT32寄存器: conn_name={}, tag={}, raw={}, reg0={}, reg1={}, word_order={}, byte_order={}",
                            connName,
                            point.tag,
                            raw,
                            values[0],
                            values[1],
                            static_cast<int>(point.wordOrder),
                            static_cast<int>(point.byteOrder));
                  engValue = static_cast<double>(raw) * point.scale + point.offset;
                }
              }
            }
          } else {
            status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "寄存器点位类型不支持");
          }
          if (status.ok() && engValue.has_value()) {
            std::optional<double> last;
            auto lastIt = lastReportedByTag.find(point.tag);
            if (lastIt != lastReportedByTag.end()) {
              last = lastIt->second;
            }
            if (!shouldReport(engValue.value(), point.deadband, last)) {
              LOG_DEBUG("ModbusRTU 死区过滤上报: conn_name={}, tag={}, value={}, last={}, 死区={}",
                        connName,
                        point.tag,
                        engValue.value(),
                        last.value(),
                        point.deadband);
              continue;
            }
            auto dc = dataCenter_.PublishDouble(connId, point.tag, engValue.value(), DataCenterProto::QUALITY_GOOD, 0);
            if (!dc.ok()) {
              updateLastError(connName, dc.error_message());
              LOG_ERROR("ModbusRTU 发布点值失败: conn_name={}, tag={}, 原因={}", connName, point.tag, dc.error_message());
            } else {
              lastReportedByTag[point.tag] = engValue.value();
            }
          }
        } else {
          status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "不支持的功能码");
        }

        if (!status.ok()) {
          updateLastError(connName, status.error_message());
          LOG_WARNING("ModbusRTU 轮询点失败: conn_name={}, tag={}, 原因={}", connName, point.tag, status.error_message());
        }
      }

      if (stopToken.stop_requested()) {
        break;
      }
      bool cycleSucceeded = false;
      {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = linksByName_.find(connName);
        cycleSucceeded = it != linksByName_.end() && it->second.pollingErrorRevision == pollingErrorRevision;
      }
      if (cycleSucceeded) {
        clearLastError(connName, LastErrorSource::kPolling);
      }
      std::this_thread::sleep_for(interval);
    }

    LOG_INFO("ModbusRTU 轮询结束: conn_name={}", connName);
    return;
  }

  struct NormalizedPoint {
    std::string tag;
    ModbusRTUProto::FunctionCode function = ModbusRTUProto::FUNCTION_UNSPECIFIED;
    ModbusRTUProto::DataType type = ModbusRTUProto::DATA_TYPE_UNSPECIFIED;
    uint32_t address = 0;
    uint32_t regCount = 1;
    ModbusRTUProto::WordOrder wordOrder = ModbusRTUProto::WORD_ORDER_HL;
    ModbusRTUProto::ByteOrder byteOrder = ModbusRTUProto::BYTE_ORDER_AB;
    double scale = 1.0;
    double offset = 0.0;
    double deadband = 0.0;
    std::optional<uint32_t> bitIndex;
  };

  std::vector<NormalizedPoint> registerPoints;
  std::vector<NormalizedPoint> coilPoints;
  registerPoints.reserve(points.size());
  coilPoints.reserve(points.size());

  for (const auto& point : points) {
    uint32_t address = point.address;
    if (config.address_base() == ModbusRTUProto::ADDRESS_BASE_ONE) {
      if (address == 0) {
        updateLastError(connName, "address_base=ONE 但 address 为 0");
        LOG_WARNING("ModbusRTU 点表地址非法: conn_name={}, tag={}, address=0", connName, point.tag);
        continue;
      }
      address -= 1;
    }

    NormalizedPoint normalized;
    normalized.tag = point.tag;
    normalized.function = point.function;
    normalized.type = point.type;
    normalized.address = address;
    normalized.regCount = point.regCount;
    normalized.wordOrder = point.wordOrder;
    normalized.byteOrder = point.byteOrder;
    normalized.scale = point.scale;
    normalized.offset = point.offset;
    normalized.deadband = point.deadband;
    normalized.bitIndex = point.bitIndex;

    if (isReadRegisterFunction(point.function)) {
      registerPoints.push_back(std::move(normalized));
    } else if (point.function == ModbusRTUProto::FUNCTION_READ_COILS) {
      coilPoints.push_back(std::move(normalized));
    } else if (!isWriteFunction(point.function)) {
      LOG_WARNING("ModbusRTU 点表功能码不支持: conn_name={}, tag={}, function={}",
                  connName,
                  point.tag,
                  static_cast<int>(point.function));
    }
  }

  if (!registerPoints.empty()) {
    size_t uncoveredCount = 0;
    std::vector<std::string> sampleTags;
    sampleTags.reserve(5);
    for (const auto& point : registerPoints) {
      bool covered = false;
      for (const auto& block : config.read_plan().blocks()) {
        if (block.function() != point.function) {
          continue;
        }
        uint32_t start = block.start();
        if (config.address_base() == ModbusRTUProto::ADDRESS_BASE_ONE) {
          if (start == 0) {
            continue;
          }
          start -= 1;
        }
        const uint32_t end = start + block.quantity() - 1;
        if (point.address >= start && (point.address + point.regCount - 1) <= end) {
          covered = true;
          break;
        }
      }
      if (!covered) {
        uncoveredCount += 1;
        if (sampleTags.size() < 5) {
          sampleTags.push_back(point.tag);
        }
      }
    }
    if (uncoveredCount > 0) {
      std::string sample;
      for (size_t i = 0; i < sampleTags.size(); ++i) {
        if (i > 0) {
          sample.append(",");
        }
        sample.append(sampleTags[i]);
      }
      LOG_WARNING("ModbusRTU 显式抄读区间未覆盖点表: conn_name={}, 未覆盖点数={}, 示例={}",
                  connName,
                  uncoveredCount,
                  sample);
    }
  }

  while (!stopToken.stop_requested()) {
    uint64_t pollingErrorRevision = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = linksByName_.find(connName);
      if (it != linksByName_.end()) {
        pollingErrorRevision = it->second.pollingErrorRevision;
      }
    }
    std::unordered_set<std::string> processedTags;
    processedTags.reserve(registerPoints.size());

    for (const auto& block : config.read_plan().blocks()) {
      if (stopToken.stop_requested()) {
        break;
      }
      if (block.function() != ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS &&
          block.function() != ModbusRTUProto::FUNCTION_READ_INPUT_REGISTERS) {
        updateLastError(connName, "显式抄读区间功能码不支持");
        LOG_WARNING("ModbusRTU 显式抄读区间功能码不支持: conn_name={}, function={}",
                    connName,
                    static_cast<int>(block.function()));
        continue;
      }
      const bool isInputRegisters = block.function() == ModbusRTUProto::FUNCTION_READ_INPUT_REGISTERS;
      const auto registerName = isInputRegisters ? "输入寄存器" : "保持寄存器";
      if (block.quantity() == 0) {
        updateLastError(connName, "read_plan.blocks.quantity 不能为空");
        LOG_WARNING("ModbusRTU 显式抄读区间数量非法: conn_name={}, quantity=0", connName);
        continue;
      }
      uint32_t start = block.start();
      if (config.address_base() == ModbusRTUProto::ADDRESS_BASE_ONE) {
        if (start == 0) {
          updateLastError(connName, "read_plan.blocks.start 不能为 0（address_base=ONE）");
          LOG_WARNING("ModbusRTU 显式抄读区间起始地址非法: conn_name={}, start=0", connName);
          continue;
        }
        start -= 1;
      }

      LOG_DEBUG("ModbusRTU 轮询区间抄读: conn_name={}, start={}, quantity={}",
                connName,
                start,
                block.quantity());

      std::vector<uint16_t> values;
      auto status = isInputRegisters
          ? bus->ReadInputRegisters(static_cast<uint8_t>(config.device_id()),
                                    static_cast<uint16_t>(start),
                                    static_cast<uint16_t>(block.quantity()),
                                    &values)
          : bus->ReadHoldingRegisters(static_cast<uint8_t>(config.device_id()),
                                      static_cast<uint16_t>(start),
                                      static_cast<uint16_t>(block.quantity()),
                                      &values);
      if (!status.ok()) {
        updateLastError(connName, status.error_message());
        LOG_WARNING("ModbusRTU 轮询区间抄读失败: conn_name={}, start={}, quantity={}, 原因={}",
                    connName,
                    start,
                    block.quantity(),
                    status.error_message());
        continue;
      }
      if (values.size() != block.quantity()) {
        updateLastError(connName, std::string(registerName) + "响应数量异常");
        LOG_WARNING("ModbusRTU 轮询区间响应数量异常: conn_name={}, start={}, quantity={}, 实际={}",
                    connName,
                    start,
                    block.quantity(),
                    values.size());
        continue;
      }

      size_t matchedPoints = 0;
      const uint32_t end = start + block.quantity() - 1;
      for (const auto& point : registerPoints) {
        if (point.function != block.function()) {
          continue;
        }
        if (point.address < start || (point.address + point.regCount - 1) > end) {
          continue;
        }
        if (!processedTags.insert(point.tag).second) {
          LOG_WARNING("ModbusRTU 显式抄读区间重叠: conn_name={}, tag={} 已被重复抄读",
                      connName,
                      point.tag);
          continue;
        }
        const size_t offset = static_cast<size_t>(point.address - start);
        double engValue = 0.0;
        if (point.type == ModbusRTUProto::DATA_TYPE_BOOL && point.bitIndex.has_value()) {
          uint32_t raw = 0;
          if (point.regCount == 1) {
            if (offset >= values.size()) {
              updateLastError(connName, std::string(registerName) + "地址溢出");
              LOG_WARNING("ModbusRTU 轮询区间解码越界: conn_name={}, tag={}, offset={}",
                          connName,
                          point.tag,
                          offset);
              continue;
            }
            uint16_t value = values[offset];
            if (point.byteOrder == ModbusRTUProto::BYTE_ORDER_BA) {
              value = swapWordBytes(value);
            }
            raw = value;
          } else if (point.regCount == 2) {
            if (offset + 1 >= values.size()) {
              updateLastError(connName, std::string(registerName) + "地址溢出");
              LOG_WARNING("ModbusRTU 轮询区间解码越界: conn_name={}, tag={}, offset={}",
                          connName,
                          point.tag,
                          offset);
              continue;
            }
            raw = decodeUint32(values[offset], values[offset + 1], point.wordOrder, point.byteOrder);
          } else {
            updateLastError(connName, "寄存器 BOOL 点位 reg_count 不支持");
            LOG_WARNING("ModbusRTU 轮询区间寄存器BOOL点位 reg_count 不支持: conn_name={}, tag={}", connName, point.tag);
            continue;
          }
          const bool bitValue = ((raw >> point.bitIndex.value()) & 0x01u) != 0;
          LOG_DEBUG("ModbusRTU 读取寄存器bit遥信: conn_name={}, tag={}, raw={}, bit_index={}, value={}",
                    connName,
                    point.tag,
                    raw,
                    point.bitIndex.value(),
                    bitValue);
          auto dc = dataCenter_.PublishBool(connId, point.tag, bitValue, DataCenterProto::QUALITY_GOOD, 0);
          if (!dc.ok()) {
            updateLastError(connName, dc.error_message());
            LOG_ERROR("ModbusRTU 发布点值失败: conn_name={}, tag={}, 原因={}", connName, point.tag, dc.error_message());
          }
          matchedPoints += 1;
          continue;
        }
        if (point.type == ModbusRTUProto::DATA_TYPE_UINT16) {
          if (offset >= values.size()) {
            updateLastError(connName, std::string(registerName) + "地址溢出");
            LOG_WARNING("ModbusRTU 轮询区间解码越界: conn_name={}, tag={}, offset={}",
                        connName,
                        point.tag,
                        offset);
            continue;
          }
          uint16_t value = values[offset];
          if (point.byteOrder == ModbusRTUProto::BYTE_ORDER_BA) {
            value = swapWordBytes(value);
          }
          LOG_DEBUG("ModbusRTU 读取UINT16寄存器: conn_name={}, tag={}, raw={}, byte_order={}",
                    connName,
                    point.tag,
                    value,
                    static_cast<int>(point.byteOrder));
          engValue = static_cast<double>(value) * point.scale + point.offset;
        } else if (point.type == ModbusRTUProto::DATA_TYPE_INT16) {
          if (offset >= values.size()) {
            updateLastError(connName, std::string(registerName) + "地址溢出");
            LOG_WARNING("ModbusRTU 轮询区间解码越界: conn_name={}, tag={}, offset={}",
                        connName,
                        point.tag,
                        offset);
            continue;
          }
          const int16_t value = decodeInt16(values[offset], point.byteOrder);
          LOG_DEBUG("ModbusRTU 读取INT16寄存器: conn_name={}, tag={}, raw={}, byte_order={}",
                    connName,
                    point.tag,
                    value,
                    static_cast<int>(point.byteOrder));
          engValue = static_cast<double>(value) * point.scale + point.offset;
        } else if (point.type == ModbusRTUProto::DATA_TYPE_UINT32) {
          if (offset + 1 >= values.size()) {
            updateLastError(connName, std::string(registerName) + "地址溢出");
            LOG_WARNING("ModbusRTU 轮询区间解码越界: conn_name={}, tag={}, offset={}",
                        connName,
                        point.tag,
                        offset);
            continue;
          }
          const uint16_t first = values[offset];
          const uint16_t second = values[offset + 1];
          const uint32_t raw = decodeUint32(first, second, point.wordOrder, point.byteOrder);
          LOG_DEBUG("ModbusRTU 读取UINT32寄存器: conn_name={}, tag={}, raw={}, reg0={}, reg1={}, word_order={}, byte_order={}",
                    connName,
                    point.tag,
                    raw,
                    first,
                    second,
                    static_cast<int>(point.wordOrder),
                    static_cast<int>(point.byteOrder));
          engValue = static_cast<double>(raw) * point.scale + point.offset;
        } else if (point.type == ModbusRTUProto::DATA_TYPE_INT32) {
          if (offset + 1 >= values.size()) {
            updateLastError(connName, std::string(registerName) + "地址溢出");
            LOG_WARNING("ModbusRTU 轮询区间解码越界: conn_name={}, tag={}, offset={}",
                        connName,
                        point.tag,
                        offset);
            continue;
          }
          const uint16_t first = values[offset];
          const uint16_t second = values[offset + 1];
          const int32_t raw = decodeInt32(first, second, point.wordOrder, point.byteOrder);
          LOG_DEBUG("ModbusRTU 读取INT32寄存器: conn_name={}, tag={}, raw={}, reg0={}, reg1={}, word_order={}, byte_order={}",
                    connName,
                    point.tag,
                    raw,
                    first,
                    second,
                    static_cast<int>(point.wordOrder),
                    static_cast<int>(point.byteOrder));
          engValue = static_cast<double>(raw) * point.scale + point.offset;
        } else {
          updateLastError(connName, "寄存器点位类型不支持");
          LOG_WARNING("ModbusRTU 轮询区间点位类型不支持: conn_name={}, tag={}", connName, point.tag);
          continue;
        }
        std::optional<double> last;
        auto lastIt = lastReportedByTag.find(point.tag);
        if (lastIt != lastReportedByTag.end()) {
          last = lastIt->second;
        }
        if (!shouldReport(engValue, point.deadband, last)) {
          LOG_DEBUG("ModbusRTU 死区过滤上报: conn_name={}, tag={}, value={}, last={}, 死区={}",
                    connName,
                    point.tag,
                    engValue,
                    last.value(),
                    point.deadband);
          continue;
        }
        auto dc = dataCenter_.PublishDouble(connId, point.tag, engValue, DataCenterProto::QUALITY_GOOD, 0);
        if (!dc.ok()) {
          updateLastError(connName, dc.error_message());
          LOG_ERROR("ModbusRTU 发布点值失败: conn_name={}, tag={}, 原因={}", connName, point.tag, dc.error_message());
        } else {
          lastReportedByTag[point.tag] = engValue;
        }
        matchedPoints += 1;
      }

      if (matchedPoints == 0) {
        LOG_DEBUG("ModbusRTU 轮询区间未命中点表: conn_name={}, start={}, quantity={}",
                  connName,
                  start,
                  block.quantity());
      }
    }

    for (const auto& point : coilPoints) {
      if (stopToken.stop_requested()) {
        break;
      }
      bool value = false;
      auto status = bus->ReadCoil(static_cast<uint8_t>(config.device_id()),
                                  static_cast<uint16_t>(point.address),
                                  &value);
      if (status.ok()) {
        auto dc = dataCenter_.PublishBool(connId, point.tag, value, DataCenterProto::QUALITY_GOOD, 0);
        if (!dc.ok()) {
          updateLastError(connName, dc.error_message());
          LOG_ERROR("ModbusRTU 发布点值失败: conn_name={}, tag={}, 原因={}", connName, point.tag, dc.error_message());
        }
      } else {
        updateLastError(connName, status.error_message());
        LOG_WARNING("ModbusRTU 轮询点失败: conn_name={}, tag={}, 原因={}", connName, point.tag, status.error_message());
      }
    }

    if (stopToken.stop_requested()) {
      break;
    }
    bool cycleSucceeded = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = linksByName_.find(connName);
      cycleSucceeded = it != linksByName_.end() && it->second.pollingErrorRevision == pollingErrorRevision;
    }
    if (cycleSucceeded) {
      clearLastError(connName, LastErrorSource::kPolling);
    }
    std::this_thread::sleep_for(interval);
  }

  LOG_INFO("ModbusRTU 轮询结束: conn_name={}", connName);
}

void LinkManager::setLastErrorLocked(LinkRuntime* link,
                                      const std::string& error,
                                      LastErrorSource source) {
  if (link == nullptr) {
    return;
  }
  switch (source) {
    case LastErrorSource::kPolling:
      link->pollingError = error;
      link->pollingErrorRevision += 1;
      break;
    case LastErrorSource::kCommand:
      link->commandError = error;
      break;
    case LastErrorSource::kLifecycle:
      link->lifecycleError = error;
      break;
    case LastErrorSource::kNone:
      return;
  }

  if (!link->lifecycleError.empty()) {
    link->lastError = link->lifecycleError;
  } else if (!link->commandError.empty()) {
    link->lastError = link->commandError;
  } else {
    link->lastError = link->pollingError;
  }
}

void LinkManager::clearLastErrorLocked(LinkRuntime* link, LastErrorSource source) {
  if (link == nullptr) {
    return;
  }
  switch (source) {
    case LastErrorSource::kPolling:
      link->pollingError.clear();
      break;
    case LastErrorSource::kCommand:
      link->commandError.clear();
      break;
    case LastErrorSource::kLifecycle:
      link->lifecycleError.clear();
      break;
    case LastErrorSource::kNone:
      return;
  }

  if (!link->lifecycleError.empty()) {
    link->lastError = link->lifecycleError;
  } else if (!link->commandError.empty()) {
    link->lastError = link->commandError;
  } else {
    link->lastError = link->pollingError;
  }
}

void LinkManager::updateLastError(const std::string& connName,
                                   const std::string& error,
                                   LastErrorSource source) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = linksByName_.find(connName);
  if (it == linksByName_.end()) {
    return;
  }
  setLastErrorLocked(&it->second, error, source);
}

void LinkManager::clearLastError(const std::string& connName, LastErrorSource source) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = linksByName_.find(connName);
  if (it == linksByName_.end()) {
    return;
  }
  std::string previous;
  switch (source) {
    case LastErrorSource::kPolling:
      previous = it->second.pollingError;
      break;
    case LastErrorSource::kCommand:
      previous = it->second.commandError;
      break;
    case LastErrorSource::kLifecycle:
      previous = it->second.lifecycleError;
      break;
    case LastErrorSource::kNone:
      break;
  }
  clearLastErrorLocked(&it->second, source);
  if (!previous.empty()) {
    LOG_INFO("ModbusRTU 错误已恢复: conn_name={}, 错误来源={}, 原因={}",
             connName,
             source == LastErrorSource::kPolling ? "轮询" :
                 source == LastErrorSource::kCommand ? "写命令" : "生命周期",
             previous);
  }
}

}  // namespace ModbusRTU
