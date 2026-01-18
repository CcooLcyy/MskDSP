#include "ModbusRTUSerialBus.h"

#include <array>
#include <chrono>
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
constexpr uint8_t kFunctionWriteMultipleCoils = 0x0F;
constexpr uint8_t kFunctionWriteMultipleRegisters = 0x10;
constexpr char kHexDigits[] = "0123456789ABCDEF";

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

grpc::Status SerialBus::ReadCoil(uint8_t slaveId, uint16_t address, bool* out) {
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
  appendCrc(&frame);

  status = writeRequestLocked(frame);
  if (!status.ok()) {
    return status;
  }

  std::vector<uint8_t> data;
  status = readResponseLocked(slaveId, kFunctionReadCoils, 1, &data);
  if (!status.ok()) {
    return status;
  }
  if (data.empty()) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "线圈响应为空");
  }
  *out = ((data[0] & 0x01) != 0);
  return grpc::Status::OK;
}

grpc::Status SerialBus::ReadHoldingRegister(uint8_t slaveId, uint16_t address, uint16_t* out) {
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
  frame.push_back(kFunctionReadHoldingRegisters);
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
  status = readResponseLocked(slaveId, kFunctionReadHoldingRegisters, 1, &data);
  if (!status.ok()) {
    return status;
  }
  if (data.size() != 2) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "保持寄存器响应长度异常");
  }
  *out = static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
  return grpc::Status::OK;
}

grpc::Status SerialBus::ReadRequest(RtuRequest* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto status = ensureOpenLocked();
  if (!status.ok()) {
    return status;
  }

  const auto timeout = std::chrono::milliseconds(config_.read_timeout_ms());
  std::array<uint8_t, 2> header{};
  status = readExactLocked(header.data(), header.size(), timeout);
  if (!status.ok()) {
    return status;
  }

  std::array<uint8_t, 4> body{};
  status = readExactLocked(body.data(), body.size(), timeout);
  if (!status.ok()) {
    return status;
  }

  std::vector<uint8_t> frame;
  frame.reserve(8);
  frame.insert(frame.end(), header.begin(), header.end());
  frame.insert(frame.end(), body.begin(), body.end());

  const uint8_t function = header[1];
  if (function == kFunctionWriteMultipleCoils || function == kFunctionWriteMultipleRegisters) {
    uint8_t byteCount = 0;
    status = readExactLocked(&byteCount, 1, timeout);
    if (!status.ok()) {
      return status;
    }
    frame.push_back(byteCount);
    std::vector<uint8_t> tail(static_cast<size_t>(byteCount) + 2, 0);
    status = readExactLocked(tail.data(), tail.size(), timeout);
    if (!status.ok()) {
      return status;
    }
    frame.insert(frame.end(), tail.begin(), tail.end());
  } else {
    std::array<uint8_t, 2> crcBytes{};
    status = readExactLocked(crcBytes.data(), crcBytes.size(), timeout);
    if (!status.ok()) {
      return status;
    }
    frame.insert(frame.end(), crcBytes.begin(), crcBytes.end());
  }

  if (frame.size() < 4) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "请求帧过短");
  }
  const uint16_t expectCrc = computeCrc(frame.data(), frame.size() - 2);
  const uint16_t gotCrc = static_cast<uint16_t>(frame[frame.size() - 2]) |
      (static_cast<uint16_t>(frame[frame.size() - 1]) << 8);
  if (expectCrc != gotCrc) {
    LOG_WARNING("ModbusRTU 请求 CRC 校验失败: device={}, 功能码={}",
                config_.device(), static_cast<unsigned int>(function));
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "请求 CRC 不匹配");
  }

  out->slaveId = header[0];
  out->function = function;
  out->address = static_cast<uint16_t>((static_cast<uint16_t>(body[0]) << 8) | body[1]);
  out->quantity = static_cast<uint16_t>((static_cast<uint16_t>(body[2]) << 8) | body[3]);
  out->frame = std::move(frame);
  LOG_INFO("ModbusRTU 报文接收: 设备={}, 长度={}, 数据={}",
           config_.device(),
           out->frame.size(),
           bytesToHex(out->frame));
  return grpc::Status::OK;
}

grpc::Status SerialBus::WriteFrame(const std::vector<uint8_t>& frame) {
  std::lock_guard<std::mutex> lock(mu_);
  auto status = ensureOpenLocked();
  if (!status.ok()) {
    return status;
  }
  return writeRequestLocked(frame);
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

grpc::Status SerialBus::readExactLocked(uint8_t* data, size_t len, std::chrono::milliseconds timeout) {
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
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "串口读取超时");
  }
  if (readEc) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, std::format("串口读取失败: {}", readEc.message()));
  }
  if (bytesRead != len) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "串口读取不完整");
  }
  return grpc::Status::OK;
}

grpc::Status SerialBus::readResponseLocked(
    uint8_t expectedSlaveId,
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

  if (header[0] != expectedSlaveId) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "响应 slave_id 不匹配");
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
