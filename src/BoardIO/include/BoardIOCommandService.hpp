#pragma once

#include <atomic>
#include <cstdint>

#include "DataCenter.grpc.pb.h"

namespace BoardIO {

class BoardOutputManager;

class BoardIOCommandService final
    : public DataCenterProto::CommandExecutor::Service {
public:
  BoardIOCommandService(BoardOutputManager* outputManager,
                        const std::atomic<uint32_t>* outputConnectionId);

  grpc::Status ExecuteCommand(
      grpc::ServerContext* context,
      const DataCenterProto::ExecuteCommandRequest* request,
      DataCenterProto::ExecuteCommandResponse* response) override;

private:
  BoardOutputManager* outputManager_ = nullptr;
  const std::atomic<uint32_t>* outputConnectionId_ = nullptr;
};

}  // namespace BoardIO
