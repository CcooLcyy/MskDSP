#pragma once

#include <grpcpp/impl/service_type.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>
#include <grpcpp/support/sync_stream.h>

#include "MQTTManager.grpc.pb.h"
#include "MQTTManager.h"
#include "MQTTManager.pb.h"

namespace MQTTManager {
class MQTTManagerGrpcServiceImpl : public MQTTManagerProto::MQTTManagerService::Service {
public:
  void setMQTTManager(MQTTManager* module);
  grpc::Status Publish(grpc::ServerContext* context, const MQTTManagerProto::PublishRequest* request,
                       MQTTManagerProto::PublishResponse* response) override;
  grpc::Status Subscribe(grpc::ServerContext* context, const MQTTManagerProto::SubscribeRequest* request,
                         grpc::ServerWriter<MQTTManagerProto::SubscribeResponse>* writer) override;
  grpc::Status RequestAndWait(grpc::ServerContext* context, const MQTTManagerProto::RequestAndWaitRequest* request,
                              MQTTManagerProto::RequestAndWaitResponse* response) override;

private:
  MQTTManager* module_{nullptr};
};
}  // namespace MQTTManager
