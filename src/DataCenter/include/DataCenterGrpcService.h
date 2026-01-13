#pragma once

#include "DataCenter.grpc.pb.h"

#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>
#include <grpcpp/support/sync_stream.h>

#include <memory>

namespace DataCenter {
class DataCenterGrpcServiceImpl : public DataCenterProto::DataCenterService::Service {
public:
  DataCenterGrpcServiceImpl();
  ~DataCenterGrpcServiceImpl() override;

  grpc::Status UpsertConnection(grpc::ServerContext* context, const DataCenterProto::UpsertConnectionRequest* request, DataCenterProto::Empty* response) override;
  grpc::Status ListConnections(grpc::ServerContext* context, const DataCenterProto::Empty* request, DataCenterProto::ListConnectionsResponse* response) override;
  grpc::Status GetOrCreateConnection(grpc::ServerContext* context, const DataCenterProto::GetOrCreateConnectionRequest* request, DataCenterProto::ConnectionInfo* response) override;
  grpc::Status RenameConnection(grpc::ServerContext* context, const DataCenterProto::RenameConnectionRequest* request, DataCenterProto::ConnectionInfo* response) override;
  grpc::Status DeleteConnection(grpc::ServerContext* context, const DataCenterProto::DeleteConnectionRequest* request, DataCenterProto::Empty* response) override;

  grpc::Status UpsertPointTable(grpc::ServerContext* context, const DataCenterProto::UpsertPointTableRequest* request, DataCenterProto::Empty* response) override;
  grpc::Status GetPointTable(grpc::ServerContext* context, const DataCenterProto::GetPointTableRequest* request, DataCenterProto::PointTable* response) override;

  grpc::Status UpsertRoutes(grpc::ServerContext* context, const DataCenterProto::UpsertRoutesRequest* request, DataCenterProto::Empty* response) override;
  grpc::Status DeleteRoutes(grpc::ServerContext* context, const DataCenterProto::DeleteRoutesRequest* request, DataCenterProto::Empty* response) override;
  grpc::Status ListRoutes(grpc::ServerContext* context, const DataCenterProto::ListRoutesRequest* request, DataCenterProto::ListRoutesResponse* response) override;

  grpc::Status Publish(grpc::ServerContext* context, const DataCenterProto::PublishRequest* request, DataCenterProto::Empty* response) override;
  grpc::Status BatchPublish(grpc::ServerContext* context, const DataCenterProto::BatchPublishRequest* request, DataCenterProto::Empty* response) override;

  grpc::Status GetLatest(grpc::ServerContext* context, const DataCenterProto::GetLatestRequest* request, DataCenterProto::GetLatestResponse* response) override;
  grpc::Status Subscribe(grpc::ServerContext* context, const DataCenterProto::SubscribeRequest* request, grpc::ServerWriter<DataCenterProto::PointUpdate>* writer) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace DataCenter
