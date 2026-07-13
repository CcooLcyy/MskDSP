#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <grpcpp/support/status.h>

#include "ModbusRTU.pb.h"

namespace ModbusRTU {

class PointTable {
public:
  struct Point {
    std::string tag;
    ModbusRTUProto::FunctionCode function = ModbusRTUProto::FUNCTION_UNSPECIFIED;
    uint32_t address = 0;
    ModbusRTUProto::DataType type = ModbusRTUProto::DATA_TYPE_UNSPECIFIED;
    uint32_t regCount = 1;
    ModbusRTUProto::WordOrder wordOrder = ModbusRTUProto::WORD_ORDER_HL;
    ModbusRTUProto::ByteOrder byteOrder = ModbusRTUProto::BYTE_ORDER_AB;
    double scale = 1.0;
    double offset = 0.0;
    double deadband = 0.0;
    std::optional<uint32_t> bitIndex;
  };

  struct RegisterLookup {
    Point point;
    uint32_t wordIndex = 0;
  };

  grpc::Status Upsert(const google::protobuf::RepeatedPtrField<ModbusRTUProto::Point>& points, bool replace);

  std::optional<Point> FindByTag(const std::string& tag) const;
  std::optional<Point> FindByAddress(ModbusRTUProto::FunctionCode function, uint32_t address) const;
  std::optional<RegisterLookup> FindRegisterByAddress(ModbusRTUProto::FunctionCode function, uint32_t address) const;
  std::vector<Point> Points() const;
  std::vector<std::string> Tags() const;
  void ToProto(const std::string& connName, ModbusRTUProto::PointTable* out) const;

private:
  struct PointKey {
    ModbusRTUProto::FunctionCode function = ModbusRTUProto::FUNCTION_UNSPECIFIED;
    uint32_t address = 0;

    bool operator==(const PointKey& other) const {
      return function == other.function && address == other.address;
    }
  };

  struct BitPointKey {
    ModbusRTUProto::FunctionCode function = ModbusRTUProto::FUNCTION_UNSPECIFIED;
    uint32_t address = 0;
    uint32_t bitIndex = 0;

    bool operator==(const BitPointKey& other) const {
      return function == other.function && address == other.address && bitIndex == other.bitIndex;
    }
  };

  struct BitPointKeyHash {
    size_t operator()(const BitPointKey& key) const {
      const auto func = static_cast<uint32_t>(key.function);
      return (static_cast<size_t>(func) << 32) ^ (static_cast<size_t>(key.address) << 8) ^ static_cast<size_t>(key.bitIndex);
    }
  };

  struct PointKeyHash {
    size_t operator()(const PointKey& key) const {
      const auto func = static_cast<uint32_t>(key.function);
      return (static_cast<size_t>(func) << 32) ^ static_cast<size_t>(key.address);
    }
  };

  struct AddressEntry {
    std::string tag;
    uint32_t wordIndex = 0;
  };

  grpc::Status validatePoint(const ModbusRTUProto::Point& point) const;
  grpc::Status insertOrUpdatePoint(const ModbusRTUProto::Point& point);

  std::unordered_map<std::string, Point> byTag_;
  std::unordered_map<PointKey, AddressEntry, PointKeyHash> tagByKey_;
  std::unordered_map<BitPointKey, std::string, BitPointKeyHash> tagByBitKey_;
};

}  // namespace ModbusRTU
