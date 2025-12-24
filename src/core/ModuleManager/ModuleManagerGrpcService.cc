#include "ModuleManagerGrpcService.h"

namespace ModuleManager {
grpc::Status ServiceImpl::SayHello(grpc::ServerContext *context, const ModuleManagerProto::HelloRequest *request, ModuleManagerProto::HelloReply *reply) {}
grpc::Status ServiceImpl::SayHelloStream(grpc::ServerContext *context, const ModuleManagerProto::HelloRequest *request, grpc::ServerWriter<ModuleManagerProto::HelloReply> *writer) {}
grpc::Status ServiceImpl::SayHelloClientStream(grpc::ServerContext *context, grpc::ServerReader<ModuleManagerProto::HelloRequest> *reader, ModuleManagerProto::HelloReply *reply) {}
grpc::Status ServiceImpl::SayHelloBidiStream(grpc::ServerContext *context, grpc::ServerReaderWriter<ModuleManagerProto::HelloReply, ModuleManagerProto::HelloRequest> *stream) {}
}  // namespace ModuleManager