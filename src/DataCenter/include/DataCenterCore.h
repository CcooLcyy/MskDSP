#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <grpcpp/support/status.h>

#include "DataCenter.pb.h"

namespace DataCenter {
class DataCenterCore {
public:
  grpc::Status UpsertConnection(const DataCenterProto::UpsertConnectionRequest& request);
  DataCenterProto::ListConnectionsResponse ListConnections() const;

  grpc::Status UpsertPointTable(const DataCenterProto::UpsertPointTableRequest& request);
  grpc::Status GetPointTable(uint32_t connId, DataCenterProto::PointTable* out) const;

  grpc::Status UpsertRoutes(const DataCenterProto::UpsertRoutesRequest& request);
  grpc::Status DeleteRoutes(const DataCenterProto::DeleteRoutesRequest& request);
  DataCenterProto::ListRoutesResponse ListRoutes(const DataCenterProto::ListRoutesRequest& request) const;

  grpc::Status Publish(const DataCenterProto::PublishRequest& request, std::vector<DataCenterProto::PointUpdate>* outUpdates);
  grpc::Status BatchPublish(const DataCenterProto::BatchPublishRequest& request, std::vector<DataCenterProto::PointUpdate>* outUpdates);

  grpc::Status GetLatest(const DataCenterProto::GetLatestRequest& request, DataCenterProto::GetLatestResponse* out) const;

private:
  struct EndpointKey {
    uint32_t connId{};
    std::string tag;

    bool operator==(const EndpointKey& other) const;
  };

  struct EndpointKeyHash {
    size_t operator()(const EndpointKey& key) const noexcept;
  };

  using EndpointKeySet = std::unordered_set<EndpointKey, EndpointKeyHash>;

  static grpc::Status validateEndpoint(uint32_t connId, const std::string& tag);
  grpc::Status validateEndpointAgainstPointTable(uint32_t connId, const std::string& tag) const;

  static int64_t nowMs();

  std::unordered_map<uint32_t, DataCenterProto::ConnectionInfo> connections_;
  std::unordered_map<uint32_t, std::unordered_set<std::string>> pointTables_;
  std::unordered_map<EndpointKey, EndpointKeySet, EndpointKeyHash> routes_;
  std::unordered_map<EndpointKey, DataCenterProto::PointUpdate, EndpointKeyHash> latestByDst_;
};
}  // namespace DataCenter

