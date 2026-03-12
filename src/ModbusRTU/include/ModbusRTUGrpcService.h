#pragma once

#include <grpcpp/impl/service_type.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include "ModbusRTU.grpc.pb.h"
#include "ModbusRTU.h"
#include "ModbusRTU.pb.h"

namespace ModbusRTU {
class ModbusRTUGrpcServiceImpl : public ModbusRTUProto::ModbusRTUService::Service {
public:
  void setModbusRTU(ModbusRTU* module);
  grpc::Status UpdateConfig(grpc::ServerContext* context,
                            const ModbusRTUProto::UpdateConfigRequest* request,
                            ModbusRTUProto::UpdateConfigResponse* response) override;
  grpc::Status UpsertLink(grpc::ServerContext* context, const ModbusRTUProto::UpsertLinkRequest* request, ModbusRTUProto::LinkInfo* response) override;
  grpc::Status GetLink(grpc::ServerContext* context, const ModbusRTUProto::GetLinkRequest* request, ModbusRTUProto::LinkInfo* response) override;
  grpc::Status ListLinks(grpc::ServerContext* context, const ModbusRTUProto::Empty*, ModbusRTUProto::ListLinksResponse* response) override;
  grpc::Status DeleteLink(grpc::ServerContext* context, const ModbusRTUProto::DeleteLinkRequest* request, ModbusRTUProto::Empty*) override;
  grpc::Status StartLink(grpc::ServerContext* context, const ModbusRTUProto::StartLinkRequest* request, ModbusRTUProto::Empty*) override;
  grpc::Status StopLink(grpc::ServerContext* context, const ModbusRTUProto::StopLinkRequest* request, ModbusRTUProto::Empty*) override;
  grpc::Status UpsertPointTable(grpc::ServerContext* context, const ModbusRTUProto::UpsertPointTableRequest* request, ModbusRTUProto::Empty*) override;
  grpc::Status GetPointTable(grpc::ServerContext* context, const ModbusRTUProto::GetPointTableRequest* request, ModbusRTUProto::PointTable* response) override;

private:
  ModbusRTU* module_;
};
}  // namespace ModbusRTU
