#pragma once

#include <filesystem>
#include <string>

#include "PlatformAgent.h"

namespace mskdsp::platformagent {
inline constexpr char kPlatformTarget[] = "60.208.15.90:50000";
struct StoredPlatformAccess {
  Config config;
  uint64_t revision{0};
};
struct PlatformAccessLoad {
  StoredPlatformAccess saved;
  std::string error;
  bool exists{false};
};
// 文件替换后的目录同步失败不能伪装成旧文件未变，调用方必须暂停接入并报告新修订。
enum class PlatformAccessSave { Durable,
                                DurabilityUncertain };
class PlatformAccessStore {
public:
  explicit PlatformAccessStore(std::filesystem::path path);
  PlatformAccessLoad Load() const;
  PlatformAccessSave Save(const StoredPlatformAccess &saved) const;

private:
  std::filesystem::path path_;
};
// 仅返回固定中文错误，不包含身份令牌或原始文件内容。
std::string ValidatePlatformAccess(const Config &config);
}  // namespace mskdsp::platformagent
