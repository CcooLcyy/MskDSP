#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <grpcpp/support/status.h>

namespace IEC104 {

struct SoeRecord {
  uint64_t eventSequence = 0;
  std::string connName;
  uint32_t ioa = 0;
  bool state = false;
  int64_t tsMs = 0;
  uint8_t quality = 0;
  bool acknowledged = false;
};

struct SoeAppendResult {
  SoeRecord record;
  bool overflowed = false;
  size_t overwrittenCount = 0;
  size_t overwrittenUnacknowledgedCount = 0;
  std::optional<uint64_t> oldestOverwrittenSequence;
};

class IEC104SoeStore {
public:
  static constexpr size_t kCapacityPerConnection = 8000;

  explicit IEC104SoeStore(std::filesystem::path databasePath = std::filesystem::path("./conf/config.db"));

  grpc::Status Append(std::string_view connName,
                      uint32_t ioa,
                      bool state,
                      int64_t tsMs,
                      uint8_t quality,
                      SoeAppendResult* result = nullptr);

  grpc::Status LoadUnacknowledged(std::string_view connName, std::vector<SoeRecord>* records) const;
  grpc::Status LoadRecent(std::string_view connName, std::vector<SoeRecord>* records) const;

  grpc::Status MarkAcknowledged(std::string_view connName,
                                std::span<const uint64_t> sequences,
                                size_t* updatedCount = nullptr);

  grpc::Status RenameConnection(std::string_view oldConnName, std::string_view newConnName);
  grpc::Status DeleteConnection(std::string_view connName);

  const std::filesystem::path& databasePath() const;

private:
  std::filesystem::path databasePath_;
};

}  // IEC104 命名空间结束
