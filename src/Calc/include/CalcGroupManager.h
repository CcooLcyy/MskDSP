#pragma once

#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Calc.pb.h"
#include "CalcDataCenterClient.h"
#include "CalcGroupStore.h"

namespace Calc {

class GroupManager {
public:
  explicit GroupManager(std::string moduleName, std::filesystem::path configDbPath = std::filesystem::path("./conf/config.db"));
  ~GroupManager();

  void setDataCenterServerAddress(std::string address);
  void setDataCenterStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub);

  grpc::Status LoadPersistedConfig();
  grpc::Status UpsertGroup(const CalcProto::UpsertGroupRequest &request, CalcProto::CalcGroupInfo *out);
  grpc::Status RenameGroup(const std::string &oldGroupName, const std::string &newGroupName, CalcProto::CalcGroupInfo *out);
  grpc::Status GetGroup(const std::string &groupName, CalcProto::CalcGroupInfo *out) const;
  grpc::Status ListGroups(CalcProto::ListGroupsResponse *out) const;
  grpc::Status StartGroup(const std::string &groupName);
  grpc::Status StopGroup(const std::string &groupName);
  grpc::Status DeleteGroup(const std::string &groupName);
  void TryAutoStartReadyGroups(std::string_view trigger);
  void Shutdown();

private:
  struct GroupRuntime {
    CalcProto::CalcGroupConfig config;
    uint32_t connId{0};
    CalcProto::GroupState state{CalcProto::GROUP_STATE_STOPPED};
    std::string lastError;

    std::shared_ptr<grpc::ClientContext> dcSubscribeContext;
    std::jthread dcSubscribeThread;
    std::jthread periodicThread;

    std::unordered_set<std::string> subscribeTags;
    std::unordered_map<std::string, DataCenterProto::PointUpdate> latestByTag;
    // 变化触发时，每个计算项本轮最先到达输入的时标。
    std::unordered_map<std::string, int64_t> firstTriggerTsByItem;
    std::unordered_map<std::string, uint64_t> triggerGenerationByItem;
    uint64_t nextTriggerGeneration{0};
    std::unordered_map<std::string, std::string> itemLastErrors;
  };

  grpc::Status validateGroupName(const std::string &groupName) const;
  grpc::Status validateGroupConfig(const CalcProto::CalcGroupConfig &config) const;
  grpc::Status fillGroupInfoLocked(const GroupRuntime &group, CalcProto::CalcGroupInfo *out) const;
  grpc::Status checkStartPreconditionsLocked(const GroupRuntime &group) const;
  grpc::Status tryAutoStartGroup(const std::string &groupName, std::string_view trigger);
  CalcProto::GroupsConfig dumpGroupsConfigLocked() const;
  grpc::Status saveGroupsLocked();
  grpc::Status restoreGroupFromConfig(const CalcProto::PersistedGroup &persisted);

  void startThreadsLocked(const std::string &groupName, GroupRuntime *group);
  void stopThreadsLocked(GroupRuntime *group,
                         bool keepPendingDeleteState,
                         std::jthread *outSubscribeThread,
                         std::jthread *outPeriodicThread);
  void handlePeriodicCalculation(const std::string &groupName);
  void handleUpdate(const std::string &groupName, const DataCenterProto::PointUpdate &update);

  static std::unordered_set<std::string> collectAllTags(const CalcProto::CalcGroupConfig &config);
  static void rebuildTagCache(GroupRuntime *group);

  mutable std::mutex mu_;
  bool shutdown_{false};
  std::unordered_map<std::string, GroupRuntime> groupsByName_;
  GroupStore groupStore_;
  DataCenterClient dataCenter_;
};

}  // namespace Calc
