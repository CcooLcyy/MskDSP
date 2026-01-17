#pragma once

#include <cstddef>
#include <grpcpp/support/status.h>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <sys/types.h>
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
    std::string peer_name;
    size_t index = 0;
  };

  struct PortPair {
    PortConfig left;
    PortConfig right;
    pid_t pid = -1;
  };

  bool prepareDevPath(const PortConfig &config);
  bool startSocatPair(const PortConfig &left, const PortConfig &right, PortPair *pair);
  void cleanupPairs(std::vector<PortPair> *pairs);
  void cleanupDevPath(const PortConfig &config);
  void stopSocat(const PortPair &pair);

  std::mutex pairs_mutex_;
  std::vector<PortPair> pairs_;
  std::shared_ptr<COMMockGrpcServiceImpl> comMockService_;
};
}  // namespace COMMock
