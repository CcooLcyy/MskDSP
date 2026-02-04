#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>

#include "ModuleManager.grpc.pb.h"
#include "ModuleInterface.h"

namespace ConfigPusher {
class ConfigPusherGrpcServiceImpl;
class ConfigPusher : public ModuleInterface::ModuleInterface {
public:
  explicit ConfigPusher();
  ~ConfigPusher() override;

  void start(std::stop_token stopToken) override;
  void setModuleManagerStub(std::shared_ptr<ModuleManagerProto::ModuleManage::StubInterface> stub);
  void setConfigDirForTest(std::optional<std::filesystem::path> dir);

private:
  friend class ConfigPusherTestPeer;
  void applyConfig();

  std::shared_ptr<ConfigPusherGrpcServiceImpl> configPusherService_;
  std::shared_ptr<ModuleManagerProto::ModuleManage::StubInterface> moduleManagerStub_;
  std::optional<std::filesystem::path> configDirOverride_;
};
}  // namespace ConfigPusher
