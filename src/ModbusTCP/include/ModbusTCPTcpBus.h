#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <grpcpp/support/status.h>

#include "ModbusRTUBus.h"
#include "ModbusTCP.pb.h"

namespace ModbusTCP {

class TcpBus final : public ModbusRTU::Bus {
public:
  explicit TcpBus(ModbusTCPProto::TcpConfig config);

  grpc::Status Open() override;
  void Close() override;
  grpc::Status ReadCoil(uint8_t deviceId, uint16_t address, bool* out) override;
  grpc::Status ReadHoldingRegister(uint8_t deviceId, uint16_t address, uint16_t* out) override;
  grpc::Status ReadHoldingRegisters(uint8_t deviceId, uint16_t address, uint16_t quantity, std::vector<uint16_t>* out) override;
  grpc::Status ReadInputRegister(uint8_t deviceId, uint16_t address, uint16_t* out) override;
  grpc::Status ReadInputRegisters(uint8_t deviceId, uint16_t address, uint16_t quantity, std::vector<uint16_t>* out) override;
  grpc::Status WriteSingleCoil(uint8_t deviceId, uint16_t address, bool value) override;
  grpc::Status WriteSingleRegister(uint8_t deviceId, uint16_t address, uint16_t value) override;
  grpc::Status WriteMultipleRegisters(uint8_t deviceId, uint16_t address, const std::vector<uint16_t>& values) override;

private:
  grpc::Status openLocked();
  grpc::Status ensureOpenLocked();
  grpc::Status transactLocked(uint8_t deviceId,
                              const std::vector<uint8_t>& pdu,
                              uint8_t expectedFunction,
                              std::vector<uint8_t>* response);
  grpc::Status readRegisters(uint8_t deviceId, uint8_t function, uint16_t address, uint16_t quantity, std::vector<uint16_t>* out);
  grpc::Status writeSingle(uint8_t deviceId, uint8_t function, uint16_t address, uint16_t value);
  static std::string formatHex(const std::vector<uint8_t>& bytes);

  ModbusTCPProto::TcpConfig config_;
  boost::asio::io_context io_;
  boost::asio::ip::tcp::socket socket_;
  std::mutex mu_;
  bool opened_ = false;
  uint16_t transactionId_ = 0;
};

}  // namespace ModbusTCP
