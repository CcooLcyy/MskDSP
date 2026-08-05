#pragma once

#include <grpcpp/impl/service_type.h>
#include <grpcpp/server.h>

#include <memory>
#include <thread>
#include <mutex>
#include <set>
#include <stop_token>
#include <string>
#include <vector>

#include "LibInfoTemp.h"

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
  explicit ModuleInterface();
  virtual ~ModuleInterface() = 0;
  // 由模块管理器控制的停止标志
  virtual void start(std::stop_token stopToken) = 0;
  MetaData metaData();
  void shutdownServers();

protected:
  void initLibInfo(LibInfo libInfo);
  void grpcServerBuilder(std::shared_ptr<grpc::Service> service);
  // 为需要接收大模型等场景的模块设置独立消息上限。
  void grpcServerBuilder(std::shared_ptr<grpc::Service> service,
                         int maxReceiveMessageBytes);
  void grpcServerBuilder(const std::vector<std::shared_ptr<grpc::Service>>& services);
  void grpcServerBuilder(
      const std::vector<std::shared_ptr<grpc::Service>>& services,
      int maxReceiveMessageBytes);
  void releasePort(std::string addr);
  void reservePort(std::string addr);
  MetaData metaData_;
  std::unique_ptr<grpc::Server> server_;
  std::jthread serverThread_;

  // 模块内部停止标志
  std::stop_source stopSource_;
  std::stop_token stopToken_;

private:
  // 对外服务端口从17001开始
  int port_{17001};
  static std::set<std::string> allocatedPorts_;
  static std::mutex portMutex_;
  std::string getRandomPort();
};
}  // namespace ModuleInterface
