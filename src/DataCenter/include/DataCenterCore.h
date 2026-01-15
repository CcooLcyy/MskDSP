#pragma once

#include <grpcpp/support/status.h>

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "DataCenter.pb.h"

namespace DataCenter {
class DataCenterCore {
public:
  grpc::Status GetOrCreateConnection(const DataCenterProto::GetOrCreateConnectionRequest &request, DataCenterProto::ConnectionInfo *out);
  grpc::Status RenameConnection(const DataCenterProto::RenameConnectionRequest &request, DataCenterProto::ConnectionInfo *out);
  grpc::Status DeleteConnection(const DataCenterProto::DeleteConnectionRequest &request);
  grpc::Status GetConnectionByKey(const DataCenterProto::ConnectionKey &key, DataCenterProto::ConnectionInfo *out) const;
  grpc::Status ReplaceConnectionsConfig(const DataCenterProto::ConnectionsConfig &config);
  DataCenterProto::ConnectionsConfig DumpConnectionsConfig() const;

  grpc::Status UpsertConnection(const DataCenterProto::UpsertConnectionRequest &request);
  DataCenterProto::ListConnectionsResponse ListConnections() const;

  grpc::Status UpsertPointTable(const DataCenterProto::UpsertPointTableRequest &request);
  grpc::Status GetPointTable(uint32_t connId, DataCenterProto::PointTable *out) const;
  grpc::Status ReplacePointTablesConfig(const DataCenterProto::PointTablesConfig &config);
  DataCenterProto::PointTablesConfig DumpPointTablesConfig() const;

  grpc::Status UpsertRoutes(const DataCenterProto::UpsertRoutesRequest &request);
  grpc::Status DeleteRoutes(const DataCenterProto::DeleteRoutesRequest &request);
  DataCenterProto::ListRoutesResponse ListRoutes(const DataCenterProto::ListRoutesRequest &request) const;
  grpc::Status ReplaceRoutesConfig(const DataCenterProto::RoutesConfig &config);
  DataCenterProto::RoutesConfig DumpRoutesConfig() const;

  grpc::Status Publish(const DataCenterProto::PublishRequest &request, std::vector<DataCenterProto::PointUpdate> *outUpdates);
  grpc::Status BatchPublish(const DataCenterProto::BatchPublishRequest &request, std::vector<DataCenterProto::PointUpdate> *outUpdates);

  grpc::Status GetLatest(const DataCenterProto::GetLatestRequest &request, DataCenterProto::GetLatestResponse *out) const;

private:
  struct ConnKey {
    std::string moduleName;
    std::string connName;

    bool operator==(const ConnKey &other) const;
  };

  struct ConnKeyHash {
    size_t operator()(const ConnKey &key) const noexcept;
  };

  struct EndpointKey {
    uint32_t connId{};
    std::string tag;

    bool operator==(const EndpointKey &other) const;
  };

  struct EndpointKeyHash {
    size_t operator()(const EndpointKey &key) const noexcept;
  };

  using EndpointKeySet = std::unordered_set<EndpointKey, EndpointKeyHash>;

  static grpc::Status validateConnKey(const DataCenterProto::ConnectionKey &key);

  static grpc::Status validateEndpoint(uint32_t connId, const std::string &tag);
  grpc::Status validateEndpointAgainstPointTable(uint32_t connId, const std::string &tag) const;

  static int64_t nowMs();

  std::unordered_map<uint32_t, DataCenterProto::ConnectionInfo> connections_;
  std::unordered_map<ConnKey, uint32_t, ConnKeyHash> connIdsByKey_;
  uint32_t nextConnId_{1};
  std::unordered_map<uint32_t, std::unordered_set<std::string>> pointTables_;
  std::unordered_map<EndpointKey, EndpointKeySet, EndpointKeyHash> routes_;
  std::unordered_map<EndpointKey, DataCenterProto::PointUpdate, EndpointKeyHash> latestByDst_;
};
}  // namespace DataCenter
