#include "ModbusRTUSerialBus.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <vector>

#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>

#include "Logger.h"

namespace ModbusRTU {
namespace {
constexpr uint8_t kFunctionReadCoils = 0x01;
constexpr uint8_t kFunctionReadHoldingRegisters = 0x03;
constexpr uint8_t kFunctionReadInputRegisters = 0x04;
constexpr uint8_t kFunctionWriteSingleCoil = 0x05;
constexpr uint8_t kFunctionWriteSingleRegister = 0x06;
constexpr uint8_t kFunctionWriteMultipleRegisters = 0x10;
constexpr char kHexDigits[] = "0123456789ABCDEF";
constexpr size_t kMaxConsecutiveTimeouts = 3;
constexpr size_t kMaxWriteMultipleRegistersQuantity = 123;

std::string bytesToHex(const std::vector<uint8_t> &data) {
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
}  // namespace

SerialBus::SerialBus(ModbusRTUProto::SerialConfig config) :
  config_(std::move(config)),
  port_(io_) {}

const ModbusRTUProto::SerialConfig& SerialBus::config() const {
  return config_;
}

grpc::Status SerialBus::Open() {
  std::lock_guard<std::mutex> lock(mu_);
  return ensureOpenLocked();
}

void SerialBus::Close() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!opened_) {
    return;
  }
  boost::system::error_code ec;
  port_.close(ec);
  if (ec) {
    LOG_WARNING("ModbusRTU 串口关闭失败: device={}, 原因={}", config_.device(), ec.message());
  } else {
    LOG_INFO("ModbusRTU 串口已关闭: device={}", config_.device());
  }
  opened_ = false;
}

grpc::Status SerialBus::ReadCoil(uint8_t deviceId, uint16_t address, bool* out) {
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
  frame.push_back(deviceId);
  frame.push_back(kFunctionReadCoils);
  frame.push_back(static_cast<uint8_t>((address >> 8) & 0xFF));
  frame.push_back(static_cast<uint8_t>(address & 0xFF));
  frame.push_back(0x00);
  frame.push_back(0x01);
  appendCrc(&frame);

  status = writeRequestLocked(frame);
  if (!status.ok()) {
    return status;
  }

  std::vector<uint8_t> data;
  status = readResponseLocked(deviceId, kFunctionReadCoils, 1, &data);
  if (!status.ok()) {
    return status;
  }
  if (data.empty()) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "线圈响应为空");
  }
  *out = ((data[0] & 0x01) != 0);
  return grpc::Status::OK;
}

grpc::Status SerialBus::ReadHoldingRegister(uint8_t deviceId, uint16_t address, uint16_t* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  std::vector<uint16_t> values;
  auto status = ReadHoldingRegisters(deviceId, address, 1, &values);
  if (!status.ok()) {
    return status;
  }
  if (values.size() != 1) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "保持寄存器响应数量异常");
  }
  *out = values.front();
  return grpc::Status::OK;
}

grpc::Status SerialBus::ReadHoldingRegisters(uint8_t deviceId,
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
  frame.push_back(deviceId);
  frame.push_back(kFunctionReadHoldingRegisters);
  frame.push_back(static_cast<uint8_t>((address >> 8) & 0xFF));
  frame.push_back(static_cast<uint8_t>(address & 0xFF));
  frame.push_back(static_cast<uint8_t>((quantity >> 8) & 0xFF));
  frame.push_back(static_cast<uint8_t>(quantity & 0xFF));
  appendCrc(&frame);

  status = writeRequestLocked(frame);
  if (!status.ok()) {
    return status;
  }

  std::vector<uint8_t> data;
  status = readResponseLocked(deviceId, kFunctionReadHoldingRegisters, quantity, &data);
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
  LOG_DEBUG("ModbusRTU 保持寄存器响应解析完成: device={}, address={}, quantity={}, 数据={}",
            config_.device(),
            address,
            quantity,
            bytesToHex(data));
  return grpc::Status::OK;
}

grpc::Status SerialBus::ReadInputRegister(uint8_t deviceId, uint16_t address, uint16_t* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  std::vector<uint16_t> values;
  auto status = ReadInputRegisters(deviceId, address, 1, &values);
  if (!status.ok()) {
    return status;
  }
  if (values.size() != 1) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "输入寄存器响应数量异常");
  }
  *out = values.front();
  return grpc::Status::OK;
}

grpc::Status SerialBus::WriteSingleCoil(uint8_t deviceId, uint16_t address, bool value) {
  std::lock_guard<std::mutex> lock(mu_);
  auto status = ensureOpenLocked();
  if (!status.ok()) {
    return status;
  }

  std::vector<uint8_t> frame;
  frame.reserve(8);
  frame.push_back(deviceId);
  frame.push_back(kFunctionWriteSingleCoil);
  frame.push_back(static_cast<uint8_t>((address >> 8) & 0xFF));
  frame.push_back(static_cast<uint8_t>(address & 0xFF));
  frame.push_back(value ? 0xFF : 0x00);
  frame.push_back(0x00);
  appendCrc(&frame);
  LOG_INFO("ModbusRTU 写单线圈请求: 设备={}, 地址={}, 值={}, 报文={}",
           config_.device(), address, value, bytesToHex(frame));
  status = writeRequestLocked(frame);
  if (!status.ok()) {
    return status;
  }
  return readWriteSingleCoilResponseLocked(deviceId, address, value);
}

grpc::Status SerialBus::ReadInputRegisters(uint8_t deviceId,
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
  frame.push_back(deviceId);
  frame.push_back(kFunctionReadInputRegisters);
  frame.push_back(static_cast<uint8_t>((address >> 8) & 0xFF));
  frame.push_back(static_cast<uint8_t>(address & 0xFF));
  frame.push_back(static_cast<uint8_t>((quantity >> 8) & 0xFF));
  frame.push_back(static_cast<uint8_t>(quantity & 0xFF));
  appendCrc(&frame);

  status = writeRequestLocked(frame);
  if (!status.ok()) {
    return status;
  }

  std::vector<uint8_t> data;
  status = readResponseLocked(deviceId, kFunctionReadInputRegisters, quantity, &data);
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
  LOG_DEBUG("ModbusRTU 输入寄存器响应解析完成: device={}, address={}, quantity={}, 数据={}",
            config_.device(),
            address,
            quantity,
            bytesToHex(data));
  return grpc::Status::OK;
}

grpc::Status SerialBus::WriteSingleRegister(uint8_t deviceId, uint16_t address, uint16_t value) {
  std::lock_guard<std::mutex> lock(mu_);
  auto status = ensureOpenLocked();
  if (!status.ok()) {
    return status;
  }

  std::vector<uint8_t> frame;
  frame.reserve(8);
  frame.push_back(deviceId);
  frame.push_back(kFunctionWriteSingleRegister);
  frame.push_back(static_cast<uint8_t>((address >> 8) & 0xFF));
  frame.push_back(static_cast<uint8_t>(address & 0xFF));
  frame.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  frame.push_back(static_cast<uint8_t>(value & 0xFF));
  appendCrc(&frame);

  status = writeRequestLocked(frame);
  if (!status.ok()) {
    return status;
  }
  return readWriteSingleRegisterResponseLocked(deviceId, address, value);
}

grpc::Status SerialBus::WriteMultipleRegisters(uint8_t deviceId,
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
  frame.push_back(deviceId);
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
  appendCrc(&frame);

  status = writeRequestLocked(frame);
  if (!status.ok()) {
    return status;
  }
  return readWriteMultipleRegistersResponseLocked(deviceId, address, quantity);
}

grpc::Status SerialBus::ensureOpenLocked() {
  if (opened_) {
    return grpc::Status::OK;
  }
  if (config_.device().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "serial.device 不能为空");
  }

  boost::system::error_code ec;
  port_.open(config_.device(), ec);
  if (ec) {
    LOG_ERROR("ModbusRTU 串口打开失败: device={}, 原因={}", config_.device(), ec.message());
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, std::format("打开串口失败: {}", ec.message()));
  }

  auto closeOnError = [this]() {
    boost::system::error_code ignore;
    port_.close(ignore);
  };

  port_.set_option(boost::asio::serial_port_base::baud_rate(config_.baud_rate()), ec);
  if (ec) {
    closeOnError();
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, std::format("设置 baud_rate 失败: {}", ec.message()));
  }
  port_.set_option(boost::asio::serial_port_base::character_size(config_.data_bits()), ec);
  if (ec) {
    closeOnError();
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, std::format("设置 data_bits 失败: {}", ec.message()));
  }

  boost::asio::serial_port_base::parity::type parity = boost::asio::serial_port_base::parity::none;
  if (config_.parity() == ModbusRTUProto::PARITY_ODD) {
    parity = boost::asio::serial_port_base::parity::odd;
  } else if (config_.parity() == ModbusRTUProto::PARITY_EVEN) {
    parity = boost::asio::serial_port_base::parity::even;
  }
  port_.set_option(boost::asio::serial_port_base::parity(parity), ec);
  if (ec) {
    closeOnError();
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, std::format("设置 parity 失败: {}", ec.message()));
  }

  boost::asio::serial_port_base::stop_bits::type stopBits = boost::asio::serial_port_base::stop_bits::one;
  if (config_.stop_bits() == ModbusRTUProto::STOP_BITS_TWO) {
    stopBits = boost::asio::serial_port_base::stop_bits::two;
  }
  port_.set_option(boost::asio::serial_port_base::stop_bits(stopBits), ec);
  if (ec) {
    closeOnError();
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, std::format("设置 stop_bits 失败: {}", ec.message()));
  }

  port_.set_option(boost::asio::serial_port_base::flow_control(boost::asio::serial_port_base::flow_control::none), ec);
  if (ec) {
    closeOnError();
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, std::format("设置 flow_control 失败: {}", ec.message()));
  }

  opened_ = true;
  LOG_INFO("ModbusRTU 串口已打开: device={}, baud={}, data_bits={}, parity={}, stop_bits={}",
           config_.device(),
           config_.baud_rate(),
           config_.data_bits(),
           static_cast<int>(config_.parity()),
           static_cast<int>(config_.stop_bits()));
  return grpc::Status::OK;
}

grpc::Status SerialBus::writeRequestLocked(const std::vector<uint8_t>& frame) {
  boost::system::error_code ec;
  boost::asio::write(port_, boost::asio::buffer(frame), ec);
  if (ec) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, std::format("串口写入失败: {}", ec.message()));
  }
  LOG_INFO("ModbusRTU 报文发送: 设备={}, 长度={}, 数据={}",
           config_.device(),
           frame.size(),
           bytesToHex(frame));
  return grpc::Status::OK;
}

grpc::Status SerialBus::readExactLocked(uint8_t* data,
                                        size_t len,
                                        std::chrono::milliseconds timeout,
                                        bool countTimeout) {
  if (len == 0) {
    return grpc::Status::OK;
  }

  boost::system::error_code readEc;
  size_t bytesRead = 0;
  bool timedOut = false;
  boost::asio::steady_timer timer(io_);
  timer.expires_after(timeout);

  boost::asio::async_read(port_, boost::asio::buffer(data, len),
                          [&](const boost::system::error_code& ec, size_t bytes) {
                            readEc = ec;
                            bytesRead = bytes;
                            timer.cancel();
                          });
  timer.async_wait([&](const boost::system::error_code& ec) {
    if (!ec) {
      timedOut = true;
      port_.cancel();
    }
  });

  io_.restart();
  io_.run();

  if (timedOut) {
    if (countTimeout) {
      ++consecutiveTimeouts_;
      if (consecutiveTimeouts_ >= kMaxConsecutiveTimeouts) {
        LOG_WARNING("ModbusRTU 串口连续读取超时，准备重置: device={}, 连续超时次数={}, 超时={}ms",
                    config_.device(),
                    consecutiveTimeouts_,
                    timeout.count());
        boost::system::error_code ec;
        port_.close(ec);
        if (ec) {
          LOG_WARNING("ModbusRTU 串口重置关闭失败: device={}, 原因={}", config_.device(), ec.message());
        } else {
          LOG_INFO("ModbusRTU 串口已重置: device={}", config_.device());
        }
        opened_ = false;
        consecutiveTimeouts_ = 0;
      }
    }
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "串口读取超时");
  }
  if (readEc) {
    if (countTimeout) {
      consecutiveTimeouts_ = 0;
    }
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, std::format("串口读取失败: {}", readEc.message()));
  }
  if (bytesRead != len) {
    if (countTimeout) {
      consecutiveTimeouts_ = 0;
    }
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "串口读取不完整");
  }
  if (countTimeout) {
    consecutiveTimeouts_ = 0;
  }
  return grpc::Status::OK;
}

grpc::Status SerialBus::readResponseLocked(
    uint8_t expectedDeviceId,
    uint8_t expectedFunction,
    uint16_t quantity,
    std::vector<uint8_t>* outData) {
  if (outData == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "outData 为空");
  }
  if (quantity == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "quantity 不能为空");
  }

  const auto timeout = std::chrono::milliseconds(config_.read_timeout_ms());
  std::vector<uint8_t> header(3, 0);
  auto status = readExactLocked(header.data(), header.size(), timeout);
  if (!status.ok()) {
    return status;
  }

  if (header[0] != expectedDeviceId) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应设备地址不匹配");
  }

  const uint8_t function = header[1];
  if (function == static_cast<uint8_t>(expectedFunction | 0x80)) {
    std::vector<uint8_t> tail(2, 0);
    auto tailStatus = readExactLocked(tail.data(), tail.size(), timeout);
    if (!tailStatus.ok()) {
      return tailStatus;
    }
    std::vector<uint8_t> frame;
    frame.reserve(header.size() + tail.size());
    frame.insert(frame.end(), header.begin(), header.end());
    frame.insert(frame.end(), tail.begin(), tail.end());
    LOG_INFO("ModbusRTU 报文接收: 设备={}, 长度={}, 数据={}",
             config_.device(),
             frame.size(),
             bytesToHex(frame));
    const uint16_t crc = computeCrc(header.data(), header.size());
    const uint16_t respCrc = static_cast<uint16_t>(tail[0]) | (static_cast<uint16_t>(tail[1]) << 8);
    if (crc != respCrc) {
      return grpc::Status(grpc::StatusCode::UNAVAILABLE, "异常响应 CRC 不匹配");
    }
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::format("Modbus 异常: {}", header[2]));
  }

  if (function != expectedFunction) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应功能码不匹配");
  }

  const uint8_t byteCount = header[2];
  const size_t expectedByteCount =
      (expectedFunction == kFunctionReadCoils) ? ((quantity + 7) / 8) : (static_cast<size_t>(quantity) * 2);
  std::vector<uint8_t> body(byteCount + 2, 0);
  status = readExactLocked(body.data(), body.size(), timeout);
  if (!status.ok()) {
    return status;
  }

  std::vector<uint8_t> frame;
  frame.reserve(header.size() + body.size());
  frame.insert(frame.end(), header.begin(), header.end());
  frame.insert(frame.end(), body.begin(), body.end());
  LOG_INFO("ModbusRTU 报文接收: 设备={}, 长度={}, 数据={}",
           config_.device(),
           frame.size(),
           bytesToHex(frame));

  const uint16_t crc = computeCrc(frame.data(), frame.size() - 2);
  const uint16_t respCrc = static_cast<uint16_t>(body[byteCount]) | (static_cast<uint16_t>(body[byteCount + 1]) << 8);
  if (crc != respCrc) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应 CRC 不匹配");
  }
  if (byteCount != expectedByteCount) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应字节数不匹配");
  }

  outData->assign(body.begin(), body.begin() + byteCount);
  return grpc::Status::OK;
}

grpc::Status SerialBus::readWriteSingleRegisterResponseLocked(
    uint8_t expectedDeviceId,
    uint16_t expectedAddress,
    uint16_t expectedValue) {
  const auto timeout = std::chrono::milliseconds(config_.read_timeout_ms());
  std::array<uint8_t, 2> header{};
  auto status = readExactLocked(header.data(), header.size(), timeout);
  if (!status.ok()) {
    return status;
  }

  if (header[1] == static_cast<uint8_t>(kFunctionWriteSingleRegister | 0x80)) {
    std::array<uint8_t, 3> body{};
    status = readExactLocked(body.data(), body.size(), timeout);
    if (!status.ok()) {
      return status;
    }
    std::vector<uint8_t> frame;
    frame.reserve(header.size() + body.size());
    frame.insert(frame.end(), header.begin(), header.end());
    frame.insert(frame.end(), body.begin(), body.end());
    LOG_INFO("ModbusRTU 报文接收: 设备={}, 长度={}, 数据={}",
             config_.device(),
             frame.size(),
             bytesToHex(frame));
    const uint16_t crc = computeCrc(frame.data(), 3);
    const uint16_t respCrc = static_cast<uint16_t>(frame[3]) | (static_cast<uint16_t>(frame[4]) << 8);
    if (crc != respCrc) {
      return grpc::Status(grpc::StatusCode::UNAVAILABLE, "异常响应 CRC 不匹配");
    }
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::format("Modbus 异常: {}", frame[2]));
  }

  std::array<uint8_t, 6> body{};
  status = readExactLocked(body.data(), body.size(), timeout);
  if (!status.ok()) {
    return status;
  }

  std::vector<uint8_t> frame;
  frame.reserve(header.size() + body.size());
  frame.insert(frame.end(), header.begin(), header.end());
  frame.insert(frame.end(), body.begin(), body.end());
  LOG_INFO("ModbusRTU 报文接收: 设备={}, 长度={}, 数据={}",
           config_.device(),
           frame.size(),
           bytesToHex(frame));

  const uint16_t crc = computeCrc(frame.data(), frame.size() - 2);
  const uint16_t respCrc = static_cast<uint16_t>(frame[6]) | (static_cast<uint16_t>(frame[7]) << 8);
  if (crc != respCrc) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应 CRC 不匹配");
  }
  if (frame[0] != expectedDeviceId) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应设备地址不匹配");
  }
  if (frame[1] != kFunctionWriteSingleRegister) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应功能码不匹配");
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

grpc::Status SerialBus::readWriteSingleCoilResponseLocked(
    uint8_t expectedDeviceId, uint16_t expectedAddress, bool expectedValue) {
  const auto timeout = std::chrono::milliseconds(config_.read_timeout_ms());
  std::array<uint8_t, 2> header{};
  auto status = readExactLocked(header.data(), header.size(), timeout);
  if (!status.ok()) {
    return status;
  }
  const bool exception = header[1] == static_cast<uint8_t>(kFunctionWriteSingleCoil | 0x80);
  std::array<uint8_t, 6> body{};
  const size_t bodySize = exception ? 3 : 6;
  status = readExactLocked(body.data(), bodySize, timeout);
  if (!status.ok()) {
    return status;
  }
  std::vector<uint8_t> frame;
  frame.reserve(2 + bodySize);
  frame.insert(frame.end(), header.begin(), header.end());
  frame.insert(frame.end(), body.begin(), body.begin() + static_cast<std::ptrdiff_t>(bodySize));
  LOG_INFO("ModbusRTU 写单线圈响应: 设备={}, 地址={}, 值={}, 报文={}",
           config_.device(), expectedAddress, expectedValue, bytesToHex(frame));
  const uint16_t crc = computeCrc(frame.data(), frame.size() - 2);
  const uint16_t respCrc = static_cast<uint16_t>(frame[frame.size() - 2]) |
      (static_cast<uint16_t>(frame[frame.size() - 1]) << 8);
  if (crc != respCrc) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "写单线圈响应 CRC 不匹配");
  }
  if (frame[0] != expectedDeviceId) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应设备地址不匹配");
  }
  if (exception) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::format("Modbus 异常: {}", frame[2]));
  }
  if (frame[1] != kFunctionWriteSingleCoil) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "写单线圈响应功能码不匹配");
  }
  const uint16_t address = static_cast<uint16_t>((static_cast<uint16_t>(frame[2]) << 8) | frame[3]);
  const uint16_t wireValue = static_cast<uint16_t>((static_cast<uint16_t>(frame[4]) << 8) | frame[5]);
  if (address != expectedAddress) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "写单线圈响应地址不匹配");
  }
  const uint16_t expectedWireValue = expectedValue ? 0xFF00 : 0x0000;
  if (wireValue != expectedWireValue) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "写单线圈响应值不匹配");
  }
  return grpc::Status::OK;
}

grpc::Status SerialBus::readWriteMultipleRegistersResponseLocked(
    uint8_t expectedDeviceId,
    uint16_t expectedAddress,
    uint16_t expectedQuantity) {
  const auto timeout = std::chrono::milliseconds(config_.read_timeout_ms());
  std::array<uint8_t, 2> header{};
  auto status = readExactLocked(header.data(), header.size(), timeout);
  if (!status.ok()) {
    return status;
  }

  if (header[1] == static_cast<uint8_t>(kFunctionWriteMultipleRegisters | 0x80)) {
    std::array<uint8_t, 3> body{};
    status = readExactLocked(body.data(), body.size(), timeout);
    if (!status.ok()) {
      return status;
    }
    std::vector<uint8_t> frame;
    frame.reserve(header.size() + body.size());
    frame.insert(frame.end(), header.begin(), header.end());
    frame.insert(frame.end(), body.begin(), body.end());
    LOG_INFO("ModbusRTU 报文接收: 设备={}, 长度={}, 数据={}",
             config_.device(),
             frame.size(),
             bytesToHex(frame));
    const uint16_t crc = computeCrc(frame.data(), 3);
    const uint16_t respCrc = static_cast<uint16_t>(frame[3]) | (static_cast<uint16_t>(frame[4]) << 8);
    if (crc != respCrc) {
      return grpc::Status(grpc::StatusCode::UNAVAILABLE, "异常响应 CRC 不匹配");
    }
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::format("Modbus 异常: {}", frame[2]));
  }

  std::array<uint8_t, 6> body{};
  status = readExactLocked(body.data(), body.size(), timeout);
  if (!status.ok()) {
    return status;
  }

  std::vector<uint8_t> frame;
  frame.reserve(header.size() + body.size());
  frame.insert(frame.end(), header.begin(), header.end());
  frame.insert(frame.end(), body.begin(), body.end());
  LOG_INFO("ModbusRTU 报文接收: 设备={}, 长度={}, 数据={}",
           config_.device(),
           frame.size(),
           bytesToHex(frame));

  const uint16_t crc = computeCrc(frame.data(), frame.size() - 2);
  const uint16_t respCrc = static_cast<uint16_t>(frame[6]) | (static_cast<uint16_t>(frame[7]) << 8);
  if (crc != respCrc) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应 CRC 不匹配");
  }
  if (frame[0] != expectedDeviceId) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应设备地址不匹配");
  }
  if (frame[1] != kFunctionWriteMultipleRegisters) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应功能码不匹配");
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

uint16_t SerialBus::computeCrc(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= static_cast<uint16_t>(data[i]);
    for (int bit = 0; bit < 8; ++bit) {
      if (crc & 0x0001) {
        crc = static_cast<uint16_t>((crc >> 1) ^ 0xA001);
      } else {
        crc = static_cast<uint16_t>(crc >> 1);
      }
    }
  }
  return crc;
}

void SerialBus::appendCrc(std::vector<uint8_t>* frame) {
  if (frame == nullptr) {
    return;
  }
  const uint16_t crc = computeCrc(frame->data(), frame->size());
  frame->push_back(static_cast<uint8_t>(crc & 0xFF));
  frame->push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
}

}  // namespace ModbusRTU
