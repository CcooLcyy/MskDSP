#pragma once

#include <grpcpp/support/status.h>
#include <grpcpp/support/sync_stream.h>

#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <unordered_map>

#include "MQTTManager.pb.h"
#include "ModuleInterface.h"

namespace MQTTManager {
class MQTTManagerGrpcServiceImpl;
class MQTTManager : public ModuleInterface::ModuleInterface {
public:
  explicit MQTTManager();
  ~MQTTManager() override;

  void start(std::stop_token stopToken) override;
  grpc::Status UpdateConfig(const MQTTManagerProto::UpdateConfigRequest& request,
                            MQTTManagerProto::UpdateConfigResponse* response);
  grpc::Status Publish(const MQTTManagerProto::PublishRequest& request,
                       MQTTManagerProto::PublishResponse* response);
  grpc::Status Subscribe(const MQTTManagerProto::SubscribeRequest& request,
                         grpc::ServerWriter<MQTTManagerProto::SubscribeResponse>* writer);
  grpc::Status GetStatus(const MQTTManagerProto::GetStatusRequest& request,
                         MQTTManagerProto::GetStatusResponse* response);

private:
  struct ScriptRuntime {
    std::string source;
    std::string script;
    std::string decodeEntry;
    std::string encodeEntry;
    std::string lastError;
  };

  struct ProfileRuntime {
    MQTTManagerProto::ProfileConfig config;
    ScriptRuntime script;
    bool ready{false};
  };

  std::mutex configMutex_;
  std::unordered_map<std::string, ProfileRuntime> profiles_;
  std::string lastConfigMessage_;
  bool hasConfig_{false};

  std::shared_ptr<MQTTManagerGrpcServiceImpl> mQTTManagerService_;
};
}  // namespace MQTTManager
