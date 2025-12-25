#include "ModuleManagerGrpcService.h"

#include <grpcpp/support/status.h>

#include <chrono>
#include <string>
#include <thread>

#include "ModuleManager.pb.h"

namespace ModuleManager {
grpc::Status ServiceImpl::SayHello(grpc::ServerContext *context, const ModuleManagerProto::HelloRequest *request, ModuleManagerProto::HelloReply *reply) {
  reply->set_message("Hello" + request->name());
  return grpc::Status::OK;
}
grpc::Status ServiceImpl::SayHelloStream(grpc::ServerContext *context, const ModuleManagerProto::HelloRequest *request, grpc::ServerWriter<ModuleManagerProto::HelloReply> *writer) {
  for (int i = 0; i != 10; i++) {
    ModuleManagerProto::HelloReply helloReply;
    helloReply.set_message(std::string("Hello, ") + std::to_string(i));
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (!writer->Write(helloReply)) {
      break;
    }
  }
  return grpc::Status::OK;
}
grpc::Status ServiceImpl::SayHelloClientStream(grpc::ServerContext *context, grpc::ServerReader<ModuleManagerProto::HelloRequest> *reader, ModuleManagerProto::HelloReply *reply) {
  ModuleManagerProto::HelloRequest request;
  int count = 0;
  std::string all_names;
  while (reader->Read(&request)) {
    all_names += request.name() + " ";
    count++;
  }
  reply->set_message("Received " + std::to_string(count) + " names: " + all_names);
  return grpc::Status::OK;
}
grpc::Status ServiceImpl::SayHelloBidiStream(grpc::ServerContext *context, grpc::ServerReaderWriter<ModuleManagerProto::HelloReply, ModuleManagerProto::HelloRequest> *stream) {
  ModuleManagerProto::HelloRequest request;
  while (stream->Read(&request)) {
    ModuleManagerProto::HelloReply reply;
    reply.set_message(request.name());
    stream->Write(reply);
  }
  return grpc::Status::OK;
}
}  // namespace ModuleManager