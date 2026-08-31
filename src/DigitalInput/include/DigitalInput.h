#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <stop_token>

#include <grpcpp/impl/service_type.h>

#include "ModuleInterface.h"
#include "DigitalInputDataCenterClient.hpp"
#include "DigitalInputEventProcessor.hpp"

namespace DigitalInput {
class DigitalInput : public ModuleInterface::ModuleInterface {
public:
  explicit DigitalInput();
  ~DigitalInput() override;

  void start(std::stop_token stopToken) override;

  void SetDataCenterStub(
      std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub);
  void SetDataCenterServerAddress(std::string address);
  void SetGpioChipPath(std::string path);

private:
  struct PendingPublish {
    uint32_t connId = 0;
    PublishedDigitalInput event;
  };

  bool EnqueuePublished(const PublishedDigitalInput& published);
  bool RefreshBoardConnection();
  void PublishLoop(std::stop_token stopToken);
  static void WaitBeforeRetry(std::stop_token stopToken);

  static constexpr std::size_t kMaxPendingPublishes = 256;

  std::shared_ptr<grpc::Service> grpcService_;
  DigitalInputDataCenterClient dataCenter_;
  std::string gpioChipPath_;
  std::mutex publishMutex_;
  std::condition_variable_any publishCondition_;
  std::deque<PendingPublish> pendingPublishes_;
  std::atomic<uint32_t> activeConnectionId_{0};
};
}  // namespace DigitalInput
