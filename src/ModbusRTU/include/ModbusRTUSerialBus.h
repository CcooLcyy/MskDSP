#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/serial_port.hpp>
#include <grpcpp/support/status.h>

#include "ModbusRTU.pb.h"

namespace ModbusRTU {

class SerialBus {
public:
  struct RtuRequest {
    uint8_t slaveId = 0;
    uint8_t function = 0;
    uint16_t address = 0;
    uint16_t quantity = 0;
    std::vector<uint8_t> frame;
  };

  explicit SerialBus(ModbusRTUProto::SerialConfig config);

  grpc::Status Open();
  void Close();

  grpc::Status ReadCoil(uint8_t slaveId, uint16_t address, bool* out);
  grpc::Status ReadHoldingRegister(uint8_t slaveId, uint16_t address, uint16_t* out);
  grpc::Status ReadHoldingRegisters(uint8_t slaveId, uint16_t address, uint16_t quantity, std::vector<uint16_t>* out);

  grpc::Status ReadRequest(RtuRequest* out);
  grpc::Status WriteFrame(const std::vector<uint8_t>& frame);

  static uint16_t computeCrc(const uint8_t* data, size_t len);
  static void appendCrc(std::vector<uint8_t>* frame);

  const ModbusRTUProto::SerialConfig& config() const;

private:
  grpc::Status ensureOpenLocked();
  grpc::Status writeRequestLocked(const std::vector<uint8_t>& frame);
  grpc::Status readExactLocked(uint8_t* data, size_t len, std::chrono::milliseconds timeout);
  grpc::Status readResponseLocked(
      uint8_t expectedSlaveId,
      uint8_t expectedFunction,
      uint16_t quantity,
      std::vector<uint8_t>* outData);

  ModbusRTUProto::SerialConfig config_;
  boost::asio::io_context io_;
  boost::asio::serial_port port_;
  std::mutex mu_;
  bool opened_ = false;
};

}  // namespace ModbusRTU
