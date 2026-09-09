#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>

#include <grpcpp/impl/service_type.h>

#include "BoardIOCommandService.hpp"
#include "BoardIODataCenterClient.hpp"
#include "BoardInputEventProcessor.hpp"
#include "BoardOutputManager.hpp"
#include "ModuleInterface.h"

namespace BoardIO {

class BoardIO : public ModuleInterface::ModuleInterface {
public:
  BoardIO();
  ~BoardIO() override;

  void start(std::stop_token stopToken) override;

  void SetDataCenterStub(
      std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub);
  void SetDataCenterServerAddress(std::string address);
  void SetGpioChipPath(std::string path);

private:
  struct PendingPublish {
    uint32_t connId = 0;
    PublishedBoardInput event;
  };

  bool EnqueuePublished(const PublishedBoardInput& published);
  bool RegisterDataCenterEndpoints();
  bool RefreshDataCenterEndpoints();
  void DataCenterRegistrationLoop(std::stop_token stopToken);
  void OutputLoop(std::stop_token stopToken);
  void PublishLoop(std::stop_token stopToken);
  static void WaitBeforeRetry(std::stop_token stopToken);

  static constexpr std::size_t kMaxPendingPublishes = 256;

  std::shared_ptr<grpc::Service> grpcService_;
  BoardIODataCenterClient dataCenter_;
  std::string gpioChipPath_;
  std::mutex registrationMutex_;
  std::mutex publishMutex_;
  std::condition_variable_any publishCondition_;
  std::deque<PendingPublish> pendingPublishes_;
  std::atomic<uint32_t> activeInputConnectionId_{0};
  std::atomic<uint32_t> activeOutputConnectionId_{0};
  BoardOutputManager outputManager_;
  std::shared_ptr<BoardIOCommandService> commandService_;
};

}  // namespace BoardIO
