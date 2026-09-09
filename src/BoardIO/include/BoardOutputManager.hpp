#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "DataCenter.pb.h"
#include "GpioOutputDriver.hpp"
#include "grpcpp/support/status.h"

namespace BoardIO {

class BoardOutputManager {
public:
  BoardOutputManager();
  explicit BoardOutputManager(std::unique_ptr<GpioOutputDriver> driver);
  ~BoardOutputManager();

  BoardOutputManager(const BoardOutputManager&) = delete;
  BoardOutputManager& operator=(const BoardOutputManager&) = delete;

  bool Start(const std::string& chipPath, std::string* error);
  bool Stop(std::string* error);
  bool Ready() const;

  grpc::Status ExecuteCommand(
      const DataCenterProto::ExecuteCommandRequest& request,
      uint32_t outputConnId,
      DataCenterProto::ExecuteCommandResponse* response,
      std::function<grpc::Status()> cancellationCheck = {});

private:
  static void Reject(DataCenterProto::ExecuteCommandResponse* response,
                     DataCenterProto::CommandRejectCode rejectCode,
                     std::string reason);
  static void Unavailable(DataCenterProto::ExecuteCommandResponse* response,
                          std::string reason);

  mutable std::mutex mutex_;
  std::unique_ptr<GpioOutputDriver> driver_;
  bool ready_ = false;
};

}  // namespace BoardIO
