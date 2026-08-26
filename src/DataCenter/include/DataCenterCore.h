#pragma once

#include <grpcpp/support/status.h>

#include <cstddef>
#include <cstdint>
#include <deque>
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

  grpc::Status UpsertConnTags(const DataCenterProto::UpsertConnTagsRequest &request);
  grpc::Status GetConnTags(uint32_t connId, DataCenterProto::ConnTags *out) const;
  grpc::Status ReplaceConnTagsConfig(const DataCenterProto::ConnTagsConfig &config);
  DataCenterProto::ConnTagsConfig DumpConnTagsConfig() const;

  grpc::Status UpsertRoutes(const DataCenterProto::UpsertRoutesRequest &request);
  grpc::Status DeleteRoutes(const DataCenterProto::DeleteRoutesRequest &request);
  DataCenterProto::ListRoutesResponse ListRoutes(const DataCenterProto::ListRoutesRequest &request) const;
  grpc::Status ReplaceRoutesConfig(const DataCenterProto::RoutesConfig &config);
  DataCenterProto::RoutesConfig DumpRoutesConfig() const;

  grpc::Status Publish(const DataCenterProto::PublishRequest &request, std::vector<DataCenterProto::PointUpdate> *outUpdates);
  grpc::Status BatchPublish(const DataCenterProto::BatchPublishRequest &request, std::vector<DataCenterProto::PointUpdate> *outUpdates);
  grpc::Status ResolveCommandRoute(const DataCenterProto::ExecuteCommandRequest &request, DataCenterProto::ExecuteCommandResponse *out);
  grpc::Status StoreAcceptedCommand(const DataCenterProto::ExecuteCommandRequest &request, const DataCenterProto::Endpoint &dst);

  grpc::Status GetLatest(const DataCenterProto::GetLatestRequest &request, DataCenterProto::GetLatestResponse *out) const;
  grpc::Status GetSourceLatest(const DataCenterProto::GetSourceLatestRequest &request, DataCenterProto::GetSourceLatestResponse *out) const;
  DataCenterProto::ThroughputSnapshot GetThroughputSnapshot() const;

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

  struct StableEndpointKey {
    std::string moduleName;
    std::string connName;
    std::string tag;

    bool operator==(const StableEndpointKey &other) const;
  };

  struct StableEndpointKeyHash {
    size_t operator()(const StableEndpointKey &key) const noexcept;
  };

  using EndpointKeySet = std::unordered_set<EndpointKey, EndpointKeyHash>;
  using StableEndpointKeySet = std::unordered_set<StableEndpointKey, StableEndpointKeyHash>;

  static grpc::Status validateConnKey(const DataCenterProto::ConnectionKey &key);

  static grpc::Status validateEndpoint(uint32_t connId, const std::string &tag);
  grpc::Status validateEndpointAgainstConnTags(const StableEndpointKey &endpoint) const;
  grpc::Status resolveEndpoint(const DataCenterProto::Endpoint &endpoint, StableEndpointKey *out, uint32_t *resolvedConnId) const;
  bool tryResolveConnId(const StableEndpointKey &endpoint, uint32_t *outConnId) const;
  bool tryResolveConnKey(uint32_t connId, ConnKey *out) const;
  DataCenterProto::Endpoint dumpEndpoint(const StableEndpointKey &endpoint) const;
  void rewriteConnectionKeyReferences(const ConnKey &oldKey, const ConnKey &newKey);
  size_t pruneRoutesRejectedByConnTags(const ConnKey &key, const std::unordered_set<std::string> &allowedTags);

  static int64_t nowMs();

  struct ThroughputBucket {
    int64_t timestampMs{};
    uint64_t routedPoints{};
  };

  void recordRoutedUpdates(size_t count);

  std::unordered_map<uint32_t, DataCenterProto::ConnectionInfo> connections_;
  std::unordered_map<ConnKey, uint32_t, ConnKeyHash> connIdsByKey_;
  uint32_t nextConnId_{1};
  std::unordered_map<ConnKey, std::unordered_set<std::string>, ConnKeyHash> connTagsByKey_;
  std::unordered_map<StableEndpointKey, StableEndpointKeySet, StableEndpointKeyHash> routes_;
  std::unordered_map<EndpointKey, DataCenterProto::PointUpdate, EndpointKeyHash> latestByDst_;
  std::unordered_map<EndpointKey, DataCenterProto::SourcePointUpdate, EndpointKeyHash> latestBySrc_;
  uint64_t sourceUpdateSequence_{0};
  int64_t processStartTimeMs_{nowMs()};
  std::deque<ThroughputBucket> throughputBuckets_;
};
}  // namespace DataCenter
