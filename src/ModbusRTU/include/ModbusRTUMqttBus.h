#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <boost/json/value.hpp>

#include "ModbusRTUBus.h"
#include "ModbusRTUMqttClient.h"
#include "ModbusRTU.pb.h"

namespace ModbusRTU {

class MqttBus : public Bus {
public:
  MqttBus(ModbusRTUProto::LinkConfig config, MqttClient* client);

  grpc::Status Open() override;
  void Close() override;

  grpc::Status ReadCoil(uint8_t deviceId, uint16_t address, bool* out) override;
  grpc::Status ReadHoldingRegister(uint8_t deviceId, uint16_t address, uint16_t* out) override;
  grpc::Status ReadHoldingRegisters(uint8_t deviceId,
                                    uint16_t address,
                                    uint16_t quantity,
                                    std::vector<uint16_t>* out) override;
  grpc::Status ReadInputRegister(uint8_t deviceId, uint16_t address, uint16_t* out) override;
  grpc::Status ReadInputRegisters(uint8_t deviceId,
                                  uint16_t address,
                                  uint16_t quantity,
                                  std::vector<uint16_t>* out) override;
  grpc::Status WriteSingleCoil(uint8_t deviceId, uint16_t address, bool value) override;
  grpc::Status WriteSingleRegister(uint8_t deviceId, uint16_t address, uint16_t value) override;
  grpc::Status WriteMultipleRegisters(uint8_t deviceId,
                                      uint16_t address,
                                      const std::vector<uint16_t>& values) override;

private:
  grpc::Status ensureOpenLocked();
  grpc::Status sendFrame(const std::vector<uint8_t>& frame, std::vector<uint8_t>* outFrame);
  grpc::Status parseReadResponse(const std::vector<uint8_t>& frame,
                                 uint8_t expectedDeviceId,
                                 uint8_t expectedFunction,
                                 uint16_t quantity,
                                 std::vector<uint8_t>* outData) const;
  grpc::Status parseWriteSingleRegisterResponse(const std::vector<uint8_t>& frame,
                                                uint8_t expectedDeviceId,
                                                uint16_t expectedAddress,
                                                uint16_t expectedValue) const;
  grpc::Status parseWriteSingleCoilResponse(const std::vector<uint8_t>& frame,
                                            uint8_t expectedDeviceId,
                                            uint16_t expectedAddress,
                                            bool expectedValue) const;
  grpc::Status parseWriteMultipleRegistersResponse(const std::vector<uint8_t>& frame,
                                                   uint8_t expectedDeviceId,
                                                   uint16_t expectedAddress,
                                                   uint16_t expectedQuantity) const;
  std::string requestTopic() const;
  std::string responseTopic() const;
  static std::string formatTimestamp();
  static uint64_t nowMs();
  static std::string base64Encode(const std::vector<uint8_t>& data);
  static bool base64Decode(std::string_view text, std::vector<uint8_t>* out);
  static int32_t parseStatusCode(const boost::json::value& value, bool* ok);
  static std::string serialParityToText(ModbusRTUProto::Parity parity);
  static uint32_t serialStopBitsToNumber(ModbusRTUProto::StopBits stopBits);
  static std::string trimAscii(std::string text);
  static std::string toLowerAscii(std::string text);
  static bool parseInt32Text(const std::string& text, int32_t* out);
  static std::string bytesToHex(const std::vector<uint8_t>& data);

  ModbusRTUProto::LinkConfig config_;
  MqttClient* client_ = nullptr;
  mutable std::mutex mu_;
  bool opened_ = false;
  static std::atomic<uint64_t> tokenCounter_;
};

}  // namespace ModbusRTU
