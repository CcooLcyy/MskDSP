#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "ModuleManager.pb.h"

extern "C" bool GetModuleManifestPb(const uint8_t **data, size_t *size);

namespace {

const ModuleManagerProto::ModuleDependency *FindDependency(const ModuleManagerProto::ModuleManifest &manifest,
                                                           const std::string &moduleName) {
  for (const auto &dependency : manifest.dependencies()) {
    if (dependency.module_name() == moduleName) {
      return &dependency;
    }
  }
  return nullptr;
}

}  // namespace

// 验证：ModbusRTU 导出的 manifest 包含 DataCenter 与 MQTTManager 依赖。
TEST(ModbusRtuManifestTest, ExportsExpectedDependencies) {
  const uint8_t *data = nullptr;
  size_t size = 0;
  ASSERT_TRUE(GetModuleManifestPb(&data, &size));
  ASSERT_NE(data, nullptr);
  ASSERT_GT(size, 0u);

  ModuleManagerProto::ModuleManifest manifest;
  ASSERT_TRUE(manifest.ParseFromArray(data, static_cast<int>(size)));
  EXPECT_EQ(manifest.module_name(), "ModbusRTU");

  const auto *dataCenter = FindDependency(manifest, "DataCenter");
  ASSERT_NE(dataCenter, nullptr);
  EXPECT_EQ(dataCenter->version_range(), "=0.0.1");

  const auto *mqttManager = FindDependency(manifest, "MQTTManager");
  ASSERT_NE(mqttManager, nullptr);
  EXPECT_EQ(mqttManager->version_range(), "=0.0.1");
}
