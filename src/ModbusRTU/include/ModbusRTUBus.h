#pragma once

#include <cstdint>
#include <vector>

#include <grpcpp/support/status.h>

namespace ModbusRTU {

class Bus {
public:
  virtual ~Bus() = default;

  virtual grpc::Status Open() = 0;
  virtual void Close() = 0;

  virtual grpc::Status ReadCoil(uint8_t slaveId, uint16_t address, bool* out) = 0;
  virtual grpc::Status ReadHoldingRegister(uint8_t slaveId, uint16_t address, uint16_t* out) = 0;
  virtual grpc::Status ReadHoldingRegisters(uint8_t slaveId,
                                            uint16_t address,
                                            uint16_t quantity,
                                            std::vector<uint16_t>* out) = 0;
  virtual grpc::Status ReadInputRegister(uint8_t slaveId, uint16_t address, uint16_t* out) = 0;
  virtual grpc::Status ReadInputRegisters(uint8_t slaveId,
                                          uint16_t address,
                                          uint16_t quantity,
                                          std::vector<uint16_t>* out) = 0;
  virtual grpc::Status WriteSingleRegister(uint8_t slaveId, uint16_t address, uint16_t value) = 0;
  virtual grpc::Status WriteMultipleRegisters(uint8_t slaveId,
                                              uint16_t address,
                                              const std::vector<uint16_t>& values) = 0;
};

}  // namespace ModbusRTU
