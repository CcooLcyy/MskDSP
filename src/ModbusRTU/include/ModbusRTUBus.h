#pragma once

#include <cstdint>
#include <format>
#include <vector>

#include <grpcpp/support/status.h>

namespace ModbusRTU {

// 只有完成地址、功能码、长度和 CRC 核对的读异常响应携带该标记。
inline constexpr char kReadExceptionDetails[] = "modbusrtu.read.exception";

inline grpc::Status MakeReadExceptionStatus(uint8_t exceptionCode) {
  return {grpc::StatusCode::FAILED_PRECONDITION,
          std::format("Modbus 异常: {}", exceptionCode), kReadExceptionDetails};
}

inline bool IsReadExceptionStatus(const grpc::Status& status) {
  return status.error_code() == grpc::StatusCode::FAILED_PRECONDITION &&
         status.error_details() == kReadExceptionDetails;
}

class Bus {
public:
  virtual ~Bus() = default;

  virtual grpc::Status Open() = 0;
  virtual void Close() = 0;

  virtual grpc::Status ReadCoil(uint8_t deviceId, uint16_t address, bool* out) = 0;
  virtual grpc::Status ReadHoldingRegister(uint8_t deviceId, uint16_t address, uint16_t* out) = 0;
  virtual grpc::Status ReadHoldingRegisters(uint8_t deviceId,
                                            uint16_t address,
                                            uint16_t quantity,
                                            std::vector<uint16_t>* out) = 0;
  virtual grpc::Status ReadInputRegister(uint8_t deviceId, uint16_t address, uint16_t* out) = 0;
  virtual grpc::Status ReadInputRegisters(uint8_t deviceId,
                                          uint16_t address,
                                          uint16_t quantity,
                                          std::vector<uint16_t>* out) = 0;
  virtual grpc::Status WriteSingleCoil(uint8_t deviceId, uint16_t address, bool value) = 0;
  virtual grpc::Status WriteSingleRegister(uint8_t deviceId, uint16_t address, uint16_t value) = 0;
  virtual grpc::Status WriteMultipleRegisters(uint8_t deviceId,
                                              uint16_t address,
                                              const std::vector<uint16_t>& values) = 0;
};

}  // namespace ModbusRTU
