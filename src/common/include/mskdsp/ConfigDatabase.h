#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <grpcpp/support/status.h>

namespace mskdsp {
class ConfigDatabase {
public:
  using TraceFn = std::function<void(const std::string&)>;

  explicit ConfigDatabase(std::filesystem::path dbPath = std::filesystem::path("./conf/config.db"));

  grpc::Status SaveBlob(std::string_view moduleName,
                        std::string_view configKey,
                        std::string_view protoType,
                        std::string_view payload,
                        uint32_t schemaVersion = 1,
                        TraceFn trace = {}) const;

  grpc::Status LoadBlob(std::string_view moduleName,
                        std::string_view configKey,
                        std::string* payload,
                        bool* found,
                        TraceFn trace = {}) const;

  grpc::Status HasAnyBlob(std::string_view moduleName,
                          const std::vector<std::string>& configKeys,
                          bool* found,
                          TraceFn trace = {}) const;

  grpc::Status DeleteBlobs(std::string_view moduleName,
                           const std::vector<std::string>& configKeys,
                           TraceFn trace = {}) const;

  const std::filesystem::path& dbPath() const;

private:
  std::filesystem::path dbPath_;
};

}  // namespace mskdsp
