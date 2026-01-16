#pragma once

#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "AGC.pb.h"
#include "AgvcDataCenterClient.h"
#include "AgvcStrategy.h"

namespace AGC {

class GroupManager {
public:
  explicit GroupManager(std::string moduleName);

  void setDataCenterServerAddress(std::string address);
  void setDataCenterStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub);

  grpc::Status UpsertGroup(const AGCProto::UpsertGroupRequest& request, AGCProto::GroupInfo* out);
  grpc::Status GetGroup(const std::string& groupName, AGCProto::GroupInfo* out) const;
  grpc::Status ListGroups(AGCProto::ListGroupsResponse* out) const;
  grpc::Status StartGroup(const std::string& groupName);
  grpc::Status StopGroup(const std::string& groupName);
  grpc::Status DeleteGroup(const std::string& groupName);

private:
  struct GroupRuntime {
    AGCProto::GroupConfig config;
    uint32_t connId{0};
    AGCProto::GroupState state{AGCProto::GROUP_STATE_STOPPED};
    std::string lastError;

    std::shared_ptr<grpc::ClientContext> dcSubscribeContext;
    std::jthread dcSubscribeThread;
    std::jthread controlThread;

    // Cached tags derived from config (used by the subscribe thread).
    std::string cmdTag;
    std::unordered_map<std::string, size_t> memberIndexByMeasTag;
    std::unordered_set<std::string> baseTags;
    std::vector<std::string> subscribeTags;

    bool hasCmdRaw{false};
    double cmdRaw{0.0};
    std::unordered_map<std::string, double> baseRawByTag;
    std::vector<bool> hasMemberMeasRaw;
    std::vector<double> memberMeasRaw;

    bool hasLastDesiredTotalKw{false};
    double lastDesiredTotalKw{0.0};

    bool hasLastTotalTargetKw{false};
    double lastTotalTargetKw{0.0};

    std::vector<bool> hasLastMemberTargetKw;
    std::vector<double> lastMemberTargetKw;

    bool hasLastUnallocatedKw{false};
    double lastUnallocatedKw{0.0};
  };

  grpc::Status validateGroupName(const std::string& groupName) const;
  grpc::Status validateGroupConfig(const AGCProto::GroupConfig& config) const;
  grpc::Status fillGroupInfoLocked(const GroupRuntime& g, AGCProto::GroupInfo* out) const;

  void stopThreadsLocked(GroupRuntime* g);
  void startThreadsLocked(const std::string& groupName, GroupRuntime* g);

  void handleUpdateLocked(GroupRuntime* g, const DataCenterProto::PointUpdate& update);
  void controlTick(const std::string& groupName);

  static bool pointValueToDouble(const DataCenterProto::PointValue& v, double* out);

  static std::unordered_set<std::string> collectAllTags(const AGCProto::GroupConfig& config);
  static void rebuildTagCache(GroupRuntime* g);

  mutable std::mutex mu_;
  std::unordered_map<std::string, GroupRuntime> groupsByName_;

  AGVC::DataCenterClient dataCenter_;
  AGVC::WeightedStrategy weightedStrategy_;
};

}  // namespace AGC
