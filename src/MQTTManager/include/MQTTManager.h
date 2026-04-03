#pragma once

#include <grpcpp/support/status.h>
#include <grpcpp/support/sync_stream.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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

struct MQTTConsumedMessage {
  std::string topic;
  std::string payload;
};

struct MQTTClientConnectOptions {
  std::string username;
  std::string password;
  uint32_t keepaliveSec{0};
  bool cleanSession{true};
  uint32_t connectTimeoutMs{0};
};

class IMQTTClient {
public:
  virtual ~IMQTTClient() = default;

  virtual void startConsuming() = 0;
  virtual void stopConsuming() = 0;
  virtual bool isConnected() const = 0;
  virtual void connect(const MQTTClientConnectOptions& options) = 0;
  virtual void disconnect() = 0;
  virtual void publish(const std::string& topic, const std::string& payload, uint32_t qos, bool retain) = 0;
  virtual void subscribe(const std::string& topic, uint32_t qos) = 0;
  virtual std::optional<MQTTConsumedMessage> consumeMessage() = 0;
};

using MQTTClientFactory =
    std::function<std::unique_ptr<IMQTTClient>(const MQTTManagerProto::ConnectionInfo&, const std::string&)>;

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
  MQTTClientFactory clientFactory_;
};
}  // namespace MQTTManager
