#pragma once
#include "ModbusTCP.grpc.pb.h"
#include "ModbusTCP.h"
#include "DataCenter.grpc.pb.h"
namespace ModbusTCP {
class GrpcService final : public ModbusTCPProto::ModbusTCPService::Service {
public:
  void setModule(ModbusTCP* module) { module_ = module; }
  grpc::Status UpsertLink(grpc::ServerContext*, const ModbusTCPProto::UpsertLinkRequest*, ModbusTCPProto::LinkInfo*) override;
  grpc::Status RenameLink(grpc::ServerContext*, const ModbusTCPProto::RenameLinkRequest*, ModbusTCPProto::LinkInfo*) override;
  grpc::Status GetLink(grpc::ServerContext*, const ModbusTCPProto::GetLinkRequest*, ModbusTCPProto::LinkInfo*) override;
  grpc::Status ListLinks(grpc::ServerContext*, const ModbusTCPProto::Empty*, ModbusTCPProto::ListLinksResponse*) override;
  grpc::Status DeleteLink(grpc::ServerContext*, const ModbusTCPProto::DeleteLinkRequest*, ModbusTCPProto::Empty*) override;
  grpc::Status StartLink(grpc::ServerContext*, const ModbusTCPProto::StartLinkRequest*, ModbusTCPProto::Empty*) override;
  grpc::Status StopLink(grpc::ServerContext*, const ModbusTCPProto::StopLinkRequest*, ModbusTCPProto::Empty*) override;
  grpc::Status UpsertPointTable(grpc::ServerContext*, const ModbusTCPProto::UpsertPointTableRequest*, ModbusTCPProto::Empty*) override;
  grpc::Status GetPointTable(grpc::ServerContext*, const ModbusTCPProto::GetPointTableRequest*, ModbusTCPProto::PointTable*) override;
private: ModbusTCP* module_ = nullptr;
};
class CommandService final : public DataCenterProto::CommandExecutor::Service {
public:
  void setModule(ModbusTCP* module) { module_ = module; }
  grpc::Status ExecuteCommand(grpc::ServerContext*, const DataCenterProto::ExecuteCommandRequest*, DataCenterProto::ExecuteCommandResponse*) override;
private: ModbusTCP* module_ = nullptr;
};
}
