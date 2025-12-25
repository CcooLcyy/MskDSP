#pragma once

#include <grpcpp/impl/service_type.h>
#include <grpcpp/server.h>

#include <memory>
#include <stop_token>
#include <string>

#include "LibInfoTemp.h"
#include "moduleManagerLibInfo.h"

namespace ModuleInterface {
struct Version {
  std::string major;
  std::string minor;
  std::string patch;
  std::string version;
};

struct MetaData {
  std::string name;
  Version version;
  std::string libName;
  std::string innerGRPCServer;
  std::string outerGRPCServer;
};

class ModuleInterface {
public:
  explicit ModuleInterface(std::stop_source stopSource);
  virtual ~ModuleInterface();
  virtual void start() = 0;
  void stop();
  MetaData metaData();

  void stopGrpcServer();
  void runInnerServer(std::vector<grpc::Service *> innerServices);
  void runOuterServer(std::vector<grpc::Service *> outerServices);

protected:
  void initLibInfo(LibInfo libInfo);
  void grpcServerBuilder(std::vector<grpc::Service *> services, std::string grpcServer, std::unique_ptr<grpc::Server> &server);
  MetaData metaData_;
  std::stop_token stopToken_;
  std::stop_source stopSource_;
  std::unique_ptr<grpc::Server> innerServer_;
  std::unique_ptr<grpc::Server> outerServer_;

private:
  int getRandomPort();
};
}  // namespace ModuleInterface