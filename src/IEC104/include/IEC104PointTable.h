#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <grpcpp/support/status.h>

#include "IEC104.pb.h"

namespace IEC104 {

class PointTable {
public:
  struct Point {
    std::string tag;
    uint32_t ioa = 0;
    IEC104Proto::PointType type = IEC104Proto::POINT_TYPE_UNSPECIFIED;
    IEC104Proto::PointBusinessType businessType = IEC104Proto::POINT_BUSINESS_TYPE_UNSPECIFIED;
    double scale = 1.0;
    double offset = 0.0;
    double deadband = 0.0;
  };

  grpc::Status Upsert(const google::protobuf::RepeatedPtrField<IEC104Proto::Point>& points, bool replace);

  std::optional<Point> FindByTag(const std::string& tag) const;
  std::optional<Point> FindByIoa(uint32_t ioa) const;

  static IEC104Proto::PointBusinessType InferBusinessType(uint32_t ioa,
                                                           IEC104Proto::PointType type);
  static bool IsSimulationBusinessType(IEC104Proto::PointBusinessType businessType);

  std::vector<std::string> Tags() const;
  void ToProto(const std::string& connName, IEC104Proto::PointTable* out) const;

private:
  grpc::Status validatePoint(const IEC104Proto::Point& point) const;
  grpc::Status insertOrUpdatePoint(const IEC104Proto::Point& point);

  std::unordered_map<std::string, Point> byTag_;
  std::unordered_map<uint32_t, std::string> tagByIoa_;
};

}  // namespace IEC104
