#include "ModbusRTULinkManager.h"

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
constexpr uint16_t kMaxReadCoilsQuantity = 2000;
constexpr uint16_t kMaxReadHoldingRegistersQuantity = 125;
constexpr uint16_t kMaxReadInputRegistersQuantity = 125;
constexpr uint16_t kMaxWriteMultipleRegistersQuantity = 123;
constexpr uint8_t kFunctionReadCoils = 0x01;
constexpr uint8_t kFunctionReadHoldingRegisters = 0x03;
constexpr uint8_t kFunctionReadInputRegisters = 0x04;
constexpr uint8_t kExceptionIllegalFunction = 0x01;
constexpr uint8_t kExceptionIllegalDataAddress = 0x02;
constexpr uint8_t kExceptionIllegalDataValue = 0x03;
constexpr uint8_t kExceptionSlaveDeviceFailure = 0x04;

bool is16BitRegisterType(ModbusRTUProto::DataType type) {
  return type == ModbusRTUProto::DATA_TYPE_UINT16 || type == ModbusRTUProto::DATA_TYPE_INT16;
}

bool is32BitRegisterType(ModbusRTUProto::DataType type) {
  return type == ModbusRTUProto::DATA_TYPE_UINT32 || type == ModbusRTUProto::DATA_TYPE_INT32;
}

bool isSignedRegisterType(ModbusRTUProto::DataType type) {
  return type == ModbusRTUProto::DATA_TYPE_INT16 || type == ModbusRTUProto::DATA_TYPE_INT32;
}

bool isReadRegisterFunction(ModbusRTUProto::FunctionCode function) {
  return function == ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS ||
      function == ModbusRTUProto::FUNCTION_READ_INPUT_REGISTERS;
}

bool isWriteSingleRegisterFunction(ModbusRTUProto::FunctionCode function) {
  return function == ModbusRTUProto::FUNCTION_WRITE_SINGLE_REGISTER;
}

bool isWriteMultipleRegistersFunction(ModbusRTUProto::FunctionCode function) {
  return function == ModbusRTUProto::FUNCTION_WRITE_MULTIPLE_REGISTERS;
}

bool isWriteRegisterFunction(ModbusRTUProto::FunctionCode function) {
  return isWriteSingleRegisterFunction(function) || isWriteMultipleRegistersFunction(function);
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

bool hasSignedRegisterPoints(const PointTable& pointTable) {
  for (const auto& point : pointTable.Points()) {
    if (!isWriteRegisterFunction(point.function) && isSignedRegisterType(point.type)) {
      return true;
    }
  }
  return false;
}

bool hasWriteRegisterPoints(const PointTable& pointTable) {
  for (const auto& point : pointTable.Points()) {
    if (isWriteRegisterFunction(point.function)) {
      return true;
    }
  }
  return false;
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

  double rawValue = 0.0;
  if (!reverseScale(engValue, point.scale, point.offset, &rawValue)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "工程量反向缩放失败");
  }

  outValues->clear();
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

LinkManager::LinkManager(std::string moduleName) :
  dataCenter_(moduleName),
  mqttClient_(std::move(moduleName)) {}

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
  response->set_ok(true);
  response->set_message("MQTT 配置更新成功");
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
  if (out->mode() == ModbusRTUProto::LINK_MODE_UNSPECIFIED) {
    out->set_mode(ModbusRTUProto::LINK_MODE_MASTER);
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
    if (out->mode() != ModbusRTUProto::LINK_MODE_MASTER) {
      return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "TRANSPORT_MQTT_UART 当前仅支持主站模式");
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
  if (out->mode() != ModbusRTUProto::LINK_MODE_MASTER &&
      out->mode() != ModbusRTUProto::LINK_MODE_SLAVE) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "link mode 非法");
  }
  if (out->slave_id() == 0 || out->slave_id() > 247) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "slave_id 必须在 [1,247] 范围内");
  }
  if (out->has_read_plan()) {
    auto *plan = out->mutable_read_plan();
    if (plan->mode() == ModbusRTUProto::READ_PLAN_MODE_UNSPECIFIED) {
      plan->set_mode(plan->blocks_size() > 0 ? ModbusRTUProto::READ_PLAN_MODE_EXPLICIT
                                             : ModbusRTUProto::READ_PLAN_MODE_POINT);
    }
    if (plan->mode() == ModbusRTUProto::READ_PLAN_MODE_EXPLICIT) {
      if (out->mode() != ModbusRTUProto::LINK_MODE_MASTER) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "read_plan 仅支持主站模式");
      }
      if (plan->blocks_size() == 0) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "read_plan.blocks 不能为空");
      }
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
    } else if (plan->mode() == ModbusRTUProto::READ_PLAN_MODE_POINT) {
      if (plan->blocks_size() > 0) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "read_plan.mode=POINT 时 blocks 必须为空");
      }
    } else {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "read_plan.mode 非法");
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

grpc::Status LinkManager::ensureSerialCompatibleLocked(
    const SerialKey& key, const std::string& connName, ModbusRTUProto::LinkMode mode) const {
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
    if (link.config.mode() != mode) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "串口链路模式与现有链路冲突");
    }
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::ensureMqttCompatibleLocked(
    const MqttKey& key, const std::string& connName, ModbusRTUProto::LinkMode mode) const {
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
    if (link.config.mode() != mode) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "远端串口链路模式与现有链路冲突");
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

grpc::Status LinkManager::startSlaveLink(const std::string& connName,
                                         const ModbusRTUProto::LinkConfig& config,
                                         const PointTable& pointTable,
                                         uint32_t connId,
                                         const SerialKey& serialKey,
                                         std::shared_ptr<SerialBus> bus) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = linksByName_.find(connName);
  if (it == linksByName_.end()) {
    return makeNotFound(connName);
  }
  if (it->second.state == ModbusRTUProto::LINK_STATE_PENDING_DELETE) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路处于待删除状态");
  }
  if (it->second.state == ModbusRTUProto::LINK_STATE_RUNNING) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路已在运行");
  }

  auto& slaveBus = slaveBuses_[serialKey];
  if (!slaveBus.bus) {
    slaveBus.bus = bus;
  }
  const auto slaveId = static_cast<uint8_t>(config.slave_id());
  if (slaveBus.linksBySlaveId.contains(slaveId)) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "该串口上的 slave_id 已在运行");
  }
  auto snapshot = std::make_shared<SlaveLinkSnapshot>();
  snapshot->connName = connName;
  snapshot->config = config;
  snapshot->connId = connId;
  snapshot->pointTable = pointTable;
  slaveBus.linksBySlaveId.emplace(slaveId, std::move(snapshot));

  if (!slaveBus.worker.joinable()) {
    slaveBus.worker = ModuleManager::StartModuleThread(
        ModbusRTULibInfo.LIB_NAME,
        [this, serialKey, bus](std::stop_token stopToken) { slaveLoop(serialKey, bus, stopToken); });
  }

  it->second.bus = bus;
  it->second.state = ModbusRTUProto::LINK_STATE_RUNNING;
  it->second.lastError.clear();
  return grpc::Status::OK;
}

grpc::Status LinkManager::stopSlaveLink(const std::string& connName,
                                        const ModbusRTUProto::LinkConfig& config,
                                        const SerialKey& serialKey,
                                        std::shared_ptr<SerialBus> bus) {
  std::jthread worker;
  std::shared_ptr<SerialBus> releasedBus;
  bool shouldStopWorker = false;
  bool removed = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = slaveBuses_.find(serialKey);
    if (it != slaveBuses_.end()) {
      const auto slaveId = static_cast<uint8_t>(config.slave_id());
      removed = (it->second.linksBySlaveId.erase(slaveId) > 0);
      if (it->second.linksBySlaveId.empty()) {
        worker = std::move(it->second.worker);
        slaveBuses_.erase(it);
        shouldStopWorker = true;
      }
    }
    if (bus) {
      releasedBus = std::dynamic_pointer_cast<SerialBus>(releaseSerialBusLocked(serialKey));
    }
  }

  if (!removed) {
    LOG_DEBUG("ModbusRTU 从站响应未找到: conn_name={}", connName);
  }
  if (shouldStopWorker && worker.joinable()) {
    worker.request_stop();
    worker.join();
  }

  if (releasedBus) {
    releasedBus->Close();
  }

  return grpc::Status::OK;
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
  if (link == nullptr || link->config.mode() != ModbusRTUProto::LINK_MODE_MASTER || !link->bus) {
    return;
  }
  stopCommandSubscribeLocked(link);

  std::vector<PointTable::Point> writePoints;
  for (const auto& point : link->pointTable.Points()) {
    if (isWriteRegisterFunction(point.function)) {
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
            updateLastError(connName, status.error_message());
            LOG_WARNING("ModbusRTU 写寄存器失败: conn_name={}, tag={}, 原因={}",
                        connName,
                        update.dst_tag(),
                        status.error_message());
          }
        }

        auto finishStatus = reader->Finish();
        if (!finishStatus.ok() && !st.stop_requested()) {
          LOG_WARNING("ModbusRTU DataCenter 命令订阅异常结束: conn_name={}, conn_id={}, 错误={}",
                      connName,
                      connId,
                      finishStatus.error_message());
          updateLastError(connName, finishStatus.error_message());
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
  if (!isWriteRegisterFunction(point.function)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "点位不是写寄存器功能码");
  }

  uint32_t address = point.address;
  if (config.address_base() == ModbusRTUProto::ADDRESS_BASE_ONE) {
    if (address == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "address_base=ONE 但 address 为 0");
    }
    address -= 1;
  }
  if (address > 0xFFFFu) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "写寄存器地址超出范围");
  }
  if (point.regCount > 1 && address + point.regCount - 1 > 0xFFFFu) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "写寄存器地址范围超出限制");
  }

  double engValue = 0.0;
  if (!pointValueToDouble(update.value(), &engValue)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "命令点值类型不支持");
  }

  std::vector<uint16_t> values;
  auto status = encodeWriteRegisters(point, engValue, &values);
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

  const auto slaveId = static_cast<uint8_t>(config.slave_id());
  if (point.function == ModbusRTUProto::FUNCTION_WRITE_SINGLE_REGISTER) {
    if (values.size() != 1) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "写单寄存器编码数量异常");
    }
    LOG_INFO("ModbusRTU 触发写单寄存器: conn_name={}, tag={}, slave_id={}, address={}, value={}, raw={}",
             connName,
             point.tag,
             config.slave_id(),
             address,
             engValue,
             formatRegisterWords(values));
    status = bus->WriteSingleRegister(slaveId, static_cast<uint16_t>(address), values.front());
    if (status.ok()) {
      LOG_INFO("ModbusRTU 写单寄存器成功: conn_name={}, tag={}, address={}",
               connName,
               point.tag,
               address);
    }
    return status;
  }

  LOG_INFO("ModbusRTU 触发写多寄存器: conn_name={}, tag={}, slave_id={}, address={}, quantity={}, value={}, raw={}",
           connName,
           point.tag,
           config.slave_id(),
           address,
           values.size(),
           engValue,
           formatRegisterWords(values));
  status = bus->WriteMultipleRegisters(slaveId, static_cast<uint16_t>(address), values);
  if (status.ok()) {
    LOG_INFO("ModbusRTU 写多寄存器成功: conn_name={}, tag={}, address={}, quantity={}",
             connName,
             point.tag,
             address,
             values.size());
  }
  return status;
}

void LinkManager::slaveLoop(SerialKey serialKey, std::shared_ptr<SerialBus> bus, std::stop_token stopToken) {
  LOG_INFO("ModbusRTU 从站监听启动: device={}", serialKey.device);
  while (!stopToken.stop_requested()) {
    SerialBus::RtuRequest request;
    auto status = bus->ReadRequest(&request);
    if (!status.ok()) {
      if (status.error_code() != grpc::StatusCode::DEADLINE_EXCEEDED) {
        LOG_WARNING("ModbusRTU 从站读取请求失败: device={}, 原因={}", serialKey.device, status.error_message());
      }
      continue;
    }

    if (stopToken.stop_requested()) {
      break;
    }

    if (request.slaveId == 0) {
      LOG_DEBUG("ModbusRTU 从站忽略广播请求: device={}", serialKey.device);
      continue;
    }

    std::shared_ptr<SlaveLinkSnapshot> link;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto busIt = slaveBuses_.find(serialKey);
      if (busIt == slaveBuses_.end()) {
        continue;
      }
      auto it = busIt->second.linksBySlaveId.find(request.slaveId);
      if (it == busIt->second.linksBySlaveId.end()) {
        LOG_DEBUG("ModbusRTU 未匹配到从站地址: device={}, slave_id={}", serialKey.device, request.slaveId);
        continue;
      }
      link = it->second;
    }

    auto sendException = [&](uint8_t exceptionCode, std::string_view reason) {
      std::vector<uint8_t> resp;
      resp.reserve(5);
      resp.push_back(request.slaveId);
      resp.push_back(static_cast<uint8_t>(request.function | 0x80));
      resp.push_back(exceptionCode);
      SerialBus::appendCrc(&resp);
      auto sendStatus = bus->WriteFrame(resp);
      if (!sendStatus.ok()) {
        LOG_ERROR("ModbusRTU 从站异常响应发送失败: conn_name={}, 原因={}", link->connName, sendStatus.error_message());
        updateLastError(link->connName, sendStatus.error_message());
      } else {
        LOG_WARNING("ModbusRTU 从站返回异常: conn_name={}, 功能码=0x{:02X}, 异常码=0x{:02X}, 原因={}",
                    link->connName,
                    static_cast<unsigned int>(request.function),
                    static_cast<unsigned int>(exceptionCode),
                    reason);
        updateLastError(link->connName, std::string(reason));
      }
    };

    if (request.function == kFunctionReadCoils) {
      if (request.quantity == 0 || request.quantity > kMaxReadCoilsQuantity) {
        sendException(kExceptionIllegalDataValue, "线圈数量非法");
        continue;
      }
      if (static_cast<uint32_t>(request.address) + request.quantity - 1 > 0xFFFF) {
        sendException(kExceptionIllegalDataAddress, "线圈地址超出范围");
        continue;
      }

      struct CoilSlot {
        bool hasPoint = false;
        std::string tag;
        std::optional<bool> defaultValue;
      };
      std::vector<CoilSlot> slots;
      slots.resize(request.quantity);
      std::vector<std::string> tags;
      tags.reserve(request.quantity);

      bool addressOverflow = false;
      for (uint16_t i = 0; i < request.quantity; ++i) {
        uint32_t reqAddr = static_cast<uint32_t>(request.address) + i;
        uint32_t lookupAddr = reqAddr;
        if (link->config.address_base() == ModbusRTUProto::ADDRESS_BASE_ONE) {
          if (reqAddr == 0xFFFF) {
            addressOverflow = true;
            break;
          }
          lookupAddr = reqAddr + 1;
        }
        auto point = link->pointTable.FindByAddress(ModbusRTUProto::FUNCTION_READ_COILS, lookupAddr);
        if (!point.has_value()) {
          continue;
        }
        slots[i].hasPoint = true;
        slots[i].tag = point->tag;
        slots[i].defaultValue = point->defaultBool;
        tags.push_back(point->tag);
      }

      if (addressOverflow) {
        sendException(kExceptionIllegalDataAddress, "线圈地址溢出");
        continue;
      }

      std::unordered_map<std::string, std::optional<bool>> valuesByTag;
      bool dcOk = true;
      if (!tags.empty()) {
        DataCenterProto::GetLatestResponse resp;
        auto dcStatus = dataCenter_.GetLatest(link->connId, tags, &resp);
        if (!dcStatus.ok()) {
          dcOk = false;
          LOG_WARNING("ModbusRTU 从站获取 DataCenter 最新值失败: conn_name={}, 原因={}",
                      link->connName, dcStatus.error_message());
          updateLastError(link->connName, dcStatus.error_message());
        } else {
          for (const auto& update : resp.updates()) {
            if (update.value().kind_case() == DataCenterProto::PointValue::kBoolValue) {
              valuesByTag[update.dst_tag()] = update.value().bool_value();
            } else {
              valuesByTag[update.dst_tag()] = std::nullopt;
              LOG_WARNING("ModbusRTU 从站点值类型不匹配: conn_name={}, tag={}",
                          link->connName, update.dst_tag());
            }
          }
        }
      }

      const size_t byteCount = (static_cast<size_t>(request.quantity) + 7) / 8;
      std::vector<uint8_t> coilBytes(byteCount, 0);
      bool missingValue = false;

      for (uint16_t i = 0; i < request.quantity; ++i) {
        bool value = false;
        if (!slots[i].hasPoint) {
          value = false;
        } else if (dcOk) {
          auto it = valuesByTag.find(slots[i].tag);
          if (it != valuesByTag.end() && it->second.has_value()) {
            value = it->second.value();
          } else if (slots[i].defaultValue.has_value()) {
            value = slots[i].defaultValue.value();
          } else {
            missingValue = true;
          }
        } else {
          if (slots[i].defaultValue.has_value()) {
            value = slots[i].defaultValue.value();
          } else {
            missingValue = true;
          }
        }

        if (missingValue) {
          break;
        }
        if (value) {
          coilBytes[static_cast<size_t>(i / 8)] |= static_cast<uint8_t>(1u << (i % 8));
        }
      }

      if (missingValue) {
        sendException(kExceptionSlaveDeviceFailure, "线圈值缺失");
        continue;
      }

      std::vector<uint8_t> response;
      response.reserve(3 + coilBytes.size() + 2);
      response.push_back(request.slaveId);
      response.push_back(kFunctionReadCoils);
      response.push_back(static_cast<uint8_t>(coilBytes.size()));
      response.insert(response.end(), coilBytes.begin(), coilBytes.end());
      SerialBus::appendCrc(&response);

      auto sendStatus = bus->WriteFrame(response);
      if (!sendStatus.ok()) {
        LOG_ERROR("ModbusRTU 从站响应发送失败: conn_name={}, 原因={}", link->connName, sendStatus.error_message());
        updateLastError(link->connName, sendStatus.error_message());
      }
      continue;
    }

    if (request.function == kFunctionReadHoldingRegisters || request.function == kFunctionReadInputRegisters) {
      const auto pointFunction = request.function == kFunctionReadInputRegisters
          ? ModbusRTUProto::FUNCTION_READ_INPUT_REGISTERS
          : ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS;
      const auto registerName = request.function == kFunctionReadInputRegisters ? "输入寄存器" : "保持寄存器";
      const auto maxQuantity = request.function == kFunctionReadInputRegisters
          ? kMaxReadInputRegistersQuantity
          : kMaxReadHoldingRegistersQuantity;

      if (request.quantity == 0 || request.quantity > maxQuantity) {
        sendException(kExceptionIllegalDataValue, std::string(registerName) + "数量非法");
        continue;
      }
      if (static_cast<uint32_t>(request.address) + request.quantity - 1 > 0xFFFF) {
        sendException(kExceptionIllegalDataAddress, std::string(registerName) + "地址超出范围");
        continue;
      }

      struct RegisterSlot {
        bool hasPoint = false;
        std::string tag;
        ModbusRTUProto::DataType type = ModbusRTUProto::DATA_TYPE_UNSPECIFIED;
        uint32_t wordIndex = 0;
      };

      struct PointMeta {
        ModbusRTUProto::DataType type = ModbusRTUProto::DATA_TYPE_UNSPECIFIED;
        uint32_t regCount = 1;
        ModbusRTUProto::WordOrder wordOrder = ModbusRTUProto::WORD_ORDER_HL;
        ModbusRTUProto::ByteOrder byteOrder = ModbusRTUProto::BYTE_ORDER_AB;
        double scale = 1.0;
        double offset = 0.0;
        std::optional<uint32_t> defaultValue;
      };

      std::vector<RegisterSlot> slots(request.quantity);
      std::unordered_map<std::string, PointMeta> metaByTag;
      metaByTag.reserve(request.quantity);

      bool addressOverflow = false;
      for (uint16_t i = 0; i < request.quantity; ++i) {
        uint32_t reqAddr = static_cast<uint32_t>(request.address) + i;
        uint32_t lookupAddr = reqAddr;
        if (link->config.address_base() == ModbusRTUProto::ADDRESS_BASE_ONE) {
          if (reqAddr == 0xFFFF) {
            addressOverflow = true;
            break;
          }
          lookupAddr = reqAddr + 1;
        }
        auto point = link->pointTable.FindRegisterByAddress(pointFunction, lookupAddr);
        if (!point) {
          continue;
        }
        slots[i].hasPoint = true;
        slots[i].tag = point->point.tag;
        slots[i].type = point->point.type;
        slots[i].wordIndex = point->wordIndex;
        if (metaByTag.find(point->point.tag) == metaByTag.end()) {
          PointMeta meta;
          meta.type = point->point.type;
          meta.regCount = point->point.regCount;
          meta.wordOrder = point->point.wordOrder;
          meta.byteOrder = point->point.byteOrder;
          meta.scale = point->point.scale;
          meta.offset = point->point.offset;
          if (point->point.defaultUInt16.has_value()) {
            meta.defaultValue = point->point.defaultUInt16.value();
          } else if (point->point.defaultUInt32.has_value()) {
            meta.defaultValue = point->point.defaultUInt32.value();
          }
          metaByTag.emplace(point->point.tag, meta);
        }
      }

      if (addressOverflow) {
        sendException(kExceptionIllegalDataAddress, std::string(registerName) + "地址溢出");
        continue;
      }

      std::vector<std::string> tags;
      tags.reserve(metaByTag.size());
      for (const auto& [tag, _] : metaByTag) {
        tags.push_back(tag);
      }

      std::unordered_map<std::string, std::optional<double>> valuesByTag;
      bool dcOk = true;
      if (!tags.empty()) {
        DataCenterProto::GetLatestResponse resp;
        auto dcStatus = dataCenter_.GetLatest(link->connId, tags, &resp);
        if (!dcStatus.ok()) {
          dcOk = false;
          LOG_WARNING("ModbusRTU 从站获取 DataCenter 最新值失败: conn_name={}, 原因={}",
                      link->connName, dcStatus.error_message());
          updateLastError(link->connName, dcStatus.error_message());
        } else {
          for (const auto& update : resp.updates()) {
            if (update.value().kind_case() == DataCenterProto::PointValue::kDoubleValue) {
              valuesByTag[update.dst_tag()] = update.value().double_value();
            } else if (update.value().kind_case() == DataCenterProto::PointValue::kIntValue) {
              valuesByTag[update.dst_tag()] = static_cast<double>(update.value().int_value());
            } else {
              valuesByTag[update.dst_tag()] = std::nullopt;
              LOG_WARNING("ModbusRTU 从站点值类型不匹配: conn_name={}, tag={}",
                          link->connName, update.dst_tag());
            }
          }
        }
      }

      std::unordered_map<std::string, std::optional<uint32_t>> rawByTag;
      rawByTag.reserve(metaByTag.size());
      bool missingValue = false;

      for (const auto& [tag, meta] : metaByTag) {
        uint32_t value = 0;
        bool hasValue = false;
        if (dcOk) {
          auto it = valuesByTag.find(tag);
          if (it != valuesByTag.end() && it->second.has_value()) {
            const double engValue = it->second.value();
            const double scale = meta.scale == 0.0 ? 1.0 : meta.scale;
            const double rawValue = (engValue - meta.offset) / scale;
            if (!std::isfinite(rawValue)) {
              LOG_WARNING("ModbusRTU 从站点值无法反向缩放: conn_name={}, tag={}, value={}",
                          link->connName, tag, engValue);
              missingValue = true;
            } else {
              long long rounded = std::llround(rawValue);
              const unsigned long long maxValue =
                  meta.type == ModbusRTUProto::DATA_TYPE_UINT32 ? 0xFFFFFFFFull : 0xFFFFull;
              if (rounded < 0) {
                LOG_WARNING("ModbusRTU 从站点值超出范围已截断: conn_name={}, tag={}, raw={}",
                            link->connName, tag, rounded);
                rounded = 0;
              } else if (static_cast<unsigned long long>(rounded) > maxValue) {
                LOG_WARNING("ModbusRTU 从站点值超出范围已截断: conn_name={}, tag={}, raw={}",
                            link->connName, tag, rounded);
                rounded = static_cast<long long>(maxValue);
              }
              value = static_cast<uint32_t>(rounded);
              hasValue = true;
            }
          }
        }
        if (!hasValue && meta.defaultValue.has_value()) {
          value = meta.defaultValue.value();
          hasValue = true;
        }
        if (!hasValue) {
          missingValue = true;
        } else {
          rawByTag[tag] = value;
        }
        if (missingValue) {
          break;
        }
      }

      if (missingValue) {
        sendException(kExceptionSlaveDeviceFailure, std::string(registerName) + "值缺失");
        continue;
      }

      std::unordered_map<std::string, std::array<uint16_t, 2>> wordsByTag;
      wordsByTag.reserve(metaByTag.size());
      for (const auto& [tag, meta] : metaByTag) {
        if (meta.type != ModbusRTUProto::DATA_TYPE_UINT32) {
          continue;
        }
        auto rawIt = rawByTag.find(tag);
        if (rawIt == rawByTag.end() || !rawIt->second.has_value()) {
          missingValue = true;
          break;
        }
        auto words = encodeUint32(rawIt->second.value(), meta.wordOrder, meta.byteOrder);
        wordsByTag.emplace(tag, words);
        LOG_DEBUG("ModbusRTU 从站32位寄存器编码: conn_name={}, tag={}, raw={}, reg0={}, reg1={}, word_order={}, byte_order={}",
                  link->connName,
                  tag,
                  rawIt->second.value(),
                  words[0],
                  words[1],
                  static_cast<int>(meta.wordOrder),
                  static_cast<int>(meta.byteOrder));
      }

      if (missingValue) {
        sendException(kExceptionSlaveDeviceFailure, std::string(registerName) + "值缺失");
        continue;
      }

      const size_t byteCount = static_cast<size_t>(request.quantity) * 2;
      std::vector<uint8_t> registerBytes(byteCount, 0);
      for (uint16_t i = 0; i < request.quantity; ++i) {
        uint16_t value = 0;
        if (!slots[i].hasPoint) {
          value = 0;
        } else {
          auto rawIt = rawByTag.find(slots[i].tag);
          if (rawIt == rawByTag.end() || !rawIt->second.has_value()) {
            missingValue = true;
          } else if (slots[i].type == ModbusRTUProto::DATA_TYPE_UINT32) {
            auto wordsIt = wordsByTag.find(slots[i].tag);
            if (wordsIt == wordsByTag.end() || slots[i].wordIndex >= wordsIt->second.size()) {
              missingValue = true;
            } else {
              value = wordsIt->second[slots[i].wordIndex];
            }
          } else {
            value = static_cast<uint16_t>(rawIt->second.value());
            auto metaIt = metaByTag.find(slots[i].tag);
            if (metaIt != metaByTag.end() && metaIt->second.byteOrder == ModbusRTUProto::BYTE_ORDER_BA) {
              value = swapWordBytes(value);
            }
          }
        }
        if (missingValue) {
          break;
        }
        const auto offset = static_cast<size_t>(i) * 2;
        registerBytes[offset] = static_cast<uint8_t>((value >> 8) & 0xFF);
        registerBytes[offset + 1] = static_cast<uint8_t>(value & 0xFF);
      }

      if (missingValue) {
        sendException(kExceptionSlaveDeviceFailure, std::string(registerName) + "值缺失");
        continue;
      }

      std::vector<uint8_t> response;
      response.reserve(3 + registerBytes.size() + 2);
      response.push_back(request.slaveId);
      response.push_back(request.function);
      response.push_back(static_cast<uint8_t>(registerBytes.size()));
      response.insert(response.end(), registerBytes.begin(), registerBytes.end());
      SerialBus::appendCrc(&response);

      auto sendStatus = bus->WriteFrame(response);
      if (!sendStatus.ok()) {
        LOG_ERROR("ModbusRTU 从站响应发送失败: conn_name={}, 原因={}", link->connName, sendStatus.error_message());
        updateLastError(link->connName, sendStatus.error_message());
      }
      continue;
    }

    sendException(kExceptionIllegalFunction, "不支持的功能码");
  }

  LOG_INFO("ModbusRTU 从站监听结束: device={}", serialKey.device);
}

grpc::Status LinkManager::UpsertLink(const ModbusRTUProto::UpsertLinkRequest& request, ModbusRTUProto::LinkInfo* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
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
        status = ensureSerialCompatibleLocked(serialKey, connName, normalized.mode());
      } else {
        status = ensureMqttCompatibleLocked(mqttKey, connName, normalized.mode());
      }
      if (!status.ok()) {
        return status;
      }
      if (normalized.mode() == ModbusRTUProto::LINK_MODE_SLAVE && hasWriteRegisterPoints(it->second.pointTable)) {
        LOG_WARNING("ModbusRTU 更新链路模式被拒绝: conn_name={}, 原因=从站模式暂不支持写寄存器点位", connName);
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "从站模式暂不支持写寄存器点位");
      }
      if (normalized.mode() == ModbusRTUProto::LINK_MODE_SLAVE && hasSignedRegisterPoints(it->second.pointTable)) {
        LOG_WARNING("ModbusRTU 更新链路模式被拒绝: conn_name={}, 原因=从站模式暂不支持 INT16/INT32 点位", connName);
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "从站模式暂不支持 INT16/INT32 点位");
      }

      it->second.config = normalized;
      it->second.serialKey = serialKey;
      it->second.mqttKey = mqttKey;
      it->second.lastError.clear();
      return fillLinkInfoLocked(it->second, out);
    }

    if (pendingCreateByName_.contains(connName)) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name 已存在");
    }
    if (isSerial) {
      status = ensureSerialCompatibleLocked(serialKey, connName, normalized.mode());
    } else {
      status = ensureMqttCompatibleLocked(mqttKey, connName, normalized.mode());
    }
    if (!status.ok()) {
      return status;
    }
    pendingCreateByName_.insert(connName);
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
    link.lastError.clear();
    auto [it, inserted] = linksByName_.emplace(connName, std::move(link));
    if (!inserted) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name 已存在");
    }
    return fillLinkInfoLocked(it->second, out);
  }
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
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路已在运行");
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
  if (config.mode() == ModbusRTUProto::LINK_MODE_SLAVE && hasWriteRegisterPoints(pointTable)) {
    updateLastError(connName, "从站模式暂不支持写寄存器点位");
    LOG_WARNING("ModbusRTU 启动从站响应被拒绝: conn_name={}, 原因=从站模式暂不支持写寄存器点位", connName);
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "从站模式暂不支持写寄存器点位");
  }
  if (config.mode() == ModbusRTUProto::LINK_MODE_SLAVE && hasSignedRegisterPoints(pointTable)) {
    updateLastError(connName, "从站模式暂不支持 INT16/INT32 点位");
    LOG_WARNING("ModbusRTU 启动从站响应被拒绝: conn_name={}, 原因=从站模式暂不支持 INT16/INT32 点位", connName);
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "从站模式暂不支持 INT16/INT32 点位");
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
        it->second.lastError = errorMessage;
      }
    }
    LOG_ERROR("ModbusRTU 打开链路失败: conn_name={}, 端点={}, 原因={}", connName, endpoint, errorMessage);
    return status;
  }

  if (config.mode() == ModbusRTUProto::LINK_MODE_SLAVE) {
    auto serialBus = std::dynamic_pointer_cast<SerialBus>(bus);
    if (!serialBus) {
      {
        std::lock_guard<std::mutex> lock(mu_);
        auto released = releaseSerialBusLocked(serialKey);
        if (released) {
          released->Close();
        }
        auto it = linksByName_.find(connName);
        if (it != linksByName_.end()) {
          it->second.lastError = "从站串口总线类型错误";
        }
      }
      LOG_ERROR("ModbusRTU 启动从站响应失败: conn_name={}, 原因=从站串口总线类型错误", connName);
      return grpc::Status(grpc::StatusCode::INTERNAL, "从站串口总线类型错误");
    }
    status = startSlaveLink(connName, config, pointTable, connId, serialKey, serialBus);
    if (!status.ok()) {
      const auto errorMessage = status.error_message();
      const auto device = config.serial().device();
      {
        std::lock_guard<std::mutex> lock(mu_);
        auto released = releaseSerialBusLocked(serialKey);
        if (released) {
          released->Close();
        }
        auto it = linksByName_.find(connName);
        if (it != linksByName_.end()) {
          it->second.lastError = errorMessage;
        }
      }
      LOG_ERROR("ModbusRTU 启动从站响应失败: conn_name={}, device={}, 原因={}", connName, device, errorMessage);
      return status;
    }
    LOG_INFO("ModbusRTU 已启动从站响应: conn_name={}, slave_id={}, device={}",
             connName, config.slave_id(), config.serial().device());
    return grpc::Status::OK;
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
    link.bus = bus;
    link.state = ModbusRTUProto::LINK_STATE_RUNNING;
    link.lastError.clear();
    link.pollThread = ModuleManager::StartModuleThread(
        ModbusRTULibInfo.LIB_NAME,
        [this, connName, connId, config, pointTable, bus](std::stop_token stopToken) {
          pollLoop(connName, connId, config, pointTable, bus, stopToken);
        });
    startCommandSubscribeLocked(connName, &link);
  }

  if (isSerial) {
    LOG_INFO("ModbusRTU 已启动轮询: conn_name={}, slave_id={}, device={}",
             connName,
             config.slave_id(),
             config.serial().device());
  } else {
    LOG_INFO("ModbusRTU 已启动 MQTT 透传轮询: conn_name={}, slave_id={}, serial_port={}",
             connName,
             config.slave_id(),
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
  bool pendingDelete = false;
  ModbusRTUProto::LinkConfig config;
  ModbusRTUProto::LinkMode mode = ModbusRTUProto::LINK_MODE_UNSPECIFIED;

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
    mode = it->second.config.mode();
    serialKey = it->second.serialKey;
    mqttKey = it->second.mqttKey;
    bus = it->second.bus;
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

  if (mode == ModbusRTUProto::LINK_MODE_SLAVE) {
    stopSlaveLink(connName, config, serialKey, std::dynamic_pointer_cast<SerialBus>(bus));
    LOG_INFO("ModbusRTU 已停止从站响应: conn_name={}", connName);
    return grpc::Status::OK;
  }

  if (pollThread.joinable()) {
    pollThread.request_stop();
    pollThread.join();
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
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it != linksByName_.end()) {
      it->second.state = ModbusRTUProto::LINK_STATE_PENDING_DELETE;
      it->second.lastError = dc.error_message();
    }
    return dc;
  }

  std::lock_guard<std::mutex> lock(mu_);
  linksByName_.erase(connName);
  return grpc::Status::OK;
}

grpc::Status LinkManager::UpsertPointTable(const ModbusRTUProto::UpsertPointTableRequest& request) {
  auto status = validateConnName(request.conn_name());
  if (!status.ok()) {
    return status;
  }

  uint32_t connId = 0;
  PointTable current;
  ModbusRTUProto::LinkConfig config;
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
    config = it->second.config;
  }

  PointTable next = current;
  status = next.Upsert(request.points(), request.replace());
  if (!status.ok()) {
    return status;
  }
  if (config.mode() == ModbusRTUProto::LINK_MODE_SLAVE && hasWriteRegisterPoints(next)) {
    LOG_WARNING("ModbusRTU 点表更新被拒绝: conn_name={}, 原因=从站模式暂不支持写寄存器点位", request.conn_name());
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "从站模式暂不支持写寄存器点位");
  }
  if (config.mode() == ModbusRTUProto::LINK_MODE_SLAVE && hasSignedRegisterPoints(next)) {
    LOG_WARNING("ModbusRTU 点表更新被拒绝: conn_name={}, 原因=从站模式暂不支持 INT16/INT32 点位", request.conn_name());
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "从站模式暂不支持 INT16/INT32 点位");
  }

  auto tags = next.Tags();
  status = dataCenter_.UpsertPointTable(connId, tags, true);
  if (!status.ok()) {
    return status;
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto it = linksByName_.find(request.conn_name());
  if (it == linksByName_.end()) {
    return makeNotFound(request.conn_name());
  }
  it->second.pointTable = std::move(next);
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
  const bool useExplicitPlan = (config.mode() == ModbusRTUProto::LINK_MODE_MASTER &&
                                readPlanMode == ModbusRTUProto::READ_PLAN_MODE_EXPLICIT &&
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
      for (const auto& point : points) {
        if (stopToken.stop_requested()) {
          break;
        }
        if (isWriteRegisterFunction(point.function)) {
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
          status = bus->ReadCoil(static_cast<uint8_t>(config.slave_id()), static_cast<uint16_t>(address), &value);
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
          if (is16BitRegisterType(point.type)) {
            uint16_t value = 0;
            status = isInputRegisters
                ? bus->ReadInputRegister(static_cast<uint8_t>(config.slave_id()),
                                         static_cast<uint16_t>(address),
                                         &value)
                : bus->ReadHoldingRegister(static_cast<uint8_t>(config.slave_id()),
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
                ? bus->ReadInputRegisters(static_cast<uint8_t>(config.slave_id()),
                                          static_cast<uint16_t>(address),
                                          2,
                                          &values)
                : bus->ReadHoldingRegisters(static_cast<uint8_t>(config.slave_id()),
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

        if (isReadRegisterFunction(point.function)) {
          registerPoints.push_back(std::move(normalized));
        } else if (point.function == ModbusRTUProto::FUNCTION_READ_COILS) {
          coilPoints.push_back(std::move(normalized));
        } else if (!isWriteRegisterFunction(point.function)) {
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
    std::unordered_set<std::string> processedTags;
    processedTags.reserve(registerPoints.size());

    for (const auto& block : config.read_plan().blocks()) {
      if (stopToken.stop_requested()) {
        break;
      }
      if (block.function() != ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS &&
          block.function() != ModbusRTUProto::FUNCTION_READ_INPUT_REGISTERS) {
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
          ? bus->ReadInputRegisters(static_cast<uint8_t>(config.slave_id()),
                                    static_cast<uint16_t>(start),
                                    static_cast<uint16_t>(block.quantity()),
                                    &values)
          : bus->ReadHoldingRegisters(static_cast<uint8_t>(config.slave_id()),
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
      auto status = bus->ReadCoil(static_cast<uint8_t>(config.slave_id()),
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
    std::this_thread::sleep_for(interval);
  }

  LOG_INFO("ModbusRTU 轮询结束: conn_name={}", connName);
}

void LinkManager::updateLastError(const std::string& connName, const std::string& error) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = linksByName_.find(connName);
  if (it == linksByName_.end()) {
    return;
  }
  it->second.lastError = error;
}

}  // namespace ModbusRTU
