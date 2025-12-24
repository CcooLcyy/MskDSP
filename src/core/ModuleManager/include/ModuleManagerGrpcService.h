#pragma once
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>
#include <grpcpp/support/sync_stream.h>

#include "ModuleManager.grpc.pb.h"
#include "ModuleManager.pb.h"

namespace ModuleManager {
class ServiceImpl : public ModuleManagerProto::Greeter::Service {
  grpc::Status SayHello(grpc::ServerContext *context, const ModuleManagerProto::HelloRequest *request, ModuleManagerProto::HelloReply *reply) override;
  grpc::Status SayHelloStream(grpc::ServerContext *context, const ModuleManagerProto::HelloRequest *request, grpc::ServerWriter<ModuleManagerProto::HelloReply> *writer) override;
  grpc::Status SayHelloClientStream(grpc::ServerContext *context, grpc::ServerReader<ModuleManagerProto::HelloRequest> *reader, ModuleManagerProto::HelloReply *reply) override;
  grpc::Status SayHelloBidiStream(grpc::ServerContext *context, grpc::ServerReaderWriter<ModuleManagerProto::HelloReply, ModuleManagerProto::HelloRequest> *stream) override;
};
}  // namespace ModuleManager