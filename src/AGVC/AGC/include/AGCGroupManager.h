#pragma once

#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <semaphore>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "AGC.pb.h"
#include "AGCGroupStore.h"
#include "AgvcDataCenterClient.h"
#include "AgvcStrategy.h"

namespace AGC {

class GroupManager {
public:
  explicit GroupManager(std::string moduleName, std::filesystem::path groupsPath = std::filesystem::path("./conf/AGC/groups.pb"));

  void setDataCenterServerAddress(std::string address);
  void setDataCenterStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub);

  grpc::Status RestorePersistedGroups();
  grpc::Status UpsertGroup(const AGCProto::UpsertGroupRequest& request, AGCProto::GroupInfo* out);
  grpc::Status GetGroup(const std::string& groupName, AGCProto::GroupInfo* out) const;
  grpc::Status ListGroups(AGCProto::ListGroupsResponse* out) const;
  grpc::Status StartGroup(const std::string& groupName);
  grpc::Status StopGroup(const std::string& groupName);
  grpc::Status DeleteGroup(const std::string& groupName);

private:
  struct ControlTrigger {
    std::atomic<bool> pending{false};
    std::counting_semaphore<1024> signal{0};
  };

  struct GroupRuntime {
    AGCProto::GroupConfig config;
    uint32_t connId{0};
    AGCProto::GroupState state{AGCProto::GROUP_STATE_STOPPED};
    std::string lastError;

    std::shared_ptr<grpc::ClientContext> dcSubscribeContext;
    std::jthread dcSubscribeThread;
    std::jthread controlThread;
    std::shared_ptr<ControlTrigger> controlTrigger;

    // 由配置派生的缓存 tags（供订阅线程使用）。
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

    std::vector<bool> hasLastMemberTargetKw;
    std::vector<double> lastMemberTargetKw;

    bool hasLastUnallocatedKw{false};
    double lastUnallocatedKw{0.0};
  };

  grpc::Status validateGroupName(const std::string& groupName) const;
  grpc::Status validateGroupConfig(const AGCProto::GroupConfig& config) const;
  grpc::Status fillGroupInfoLocked(const GroupRuntime& g, AGCProto::GroupInfo* out) const;
  AGCProto::GroupsConfig dumpGroupsConfigLocked() const;
  grpc::Status saveGroupsLocked();
  grpc::Status restoreGroupFromConfig(const AGCProto::GroupConfig& config, AGCProto::GroupState restoredState);

  void startThreadsLocked(const std::string& groupName, GroupRuntime* g);
  void primeControlInputs(const std::string& groupName);
  void requestControlLocked(const std::string& groupName, GroupRuntime* g, std::string_view reason, std::string_view tag);

  bool handleUpdateLocked(GroupRuntime* g, const DataCenterProto::PointUpdate& update);
  void controlTick(const std::string& groupName);

  static bool pointValueToDouble(const DataCenterProto::PointValue& v, double* out);

  static std::unordered_set<std::string> collectAllTags(const AGCProto::GroupConfig& config);
  static void rebuildTagCache(GroupRuntime* g);

  mutable std::mutex mu_;
  std::unordered_map<std::string, GroupRuntime> groupsByName_;
  AGCGroupStore groupStore_;

  AGVC::DataCenterClient dataCenter_;
  AGVC::WeightedStrategy weightedStrategy_;
};

}  // namespace AGC
