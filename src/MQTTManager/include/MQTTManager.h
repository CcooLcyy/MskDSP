#pragma once

#include <grpcpp/support/status.h>
#include <grpcpp/support/sync_stream.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <grpcpp/server_context.h>
#include "MQTTManager.pb.h"
#include "ModuleInterface.h"

namespace MQTTManager {
class MQTTManagerGrpcServiceImpl;
class MQTTManager : public ModuleInterface::ModuleInterface {
public:
  explicit MQTTManager();
  ~MQTTManager() override;

  void start(std::stop_token stopToken) override;
  grpc::Status Publish(const MQTTManagerProto::PublishRequest& request,
                       MQTTManagerProto::PublishResponse* response);
  grpc::Status Subscribe(const MQTTManagerProto::SubscribeRequest& request,
                         grpc::ServerWriter<MQTTManagerProto::SubscribeResponse>* writer,
                         grpc::ServerContext* context);
  grpc::Status RequestAndWait(const MQTTManagerProto::RequestAndWaitRequest& request,
                              MQTTManagerProto::RequestAndWaitResponse* response);

private:
  friend class MQTTManagerTestPeer;
  struct PendingResponse;
  struct ConnectionContext;
  struct Subscriber;

  std::shared_ptr<ConnectionContext> getOrCreateConnection(const MQTTManagerProto::ConnectionInfo& info,
                                                           std::string* error);
  std::string makeConnectionKey(const MQTTManagerProto::ConnectionInfo& info) const;
  std::string generateRequestId();
  void handleIncomingMessage(const std::string& connectionKey, const std::string& topic,
                             const std::string& payload);
  void dispatchToSubscribers(const std::string& connectionKey, const std::string& topic, const std::string& payload);

  std::mutex connectionMutex_;
  std::unordered_map<std::string, std::shared_ptr<ConnectionContext>> connections_;

  std::mutex pendingMutex_;
  std::unordered_map<std::string, std::unordered_set<std::shared_ptr<PendingResponse>>> pendingByTopic_;
  std::unordered_set<std::string> pendingMatchKeys_;
  std::atomic<uint64_t> requestCounter_{0};

  std::mutex subscriberMutex_;
  std::unordered_map<std::string, std::unordered_set<std::shared_ptr<Subscriber>>> subscribers_;

  std::shared_ptr<MQTTManagerGrpcServiceImpl> mQTTManagerService_;
};
}  // namespace MQTTManager
