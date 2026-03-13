#include "ModbusRTUMqttBus.h"

#include <boost/json.hpp>

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <format>
#include <string_view>
#include <utility>

#include "Logger.h"
#include "ModbusRTUSerialBus.h"

namespace ModbusRTU {
namespace {
constexpr uint8_t kFunctionReadCoils = 0x01;
constexpr uint8_t kFunctionReadHoldingRegisters = 0x03;
constexpr uint8_t kFunctionReadInputRegisters = 0x04;
constexpr uint8_t kFunctionWriteSingleRegister = 0x06;
constexpr uint8_t kFunctionWriteMultipleRegisters = 0x10;
constexpr const char* kAppName = "AGVC";
constexpr const char* kAppTypeUart = "uartManager";
constexpr char kHexDigits[] = "0123456789ABCDEF";
constexpr size_t kMaxWriteMultipleRegistersQuantity = 123;

std::string statusToMessage(int32_t statusCode) {
  switch (statusCode) {
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
    default:
      return "透传请求失败";
  }
}
}  // namespace

std::atomic<uint64_t> MqttBus::tokenCounter_{0};

MqttBus::MqttBus(ModbusRTUProto::LinkConfig config, MqttClient* client) :
  config_(std::move(config)),
  client_(client) {}

grpc::Status MqttBus::Open() {
  std::lock_guard<std::mutex> lock(mu_);
  return ensureOpenLocked();
}

void MqttBus::Close() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!opened_) {
    return;
  }
  opened_ = false;
  LOG_INFO("ModbusRTU MQTT 串口总线已关闭: conn_name={}, serial_port={}",
           config_.conn_name(),
           config_.serial_port());
}

grpc::Status MqttBus::ReadCoil(uint8_t slaveId, uint16_t address, bool* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  std::lock_guard<std::mutex> lock(mu_);
  auto status = ensureOpenLocked();
  if (!status.ok()) {
    return status;
  }

  std::vector<uint8_t> frame;
  frame.reserve(8);
  frame.push_back(slaveId);
  frame.push_back(kFunctionReadCoils);
  frame.push_back(static_cast<uint8_t>((address >> 8) & 0xFF));
  frame.push_back(static_cast<uint8_t>(address & 0xFF));
  frame.push_back(0x00);
  frame.push_back(0x01);
  SerialBus::appendCrc(&frame);

  std::vector<uint8_t> responseFrame;
  status = sendFrame(frame, &responseFrame);
  if (!status.ok()) {
    return status;
  }

  std::vector<uint8_t> data;
  status = parseReadResponse(responseFrame, slaveId, kFunctionReadCoils, 1, &data);
  if (!status.ok()) {
    return status;
  }
  if (data.empty()) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "线圈响应为空");
  }
  *out = ((data[0] & 0x01) != 0);
  return grpc::Status::OK;
}

grpc::Status MqttBus::ReadHoldingRegister(uint8_t slaveId, uint16_t address, uint16_t* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  std::vector<uint16_t> values;
  auto status = ReadHoldingRegisters(slaveId, address, 1, &values);
  if (!status.ok()) {
    return status;
  }
  if (values.size() != 1) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "保持寄存器响应数量异常");
  }
  *out = values.front();
  return grpc::Status::OK;
}

grpc::Status MqttBus::ReadHoldingRegisters(uint8_t slaveId,
                                           uint16_t address,
                                           uint16_t quantity,
                                           std::vector<uint16_t>* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  if (quantity == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "quantity 不能为空");
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto status = ensureOpenLocked();
  if (!status.ok()) {
    return status;
  }

  std::vector<uint8_t> frame;
  frame.reserve(8);
  frame.push_back(slaveId);
  frame.push_back(kFunctionReadHoldingRegisters);
  frame.push_back(static_cast<uint8_t>((address >> 8) & 0xFF));
  frame.push_back(static_cast<uint8_t>(address & 0xFF));
  frame.push_back(static_cast<uint8_t>((quantity >> 8) & 0xFF));
  frame.push_back(static_cast<uint8_t>(quantity & 0xFF));
  SerialBus::appendCrc(&frame);

  std::vector<uint8_t> responseFrame;
  status = sendFrame(frame, &responseFrame);
  if (!status.ok()) {
    return status;
  }

  std::vector<uint8_t> data;
  status = parseReadResponse(responseFrame, slaveId, kFunctionReadHoldingRegisters, quantity, &data);
  if (!status.ok()) {
    return status;
  }
  if (data.size() != static_cast<size_t>(quantity) * 2) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "保持寄存器响应长度异常");
  }

  out->clear();
  out->reserve(quantity);
  for (uint16_t i = 0; i < quantity; ++i) {
    const size_t offset = static_cast<size_t>(i) * 2;
    const uint16_t value = static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]);
    out->push_back(value);
  }
  LOG_DEBUG("ModbusRTU MQTT 保持寄存器响应解析完成: conn_name={}, serial_port={}, address={}, quantity={}, 数据={}",
            config_.conn_name(),
            config_.serial_port(),
            address,
            quantity,
            bytesToHex(data));
  return grpc::Status::OK;
}

grpc::Status MqttBus::ReadInputRegister(uint8_t slaveId, uint16_t address, uint16_t* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  std::vector<uint16_t> values;
  auto status = ReadInputRegisters(slaveId, address, 1, &values);
  if (!status.ok()) {
    return status;
  }
  if (values.size() != 1) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "输入寄存器响应数量异常");
  }
  *out = values.front();
  return grpc::Status::OK;
}

grpc::Status MqttBus::ReadInputRegisters(uint8_t slaveId,
                                         uint16_t address,
                                         uint16_t quantity,
                                         std::vector<uint16_t>* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  if (quantity == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "quantity 不能为空");
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto status = ensureOpenLocked();
  if (!status.ok()) {
    return status;
  }

  std::vector<uint8_t> frame;
  frame.reserve(8);
  frame.push_back(slaveId);
  frame.push_back(kFunctionReadInputRegisters);
  frame.push_back(static_cast<uint8_t>((address >> 8) & 0xFF));
  frame.push_back(static_cast<uint8_t>(address & 0xFF));
  frame.push_back(static_cast<uint8_t>((quantity >> 8) & 0xFF));
  frame.push_back(static_cast<uint8_t>(quantity & 0xFF));
  SerialBus::appendCrc(&frame);

  std::vector<uint8_t> responseFrame;
  status = sendFrame(frame, &responseFrame);
  if (!status.ok()) {
    return status;
  }

  std::vector<uint8_t> data;
  status = parseReadResponse(responseFrame, slaveId, kFunctionReadInputRegisters, quantity, &data);
  if (!status.ok()) {
    return status;
  }
  if (data.size() != static_cast<size_t>(quantity) * 2) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "输入寄存器响应长度异常");
  }

  out->clear();
  out->reserve(quantity);
  for (uint16_t i = 0; i < quantity; ++i) {
    const size_t offset = static_cast<size_t>(i) * 2;
    const uint16_t value = static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]);
    out->push_back(value);
  }
  LOG_DEBUG("ModbusRTU MQTT 输入寄存器响应解析完成: conn_name={}, serial_port={}, address={}, quantity={}, 数据={}",
            config_.conn_name(),
            config_.serial_port(),
            address,
            quantity,
            bytesToHex(data));
  return grpc::Status::OK;
}

grpc::Status MqttBus::WriteSingleRegister(uint8_t slaveId, uint16_t address, uint16_t value) {
  std::lock_guard<std::mutex> lock(mu_);
  auto status = ensureOpenLocked();
  if (!status.ok()) {
    return status;
  }

  std::vector<uint8_t> frame;
  frame.reserve(8);
  frame.push_back(slaveId);
  frame.push_back(kFunctionWriteSingleRegister);
  frame.push_back(static_cast<uint8_t>((address >> 8) & 0xFF));
  frame.push_back(static_cast<uint8_t>(address & 0xFF));
  frame.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  frame.push_back(static_cast<uint8_t>(value & 0xFF));
  SerialBus::appendCrc(&frame);

  std::vector<uint8_t> responseFrame;
  status = sendFrame(frame, &responseFrame);
  if (!status.ok()) {
    return status;
  }
  return parseWriteSingleRegisterResponse(responseFrame, slaveId, address, value);
}

grpc::Status MqttBus::WriteMultipleRegisters(uint8_t slaveId,
                                             uint16_t address,
                                             const std::vector<uint16_t>& values) {
  if (values.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "values 不能为空");
  }
  if (values.size() > kMaxWriteMultipleRegistersQuantity) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "写多寄存器数量不能超过 123");
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto status = ensureOpenLocked();
  if (!status.ok()) {
    return status;
  }
  const auto quantity = static_cast<uint16_t>(values.size());

  std::vector<uint8_t> frame;
  frame.reserve(9 + values.size() * 2);
  frame.push_back(slaveId);
  frame.push_back(kFunctionWriteMultipleRegisters);
  frame.push_back(static_cast<uint8_t>((address >> 8) & 0xFF));
  frame.push_back(static_cast<uint8_t>(address & 0xFF));
  frame.push_back(static_cast<uint8_t>((quantity >> 8) & 0xFF));
  frame.push_back(static_cast<uint8_t>(quantity & 0xFF));
  frame.push_back(static_cast<uint8_t>(quantity * 2));
  for (const auto value : values) {
    frame.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(value & 0xFF));
  }
  SerialBus::appendCrc(&frame);

  std::vector<uint8_t> responseFrame;
  status = sendFrame(frame, &responseFrame);
  if (!status.ok()) {
    return status;
  }
  return parseWriteMultipleRegistersResponse(responseFrame, slaveId, address, quantity);
}

grpc::Status MqttBus::ensureOpenLocked() {
  if (opened_) {
    return grpc::Status::OK;
  }
  if (client_ == nullptr) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "MQTT 客户端未初始化");
  }
  if (!client_->hasConfig()) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "MQTT 连接参数未配置");
  }
  if (config_.serial_port().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "serial_port 不能为空");
  }
  opened_ = true;
  LOG_INFO("ModbusRTU MQTT 串口总线已打开: conn_name={}, serial_port={}, MQTT连接={}",
           config_.conn_name(),
           config_.serial_port(),
           client_->connectionLabel());
  return grpc::Status::OK;
}

grpc::Status MqttBus::sendFrame(const std::vector<uint8_t>& frame, std::vector<uint8_t>* outFrame) {
  if (outFrame == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "outFrame 为空");
  }

  const auto token = std::to_string(tokenCounter_.fetch_add(1, std::memory_order_relaxed) + 1);
  boost::json::object requestObj;
  requestObj["token"] = token;
  requestObj["timestamp"] = formatTimestamp();
  requestObj["port"] = config_.serial_port();
  requestObj["prio"] = 1;
  requestObj["prm"] = 1;
  requestObj["byteTimeout"] = config_.serial_byte_timeout_ms();
  requestObj["frameTimeout"] = config_.serial_frame_timeout_ms();
  requestObj["taskTimeout"] = config_.request_timeout_ms();
  boost::json::object param;
  param["baudRate"] = config_.serial().baud_rate();
  param["byteSize"] = config_.serial().data_bits();
  param["parity"] = serialParityToText(config_.serial().parity());
  param["stopBits"] = serialStopBitsToNumber(config_.serial().stop_bits());
  requestObj["param"] = std::move(param);
  requestObj["estSize"] = config_.serial_est_size();
  requestObj["data"] = base64Encode(frame);

  const auto requestJson = boost::json::serialize(requestObj);
  const auto reqTopic = requestTopic();
  const auto respTopic = responseTopic();
  LOG_INFO("ModbusRTU MQTT UART 请求发送: conn_name={}, 请求主题={}, 响应主题={}, RTU报文={}",
           config_.conn_name(),
           reqTopic,
           respTopic,
           bytesToHex(frame));
  LOG_INFO("ModbusRTU MQTT UART 请求报文: conn_name={}, payload={}", config_.conn_name(), requestJson);

  std::string responsePayload;
  std::string error;
  auto status = client_->RequestAndWait(reqTopic,
                                        respTopic,
                                        requestJson,
                                        config_.request_timeout_ms(),
                                        0,
                                        0,
                                        "token",
                                        &responsePayload,
                                        &error);
  if (!status.ok()) {
    LOG_ERROR("ModbusRTU MQTT UART 请求失败: conn_name={}, serial_port={}, 原因={}",
              config_.conn_name(),
              config_.serial_port(),
              error);
    return status;
  }
  if (responsePayload.empty()) {
    LOG_ERROR("ModbusRTU MQTT UART 响应为空: conn_name={}, serial_port={}",
              config_.conn_name(),
              config_.serial_port());
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "MQTT UART 响应为空");
  }
  LOG_INFO("ModbusRTU MQTT UART 响应报文: conn_name={}, payload={}", config_.conn_name(), responsePayload);

  boost::system::error_code ec;
  auto parsed = boost::json::parse(responsePayload, ec);
  if (ec || !parsed.is_object()) {
    LOG_ERROR("ModbusRTU MQTT UART 响应解析失败: conn_name={}, 原因={}", config_.conn_name(), ec.message());
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "MQTT UART 响应解析失败");
  }

  const auto& responseObj = parsed.as_object();
  auto statusIt = responseObj.find("status");
  if (statusIt != responseObj.end()) {
    bool ok = false;
    const int32_t statusCode = parseStatusCode(statusIt->value(), &ok);
    if (ok && statusCode != 0) {
      const auto statusMessage = statusToMessage(statusCode);
      LOG_ERROR("ModbusRTU MQTT UART 响应状态失败: conn_name={}, serial_port={}, status={}, 描述={}",
                config_.conn_name(),
                config_.serial_port(),
                statusCode,
                statusMessage);
      return grpc::Status(grpc::StatusCode::UNAVAILABLE, statusMessage);
    }
  }

  auto dataIt = responseObj.find("data");
  if (dataIt == responseObj.end() || !dataIt->value().is_string()) {
    LOG_ERROR("ModbusRTU MQTT UART 响应缺少 data: conn_name={}, serial_port={}",
              config_.conn_name(),
              config_.serial_port());
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "MQTT UART 响应缺少 data");
  }

  std::vector<uint8_t> responseFrame;
  if (!base64Decode(dataIt->value().as_string().c_str(), &responseFrame)) {
    LOG_ERROR("ModbusRTU MQTT UART 响应 data Base64 解码失败: conn_name={}, serial_port={}",
              config_.conn_name(),
              config_.serial_port());
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "MQTT UART 响应 data 解码失败");
  }
  LOG_INFO("ModbusRTU MQTT UART 响应接收: conn_name={}, serial_port={}, RTU报文={}",
           config_.conn_name(),
           config_.serial_port(),
           bytesToHex(responseFrame));
  *outFrame = std::move(responseFrame);
  return grpc::Status::OK;
}

grpc::Status MqttBus::parseReadResponse(const std::vector<uint8_t>& frame,
                                        uint8_t expectedSlaveId,
                                        uint8_t expectedFunction,
                                        uint16_t quantity,
                                        std::vector<uint8_t>* outData) const {
  if (outData == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "outData 为空");
  }
  if (quantity == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "quantity 不能为空");
  }
  if (frame.size() < 5) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应帧过短");
  }
  if (frame[0] != expectedSlaveId) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应 slave_id 不匹配");
  }

  const uint8_t function = frame[1];
  if (function == static_cast<uint8_t>(expectedFunction | 0x80)) {
    if (frame.size() != 5) {
      return grpc::Status(grpc::StatusCode::UNAVAILABLE, "异常响应长度异常");
    }
    const uint16_t crc = SerialBus::computeCrc(frame.data(), 3);
    const uint16_t respCrc = static_cast<uint16_t>(frame[3]) | (static_cast<uint16_t>(frame[4]) << 8);
    if (crc != respCrc) {
      return grpc::Status(grpc::StatusCode::UNAVAILABLE, "异常响应 CRC 不匹配");
    }
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::format("Modbus 异常: {}", frame[2]));
  }

  if (function != expectedFunction) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应功能码不匹配");
  }

  const uint8_t byteCount = frame[2];
  const size_t expectedByteCount =
      (expectedFunction == kFunctionReadCoils) ? ((quantity + 7) / 8) : (static_cast<size_t>(quantity) * 2);
  if (frame.size() != 3 + static_cast<size_t>(byteCount) + 2) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应长度不匹配");
  }

  const uint16_t crc = SerialBus::computeCrc(frame.data(), frame.size() - 2);
  const uint16_t respCrc = static_cast<uint16_t>(frame[frame.size() - 2]) |
      (static_cast<uint16_t>(frame[frame.size() - 1]) << 8);
  if (crc != respCrc) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应 CRC 不匹配");
  }
  if (byteCount != expectedByteCount) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应字节数不匹配");
  }

  outData->assign(frame.begin() + 3, frame.begin() + 3 + byteCount);
  return grpc::Status::OK;
}

grpc::Status MqttBus::parseWriteSingleRegisterResponse(const std::vector<uint8_t>& frame,
                                                       uint8_t expectedSlaveId,
                                                       uint16_t expectedAddress,
                                                       uint16_t expectedValue) const {
  if (frame.size() < 5) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应帧过短");
  }
  if (frame[0] != expectedSlaveId) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应 slave_id 不匹配");
  }

  const uint8_t function = frame[1];
  if (function == static_cast<uint8_t>(kFunctionWriteSingleRegister | 0x80)) {
    if (frame.size() != 5) {
      return grpc::Status(grpc::StatusCode::UNAVAILABLE, "异常响应长度异常");
    }
    const uint16_t crc = SerialBus::computeCrc(frame.data(), 3);
    const uint16_t respCrc = static_cast<uint16_t>(frame[3]) | (static_cast<uint16_t>(frame[4]) << 8);
    if (crc != respCrc) {
      return grpc::Status(grpc::StatusCode::UNAVAILABLE, "异常响应 CRC 不匹配");
    }
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::format("Modbus 异常: {}", frame[2]));
  }

  if (function != kFunctionWriteSingleRegister) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应功能码不匹配");
  }
  if (frame.size() != 8) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "写单寄存器响应长度不匹配");
  }

  const uint16_t crc = SerialBus::computeCrc(frame.data(), frame.size() - 2);
  const uint16_t respCrc = static_cast<uint16_t>(frame[6]) | (static_cast<uint16_t>(frame[7]) << 8);
  if (crc != respCrc) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应 CRC 不匹配");
  }

  const uint16_t address = static_cast<uint16_t>((static_cast<uint16_t>(frame[2]) << 8) | frame[3]);
  const uint16_t value = static_cast<uint16_t>((static_cast<uint16_t>(frame[4]) << 8) | frame[5]);
  if (address != expectedAddress) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "写单寄存器响应地址不匹配");
  }
  if (value != expectedValue) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "写单寄存器响应值不匹配");
  }
  return grpc::Status::OK;
}

grpc::Status MqttBus::parseWriteMultipleRegistersResponse(const std::vector<uint8_t>& frame,
                                                          uint8_t expectedSlaveId,
                                                          uint16_t expectedAddress,
                                                          uint16_t expectedQuantity) const {
  if (frame.size() < 5) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应帧过短");
  }
  if (frame[0] != expectedSlaveId) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应 slave_id 不匹配");
  }

  const uint8_t function = frame[1];
  if (function == static_cast<uint8_t>(kFunctionWriteMultipleRegisters | 0x80)) {
    if (frame.size() != 5) {
      return grpc::Status(grpc::StatusCode::UNAVAILABLE, "异常响应长度异常");
    }
    const uint16_t crc = SerialBus::computeCrc(frame.data(), 3);
    const uint16_t respCrc = static_cast<uint16_t>(frame[3]) | (static_cast<uint16_t>(frame[4]) << 8);
    if (crc != respCrc) {
      return grpc::Status(grpc::StatusCode::UNAVAILABLE, "异常响应 CRC 不匹配");
    }
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::format("Modbus 异常: {}", frame[2]));
  }

  if (function != kFunctionWriteMultipleRegisters) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应功能码不匹配");
  }
  if (frame.size() != 8) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "写多寄存器响应长度不匹配");
  }

  const uint16_t crc = SerialBus::computeCrc(frame.data(), frame.size() - 2);
  const uint16_t respCrc = static_cast<uint16_t>(frame[6]) | (static_cast<uint16_t>(frame[7]) << 8);
  if (crc != respCrc) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应 CRC 不匹配");
  }

  const uint16_t address = static_cast<uint16_t>((static_cast<uint16_t>(frame[2]) << 8) | frame[3]);
  const uint16_t quantity = static_cast<uint16_t>((static_cast<uint16_t>(frame[4]) << 8) | frame[5]);
  if (address != expectedAddress) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "写多寄存器响应地址不匹配");
  }
  if (quantity != expectedQuantity) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "写多寄存器响应数量不匹配");
  }
  return grpc::Status::OK;
}

std::string MqttBus::requestTopic() const {
  return std::format("{}/{}/JSON/transparant/notification/{}/data",
                     kAppName,
                     kAppTypeUart,
                     config_.serial_port());
}

std::string MqttBus::responseTopic() const {
  return std::format("{}/{}/JSON/transparant/notification/{}/data",
                     kAppTypeUart,
                     kAppName,
                     config_.serial_port());
}

std::string MqttBus::formatTimestamp() {
  const auto ms = nowMs();
  const auto sec = ms / 1000;
  const auto milli = ms % 1000;
  std::time_t t = static_cast<std::time_t>(sec);
  std::tm tm = *std::gmtime(&t);
  return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}+0000",
                     tm.tm_year + 1900,
                     tm.tm_mon + 1,
                     tm.tm_mday,
                     tm.tm_hour,
                     tm.tm_min,
                     tm.tm_sec,
                     milli);
}

uint64_t MqttBus::nowMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count());
}

std::string MqttBus::base64Encode(const std::vector<uint8_t>& data) {
  static const char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  for (size_t i = 0; i < data.size(); i += 3) {
    const uint32_t octetA = data[i];
    const uint32_t octetB = (i + 1 < data.size()) ? data[i + 1] : 0;
    const uint32_t octetC = (i + 2 < data.size()) ? data[i + 2] : 0;

    const uint32_t triple = (octetA << 16) | (octetB << 8) | octetC;
    out.push_back(kTable[(triple >> 18) & 0x3F]);
    out.push_back(kTable[(triple >> 12) & 0x3F]);
    out.push_back(i + 1 < data.size() ? kTable[(triple >> 6) & 0x3F] : '=');
    out.push_back(i + 2 < data.size() ? kTable[triple & 0x3F] : '=');
  }
  return out;
}

bool MqttBus::base64Decode(std::string_view text, std::vector<uint8_t>* out) {
  if (out == nullptr) {
    return false;
  }
  static const int kDecodeTable[256] = {
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
      52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-2,-1,-1,
      -1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,
      15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
      -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
      41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};

  out->clear();
  uint32_t buffer = 0;
  int bitsCollected = 0;
  for (char ch : text) {
    const int value = kDecodeTable[static_cast<unsigned char>(ch)];
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

int32_t MqttBus::parseStatusCode(const boost::json::value& value, bool* ok) {
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
  std::string text = trimAscii(value.as_string().c_str());
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

std::string MqttBus::serialParityToText(ModbusRTUProto::Parity parity) {
  switch (parity) {
    case ModbusRTUProto::PARITY_ODD:
      return "odd";
    case ModbusRTUProto::PARITY_EVEN:
      return "even";
    case ModbusRTUProto::PARITY_NONE:
    case ModbusRTUProto::PARITY_UNSPECIFIED:
    default:
      return "none";
  }
}

uint32_t MqttBus::serialStopBitsToNumber(ModbusRTUProto::StopBits stopBits) {
  switch (stopBits) {
    case ModbusRTUProto::STOP_BITS_TWO:
      return 2;
    case ModbusRTUProto::STOP_BITS_ONE:
    case ModbusRTUProto::STOP_BITS_UNSPECIFIED:
    default:
      return 1;
  }
}

std::string MqttBus::trimAscii(std::string text) {
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

std::string MqttBus::toLowerAscii(std::string text) {
  for (char& ch : text) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return text;
}

bool MqttBus::parseInt32Text(const std::string& text, int32_t* out) {
  if (out == nullptr || text.empty()) {
    return false;
  }
  char* end = nullptr;
  const auto value = std::strtol(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') {
    return false;
  }
  *out = static_cast<int32_t>(value);
  return true;
}

std::string MqttBus::bytesToHex(const std::vector<uint8_t>& data) {
  if (data.empty()) {
    return {};
  }
  std::string out;
  out.reserve(data.size() * 3 - 1);
  for (size_t i = 0; i < data.size(); ++i) {
    const auto byte = data[i];
    out.push_back(kHexDigits[(byte >> 4) & 0x0F]);
    out.push_back(kHexDigits[byte & 0x0F]);
    if (i + 1 != data.size()) {
      out.push_back(' ');
    }
  }
  return out;
}

}  // namespace ModbusRTU
