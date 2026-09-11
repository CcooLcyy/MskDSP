#include "ModbusTCPTcpBus.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <sstream>

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include "Logger.h"

namespace ModbusTCP {
namespace {
void putU16(std::vector<uint8_t>* out, uint16_t value) {
  out->push_back(static_cast<uint8_t>(value >> 8));
  out->push_back(static_cast<uint8_t>(value & 0xff));
}

uint16_t getU16(const uint8_t* data) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

grpc::Status ioError(const char* operation, const boost::system::error_code& ec) {
  return grpc::Status(grpc::StatusCode::UNAVAILABLE, std::format("TCP {}失败: {}", operation, ec.message()));
}

grpc::Status validateResponse(const std::vector<uint8_t>& pdu, uint8_t expectedFunction) {
  if (pdu.empty()) {
    return grpc::Status(grpc::StatusCode::DATA_LOSS, "Modbus TCP 响应 PDU 为空");
  }
  if (pdu[0] == static_cast<uint8_t>(expectedFunction | 0x80)) {
    const auto code = pdu.size() > 1 ? pdu[1] : 0;
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::format("Modbus TCP 异常响应: 功能码=0x{:02X}, 异常码=0x{:02X}", expectedFunction, code));
  }
  if (pdu[0] != expectedFunction) {
    return grpc::Status(grpc::StatusCode::DATA_LOSS, std::format("Modbus TCP 响应功能码不匹配: 期望=0x{:02X}, 实际=0x{:02X}", expectedFunction, pdu[0]));
  }
  return grpc::Status::OK;
}
}  // namespace

TcpBus::TcpBus(ModbusTCPProto::TcpConfig config) : config_(std::move(config)), socket_(io_) {}

std::string TcpBus::formatHex(const std::vector<uint8_t>& bytes) {
  std::ostringstream out;
  for (size_t i = 0; i < bytes.size(); ++i) {
    if (i != 0) out << ' ';
    out << std::format("{:02X}", bytes[i]);
  }
  return out.str();
}

grpc::Status TcpBus::openLocked() {
  if (opened_) return grpc::Status::OK;
  if (config_.host().empty() || config_.port() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "TCP host/port 不能为空");
  }
  boost::system::error_code ec;
  boost::system::error_code ignored;
  socket_.close(ignored);
  io_.restart();
  boost::asio::ip::tcp::resolver resolver(io_);
  auto endpoints = resolver.resolve(config_.host(), std::to_string(config_.port()), ec);
  if (ec) return ioError("地址解析", ec);

  boost::asio::steady_timer timer(io_);
  bool completed = false;
  bool timedOut = false;
  boost::system::error_code connectError;
  boost::asio::async_connect(socket_, endpoints,
                             [&](const boost::system::error_code& connectEc, const auto&) {
                               connectError = connectEc;
                               completed = true;
                               timer.cancel();
                             });
  const auto timeout = std::chrono::milliseconds(
      config_.connect_timeout_ms() == 0 ? 3000 : config_.connect_timeout_ms());
  timer.expires_after(timeout);
  timer.async_wait([&](const boost::system::error_code& timerEc) {
    if (!timerEc && !completed) {
      timedOut = true;
      boost::system::error_code closeEc;
      socket_.close(closeEc);
    }
  });
  io_.run();
  if (timedOut) {
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "TCP 连接超时");
  }
  if (connectError) return ioError("连接", connectError);
  opened_ = true;
  transactionId_ = 0;
  LOG_INFO("ModbusTCP TCP 连接已打开: host={}, port={}, unit_id={}", config_.host(), config_.port(), config_.unit_id());
  return grpc::Status::OK;
}

grpc::Status TcpBus::Open() {
  std::lock_guard lock(mu_);
  return openLocked();
}

void TcpBus::Close() {
  std::lock_guard lock(mu_);
  boost::system::error_code ec;
  socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
  socket_.close(ec);
  opened_ = false;
  LOG_INFO("ModbusTCP TCP 连接已关闭: host={}, port={}", config_.host(), config_.port());
}

grpc::Status TcpBus::ensureOpenLocked() {
  if (opened_) return grpc::Status::OK;
  return openLocked();
}

grpc::Status TcpBus::transactLocked(uint8_t deviceId, const std::vector<uint8_t>& pdu, uint8_t expectedFunction, std::vector<uint8_t>* response) {
  if (response == nullptr) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "response 为空");
  auto status = ensureOpenLocked();
  if (!status.ok()) return status;
  const uint16_t transaction = ++transactionId_;
  std::vector<uint8_t> frame;
  frame.reserve(7 + pdu.size());
  putU16(&frame, transaction);
  putU16(&frame, 0);
  putU16(&frame, static_cast<uint16_t>(pdu.size() + 1));
  frame.push_back(deviceId);
  frame.insert(frame.end(), pdu.begin(), pdu.end());
  boost::system::error_code ec;
  io_.restart();
  boost::asio::steady_timer writeTimer(io_);
  bool writeDone = false;
  bool writeTimedOut = false;
  boost::asio::async_write(socket_, boost::asio::buffer(frame),
                           [&](const boost::system::error_code& writeEc, std::size_t) {
                             ec = writeEc;
                             writeDone = true;
                             writeTimer.cancel();
                           });
  writeTimer.expires_after(std::chrono::milliseconds(
      config_.request_timeout_ms() == 0 ? 3000 : config_.request_timeout_ms()));
  writeTimer.async_wait([&](const boost::system::error_code& timerEc) {
    if (!timerEc && !writeDone) {
      writeTimedOut = true;
      boost::system::error_code closeEc;
      socket_.close(closeEc);
    }
  });
  io_.run();
  LOG_INFO("ModbusTCP 报文发送: host={}, port={}, 数据={}", config_.host(), config_.port(), formatHex(frame));
  if (writeTimedOut) {
    opened_ = false;
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "TCP 发送超时");
  }
  if (ec) { opened_ = false; return ioError("发送", ec); }

  std::array<uint8_t, 6> header{};
  io_.restart();
  boost::asio::steady_timer readTimer(io_);
  bool readDone = false;
  bool readTimedOut = false;
  boost::asio::async_read(socket_, boost::asio::buffer(header),
                          [&](const boost::system::error_code& readEc, std::size_t) {
                            ec = readEc;
                            readDone = true;
                            readTimer.cancel();
                          });
  readTimer.expires_after(std::chrono::milliseconds(
      config_.request_timeout_ms() == 0 ? 3000 : config_.request_timeout_ms()));
  readTimer.async_wait([&](const boost::system::error_code& timerEc) {
    if (!timerEc && !readDone) {
      readTimedOut = true;
      boost::system::error_code closeEc;
      socket_.close(closeEc);
    }
  });
  io_.run();
  if (readTimedOut) {
    opened_ = false;
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "TCP 接收 MBAP 超时");
  }
  if (ec) { opened_ = false; return ioError("接收 MBAP", ec); }
  const uint16_t responseTransaction = getU16(header.data());
  const uint16_t protocol = getU16(header.data() + 2);
  const uint16_t length = getU16(header.data() + 4);
  if (responseTransaction != transaction) { opened_ = false; socket_.close(ec); return grpc::Status(grpc::StatusCode::DATA_LOSS, "Modbus TCP 事务标识不匹配"); }
  if (protocol != 0) { opened_ = false; socket_.close(ec); return grpc::Status(grpc::StatusCode::DATA_LOSS, "Modbus TCP 协议标识不为 0"); }
  if (length < 2 || length > 253) { opened_ = false; socket_.close(ec); return grpc::Status(grpc::StatusCode::DATA_LOSS, "Modbus TCP MBAP 长度非法"); }
  std::vector<uint8_t> body(length);
  io_.restart();
  boost::asio::steady_timer bodyTimer(io_);
  bool bodyDone = false;
  bool bodyTimedOut = false;
  boost::asio::async_read(socket_, boost::asio::buffer(body),
                          [&](const boost::system::error_code& bodyEc, std::size_t) {
                            ec = bodyEc;
                            bodyDone = true;
                            bodyTimer.cancel();
                          });
  bodyTimer.expires_after(std::chrono::milliseconds(
      config_.request_timeout_ms() == 0 ? 3000 : config_.request_timeout_ms()));
  bodyTimer.async_wait([&](const boost::system::error_code& timerEc) {
    if (!timerEc && !bodyDone) {
      bodyTimedOut = true;
      boost::system::error_code closeEc;
      socket_.close(closeEc);
    }
  });
  io_.run();
  if (bodyTimedOut) {
    opened_ = false;
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "TCP 接收 PDU 超时");
  }
  if (ec) { opened_ = false; return ioError("接收 PDU", ec); }
  if (body[0] != deviceId) { opened_ = false; socket_.close(ec); return grpc::Status(grpc::StatusCode::DATA_LOSS, "Modbus TCP Unit ID 不匹配"); }
  response->assign(body.begin() + 1, body.end());
  std::vector<uint8_t> full(header.begin(), header.end());
  full.insert(full.end(), body.begin(), body.end());
  LOG_INFO("ModbusTCP 报文接收: host={}, port={}, 数据={}", config_.host(), config_.port(), formatHex(full));
  auto responseStatus = validateResponse(*response, expectedFunction);
  if (!responseStatus.ok()) { opened_ = false; socket_.close(ec); }
  return responseStatus;
}

grpc::Status TcpBus::readRegisters(uint8_t deviceId, uint8_t function, uint16_t address, uint16_t quantity, std::vector<uint16_t>* out) {
  if (out == nullptr || quantity == 0 || quantity > 125) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "读取寄存器参数非法");
  std::lock_guard lock(mu_);
  std::vector<uint8_t> pdu{function}; putU16(&pdu, address); putU16(&pdu, quantity);
  std::vector<uint8_t> response;
  auto status = transactLocked(deviceId, pdu, function, &response);
  if (!status.ok()) return status;
  if (response.size() < 2 || response[1] != quantity * 2 || response.size() != static_cast<size_t>(response[1]) + 2) return grpc::Status(grpc::StatusCode::DATA_LOSS, "Modbus TCP 寄存器响应长度非法");
  out->clear(); out->reserve(quantity);
  for (size_t i = 0; i < quantity; ++i) out->push_back(getU16(response.data() + 2 + i * 2));
  return grpc::Status::OK;
}

grpc::Status TcpBus::ReadCoil(uint8_t deviceId, uint16_t address, bool* out) {
  if (out == nullptr) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  std::lock_guard lock(mu_); std::vector<uint8_t> pdu{0x01}; putU16(&pdu, address); putU16(&pdu, 1);
  std::vector<uint8_t> response; auto status = transactLocked(deviceId, pdu, 0x01, &response);
  if (!status.ok()) return status; if (response.size() != 3 || response[1] != 1) return grpc::Status(grpc::StatusCode::DATA_LOSS, "Modbus TCP 线圈响应长度非法"); *out = (response[2] & 1) != 0; return grpc::Status::OK;
}

grpc::Status TcpBus::ReadHoldingRegister(uint8_t id, uint16_t address, uint16_t* out) { std::vector<uint16_t> v; auto s = readRegisters(id, 0x03, address, 1, &v); if (s.ok() && out) *out = v[0]; return s; }
grpc::Status TcpBus::ReadHoldingRegisters(uint8_t id, uint16_t address, uint16_t quantity, std::vector<uint16_t>* out) { return readRegisters(id, 0x03, address, quantity, out); }
grpc::Status TcpBus::ReadInputRegister(uint8_t id, uint16_t address, uint16_t* out) { std::vector<uint16_t> v; auto s = readRegisters(id, 0x04, address, 1, &v); if (s.ok() && out) *out = v[0]; return s; }
grpc::Status TcpBus::ReadInputRegisters(uint8_t id, uint16_t address, uint16_t quantity, std::vector<uint16_t>* out) { return readRegisters(id, 0x04, address, quantity, out); }

grpc::Status TcpBus::writeSingle(uint8_t deviceId, uint8_t function, uint16_t address, uint16_t value) {
  std::lock_guard lock(mu_); std::vector<uint8_t> pdu{function}; putU16(&pdu, address); putU16(&pdu, value); std::vector<uint8_t> response; auto status = transactLocked(deviceId, pdu, function, &response); if (!status.ok()) return status; if (response != pdu) return grpc::Status(grpc::StatusCode::DATA_LOSS, "Modbus TCP 写单响应回显不匹配"); return grpc::Status::OK;
}

grpc::Status TcpBus::WriteSingleCoil(uint8_t id, uint16_t address, bool value) { return writeSingle(id, 0x05, address, value ? 0xff00 : 0); }
grpc::Status TcpBus::WriteSingleRegister(uint8_t id, uint16_t address, uint16_t value) { return writeSingle(id, 0x06, address, value); }

grpc::Status TcpBus::WriteMultipleRegisters(uint8_t id, uint16_t address, const std::vector<uint16_t>& values) {
  if (values.empty() || values.size() > 123) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "写多寄存器数量非法");
  std::lock_guard lock(mu_); std::vector<uint8_t> pdu{0x10}; putU16(&pdu, address); putU16(&pdu, static_cast<uint16_t>(values.size())); pdu.push_back(static_cast<uint8_t>(values.size() * 2)); for (auto value : values) putU16(&pdu, value);
  std::vector<uint8_t> response; auto status = transactLocked(id, pdu, 0x10, &response); if (!status.ok()) return status; if (response.size() != 5 || response[0] != 0x10 || getU16(response.data() + 1) != address || getU16(response.data() + 3) != values.size()) return grpc::Status(grpc::StatusCode::DATA_LOSS, "Modbus TCP 写多响应回显不匹配"); return grpc::Status::OK;
}
}  // namespace ModbusTCP
