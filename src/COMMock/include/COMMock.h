#pragma once

#include <cstddef>
#include <grpcpp/support/status.h>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

#include "ModuleInterface.h"

namespace COMMockProto {
class COMMockConfig;
}

namespace COMMock {
class COMMockGrpcServiceImpl;
class COMMock : public ModuleInterface::ModuleInterface {
public:
  explicit COMMock();
  ~COMMock() override;

  void start(std::stop_token stopToken) override;
  grpc::Status ApplyConfig(const COMMockProto::COMMockConfig &config);

private:
  struct PortConfig {
    std::string name;
    std::string dev_path;
  };

  struct PortHandle {
    PortConfig config;
    int master_fd = -1;
    int slave_fd = -1;
    std::string slave_path;
    bool symlink_created = false;
  };

  bool createPort(const PortConfig &config, size_t index, PortHandle *handle);
  void cleanupPorts(std::vector<PortHandle> *ports);

  std::mutex ports_mutex_;
  std::vector<PortHandle> ports_;
  std::shared_ptr<COMMockGrpcServiceImpl> comMockService_;
};
}  // namespace COMMock
