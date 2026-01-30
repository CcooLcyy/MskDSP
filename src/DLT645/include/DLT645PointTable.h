#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <grpcpp/support/status.h>

#include "DLT645.pb.h"

namespace DLT645 {

class PointTable {
public:
  struct Point {
    std::string tag;
    std::string diText;
    std::array<uint8_t, 4> diBytes{};
    uint32_t dataLen = 0;
    DLT645Proto::DataType type = DLT645Proto::DATA_TYPE_UNSPECIFIED;
    DLT645Proto::AccessMode access = DLT645Proto::ACCESS_UNSPECIFIED;
    double scale = 1.0;
    double offset = 0.0;
    double deadband = 0.0;
  };

  grpc::Status Upsert(const google::protobuf::RepeatedPtrField<DLT645Proto::Point>& points, bool replace);
  std::optional<Point> FindByTag(const std::string& tag) const;
  std::vector<Point> Points() const;
  std::vector<std::string> Tags() const;
  void ToProto(const std::string& connName, DLT645Proto::PointTable* out) const;

private:
  static grpc::Status validatePoint(const DLT645Proto::Point& point);
  static bool parseHexByte(std::string_view text, uint8_t* out);
  static grpc::Status parseDi(const std::string& di, std::array<uint8_t, 4>* out);
  grpc::Status insertOrUpdatePoint(const DLT645Proto::Point& point);

  std::unordered_map<std::string, Point> byTag_;
  std::unordered_map<std::string, std::string> tagByDi_;
};

}  // namespace DLT645
