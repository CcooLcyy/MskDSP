#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
    std::optional<uint32_t> byteIndex;
    std::optional<uint32_t> bitIndex;
  };

  struct BlockItem {
    Point point;
    uint32_t offset = 0;
    bool trimRightSpace = true;
  };

  struct Block {
    std::string diText;
    std::array<uint8_t, 4> diBytes{};
    uint32_t dataLen = 0;
    std::vector<BlockItem> items;
  };

  grpc::Status Upsert(const google::protobuf::RepeatedPtrField<DLT645Proto::Point>& points,
                      const google::protobuf::RepeatedPtrField<DLT645Proto::Block>& blocks,
                      bool replace);
  std::optional<Point> FindByTag(const std::string& tag) const;
  std::vector<Point> Points() const;
  const std::vector<Block>& Blocks() const;
  std::vector<std::string> Tags() const;
  const std::unordered_set<std::string>& BlockTags() const;
  void ToProto(const std::string& connName, DLT645Proto::PointTable* out) const;

private:
  static bool isSameDefinition(const Point& lhs, const Point& rhs);
  static grpc::Status validatePoint(const DLT645Proto::Point& point);
  static grpc::Status validateBlockItem(const DLT645Proto::BlockItem& item);
  static grpc::Status validateBlock(const DLT645Proto::Block& block);
  static bool parseHexByte(std::string_view text, uint8_t* out);
  static grpc::Status parseDi(const std::string& di, std::array<uint8_t, 4>* out);
  grpc::Status insertOrUpdatePoint(const DLT645Proto::Point& point);
  grpc::Status insertOrUpdateBlock(const DLT645Proto::Block& block);

  std::unordered_map<std::string, Point> byTag_;
  std::unordered_map<std::string, std::vector<std::string>> tagsByDi_;
  std::vector<Block> blocks_;
  std::unordered_set<std::string> blockDiSet_;
  std::unordered_map<std::string, BlockItem> blockItemByTag_;
  std::unordered_set<std::string> blockTags_;
};

}  // namespace DLT645
