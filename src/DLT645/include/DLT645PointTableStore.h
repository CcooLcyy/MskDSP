#pragma once

#include <filesystem>

#include <grpcpp/support/status.h>

#include "DLT645.pb.h"

namespace DLT645 {
class DLT645PointTableStore {
public:
  explicit DLT645PointTableStore(std::filesystem::path pointTablesPath = std::filesystem::path("./conf/DLT645/point_tables.pb"));

  grpc::Status Save(const DLT645Proto::PointTablesConfig &config);
  grpc::Status Load(DLT645Proto::PointTablesConfig *out);

  std::filesystem::path pointTablesPath() const;
  std::filesystem::path backupPath() const;
  std::filesystem::path tmpPath() const;

private:
  std::filesystem::path pointTablesPath_;
};
}  // namespace DLT645
