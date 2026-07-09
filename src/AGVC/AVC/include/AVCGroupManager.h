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

#include "AVC.pb.h"
#include "AVCGroupStore.h"
#include "AgvcDataCenterClient.h"
#include "AgvcStrategy.h"

namespace AVC {

class GroupManager {
public:
  explicit GroupManager(std::string moduleName, std::filesystem::path configDbPath = std::filesystem::path("./conf/config.db"));

  void setDataCenterServerAddress(std::string address);
  void setDataCenterStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub);

  grpc::Status LoadPersistedConfig();
  grpc::Status UpsertGroup(const AVCProto::UpsertGroupRequest& request, AVCProto::GroupInfo* out);
  grpc::Status RenameGroup(const std::string& oldGroupName, const std::string& newGroupName, AVCProto::GroupInfo* out);
  grpc::Status GetGroup(const std::string& groupName, AVCProto::GroupInfo* out) const;
  grpc::Status ListGroups(AVCProto::ListGroupsResponse* out) const;
  grpc::Status StartGroup(const std::string& groupName);
  grpc::Status StopGroup(const std::string& groupName);
  grpc::Status DeleteGroup(const std::string& groupName);
  void TryAutoStartReadyGroups(std::string_view trigger);

private:
  struct ControlTrigger {
    std::atomic<bool> pending{false};
    std::counting_semaphore<1024> signal{0};
  };

  struct GroupRuntime {
    AVCProto::GroupConfig config;
    uint32_t connId{0};
    AVCProto::GroupState state{AVCProto::GROUP_STATE_STOPPED};
    std::string lastError;

    std::shared_ptr<grpc::ClientContext> dcSubscribeContext;
    std::jthread dcSubscribeThread;
    std::jthread controlThread;
    std::shared_ptr<ControlTrigger> controlTrigger;

    bool voltageMode{false};
    std::string commandTag;
    std::string voltageMeasTag;
    std::unordered_map<std::string, size_t> memberIndexByQMeasTag;
    std::unordered_set<std::string> baseTags;
    std::vector<std::string> subscribeTags;

    bool hasVoltageMeasRaw{false};
    double voltageMeasRaw{0.0};

    bool hasVoltageCmdRaw{false};
    double voltageCmdRaw{0.0};

    bool hasQTotalCmdRaw{false};
    double qTotalCmdRaw{0.0};

    std::unordered_map<std::string, double> baseRawByTag;
    std::vector<bool> hasMemberQMeasRaw;
    std::vector<double> memberQMeasRaw;

    bool hasLastDesiredTotalQKvar{false};
    double lastDesiredTotalQKvar{0.0};

    std::vector<bool> hasLastMemberTargetQKvar;
    std::vector<double> lastMemberTargetQKvar;

    bool hasLastUnallocatedQKvar{false};
    double lastUnallocatedQKvar{0.0};
  };

  grpc::Status validateGroupName(const std::string& groupName) const;
  grpc::Status validateGroupConfig(const AVCProto::GroupConfig& config) const;
  grpc::Status fillGroupInfoLocked(const GroupRuntime& group, AVCProto::GroupInfo* out) const;
  grpc::Status checkStartPreconditionsLocked(const GroupRuntime& group) const;
  grpc::Status tryAutoStartGroup(const std::string& groupName, std::string_view trigger);
  AVCProto::GroupsConfig dumpGroupsConfigLocked() const;
  grpc::Status saveGroupsLocked();
  grpc::Status restoreGroupFromConfig(const AVCProto::GroupConfig& config, AVCProto::GroupState restoredState);

  void startThreadsLocked(const std::string& groupName, GroupRuntime* group);
  void primeControlInputs(const std::string& groupName);
  void requestControlLocked(const std::string& groupName, GroupRuntime* group, std::string_view reason, std::string_view tag);
  void publishDefaultLimitPoints(const std::string& groupName, std::string_view trigger);
  void publishCommandEchoPoint(
      uint32_t connId,
      const AVCProto::SignalSpec& commandSignal,
      AVCProto::ValueMode commandMode,
      const DataCenterProto::PointUpdate& update);

  bool handleUpdateLocked(GroupRuntime* group, const DataCenterProto::PointUpdate& update);
  void controlTick(const std::string& groupName);

  static bool pointValueToDouble(const DataCenterProto::PointValue& value, double* out);
  static std::unordered_set<std::string> collectAllTags(const AVCProto::GroupConfig& config);
  static void rebuildTagCache(GroupRuntime* group);

  mutable std::mutex mu_;
  std::unordered_map<std::string, GroupRuntime> groupsByName_;
  AVCGroupStore groupStore_;
  AGVC::DataCenterClient dataCenter_;
  AGVC::WeightedStrategy weightedStrategy_;
};

}  // namespace AVC
