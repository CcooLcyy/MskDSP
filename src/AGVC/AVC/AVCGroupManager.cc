#include "AVCGroupManager.h"

#include <grpcpp/client_context.h>

#include <cmath>
#include <format>
#include <utility>

#include "AVCControl.h"
#include "AVCDefaultPoints.h"
#include "AVCGroupValidation.h"
#include "AVCLibInfo.h"
#include "Logger.h"
#include "ThreadUtil.hpp"

namespace AVC {
namespace {

grpc::Status makeNotFound(const std::string& groupName) {
  return grpc::Status(grpc::StatusCode::NOT_FOUND, std::format("未找到控制组: {}", groupName));
}

grpc::Status makeInvalid(std::string message) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::move(message));
}

grpc::Status makePreconditionFailed(std::string message) {
  return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::move(message));
}

const char* groupStateToString(AVCProto::GroupState state) {
  switch (state) {
  case AVCProto::GROUP_STATE_RUNNING:
    return "运行中";
  case AVCProto::GROUP_STATE_PENDING_DELETE:
    return "待删除";
  case AVCProto::GROUP_STATE_STOPPED:
    return "已停止";
  case AVCProto::GROUP_STATE_UNSPECIFIED:
  default:
    return "未指定";
  }
}

constexpr double kValueChangeEps = 1e-6;

bool sameValue(double lhs, double rhs) {
  return std::fabs(lhs - rhs) <= kValueChangeEps;
}

double effectiveScale(const AVCProto::SignalSpec& signal) {
  return signal.scale() == 0.0 ? 1.0 : signal.scale();
}

double commandEchoEngineeringValue(
    const AVCProto::SignalSpec& signal, AVCProto::ValueMode mode, double value) {
  const auto scale = effectiveScale(signal);
  if (mode == AVCProto::VALUE_MODE_DELTA) {
    return value * scale;
  }
  return value * scale + signal.offset();
}

std::string_view defaultPointTag(AVCProto::DefaultPointKind kind) {
  for (const auto& point : DefaultPointDefinitions()) {
    if (point.kind == kind) {
      return point.tag;
    }
  }
  return {};
}

bool hasControllableMember(const AVCProto::GroupConfig& config) {
  for (const auto& member : config.members()) {
    if (member.controllable()) {
      return true;
    }
  }
  return false;
}

}  // namespace

GroupManager::GroupManager(std::string moduleName, std::filesystem::path configDbPath) :
  groupStore_(std::move(configDbPath)),
  dataCenter_(std::move(moduleName)) {}

void GroupManager::setDataCenterServerAddress(std::string address) {
  dataCenter_.setServerAddress(std::move(address));
}

void GroupManager::setDataCenterStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub) {
  dataCenter_.setStub(std::move(stub));
}

grpc::Status GroupManager::validateGroupName(const std::string& groupName) const {
  if (groupName.empty()) {
    return makeInvalid("group_name 不能为空");
  }
  return grpc::Status::OK;
}

grpc::Status GroupManager::validateGroupConfig(const AVCProto::GroupConfig& config) const {
  return AVC::ValidateGroupConfig(config);
}

grpc::Status GroupManager::fillGroupInfoLocked(const GroupRuntime& group, AVCProto::GroupInfo* out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  out->Clear();
  *out->mutable_config() = group.config;
  out->set_conn_id(group.connId);
  out->set_state(group.state);
  out->set_last_error(group.lastError);
  FillDefaultPointInfos(out->mutable_default_points());
  return grpc::Status::OK;
}

grpc::Status GroupManager::checkStartPreconditionsLocked(const GroupRuntime& group) const {
  if (group.state == AVCProto::GROUP_STATE_PENDING_DELETE) {
    return makePreconditionFailed("控制组处于待删除状态");
  }
  auto status = validateGroupConfig(group.config);
  if (!status.ok()) {
    return makePreconditionFailed(std::format("控制组配置未通过当前校验: {}", status.error_message()));
  }
  if (group.connId == 0) {
    return makePreconditionFailed("控制组 conn_id 无效");
  }
  if (!hasControllableMember(group.config)) {
    return makePreconditionFailed("控制组缺少可控成员");
  }
  if (group.subscribeTags.empty()) {
    return makePreconditionFailed("控制组订阅标签为空，当前规则要求控制组配置完整后才启动控制组功能");
  }
  return grpc::Status::OK;
}

grpc::Status GroupManager::tryAutoStartGroup(const std::string& groupName, std::string_view trigger) {
  size_t memberCount = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      return makeNotFound(groupName);
    }
    memberCount = static_cast<size_t>(it->second.config.members_size());
    if (it->second.state == AVCProto::GROUP_STATE_RUNNING) {
      LOG_INFO("AVC 自动启动控制组跳过: group_name={}, 触发来源={}, 原因=控制组已在运行", groupName, trigger);
      return grpc::Status::OK;
    }
    auto status = checkStartPreconditionsLocked(it->second);
    if (!status.ok()) {
      it->second.lastError = status.error_message();
      LOG_INFO("AVC 自动启动控制组跳过: group_name={}, 触发来源={}, 成员数={}, 原因={}",
               groupName,
               trigger,
               memberCount,
               status.error_message());
      return status;
    }
  }

  LOG_INFO("AVC 自动启动控制组: group_name={}, 触发来源={}, 成员数={}", groupName, trigger, memberCount);
  auto status = StartGroup(groupName);
  if (!status.ok()) {
    LOG_WARNING("AVC 自动启动控制组失败: group_name={}, 触发来源={}, 原因={}", groupName, trigger, status.error_message());
  } else {
    LOG_INFO("AVC 自动启动控制组成功: group_name={}, 触发来源={}", groupName, trigger);
  }
  return status;
}

void GroupManager::TryAutoStartReadyGroups(std::string_view trigger) {
  std::vector<std::string> groupNames;
  {
    std::lock_guard<std::mutex> lock(mu_);
    groupNames.reserve(groupsByName_.size());
    for (const auto& [groupName, group] : groupsByName_) {
      if (group.state == AVCProto::GROUP_STATE_STOPPED) {
        groupNames.push_back(groupName);
      }
    }
  }

  if (groupNames.empty()) {
    LOG_INFO("AVC 自动启动检查完成: 触发来源={}, 当前无可评估控制组", trigger);
    return;
  }

  for (const auto& groupName : groupNames) {
    (void)tryAutoStartGroup(groupName, trigger);
  }
}

AVCProto::GroupsConfig GroupManager::dumpGroupsConfigLocked() const {
  AVCProto::GroupsConfig config;
  for (const auto& [_, group] : groupsByName_) {
    auto* persisted = config.add_persisted_groups();
    *persisted->mutable_config() = group.config;
    persisted->set_pending_delete(group.state == AVCProto::GROUP_STATE_PENDING_DELETE);
  }
  return config;
}

grpc::Status GroupManager::saveGroupsLocked() {
  auto config = dumpGroupsConfigLocked();
  auto status = groupStore_.Save(config);
  if (!status.ok()) {
    LOG_ERROR("AVC 控制组配置落盘失败: 原因={}", status.error_message());
  }
  return status;
}

grpc::Status GroupManager::restoreGroupFromConfig(const AVCProto::GroupConfig& config, AVCProto::GroupState restoredState) {
  auto status = validateGroupConfig(config);
  if (!status.ok()) {
    return status;
  }
  LOG_INFO("AVC 开始恢复控制组持久化记录: group_name={}, 状态={}, 成员数={}",
           config.group_name(),
           groupStateToString(restoredState),
           config.members_size());

  DataCenterProto::ConnectionInfo connInfo;
  status = dataCenter_.GetOrCreateConnection(config.group_name(), &connInfo);
  if (!status.ok()) {
    LOG_ERROR("AVC 恢复控制组时获取 DataCenter 连接失败: group_name={}, 成员数={}, 原因={}",
              config.group_name(),
              config.members_size(),
              status.error_message());
    return status;
  }
  if (connInfo.conn_id() == 0) {
    LOG_ERROR("AVC 恢复控制组时 DataCenter 返回无效 conn_id: group_name={}", config.group_name());
    return grpc::Status(grpc::StatusCode::INTERNAL, "DataCenter 返回 conn_id=0");
  }

  GroupRuntime runtime;
  runtime.config = config;
  runtime.connId = connInfo.conn_id();
  runtime.state = restoredState;
  runtime.lastError.clear();
  rebuildTagCache(&runtime);

  const auto tags = collectAllTags(config);
  if (!tags.empty()) {
    std::vector<std::string> tagList;
    tagList.reserve(tags.size());
    for (const auto& tag : tags) {
      tagList.emplace_back(tag);
    }
    auto connTagsStatus = dataCenter_.UpsertConnTags(runtime.connId, tagList, true);
    if (!connTagsStatus.ok()) {
      runtime.lastError = connTagsStatus.error_message();
      LOG_ERROR("AVC 恢复控制组时同步 DataCenter 连接标签注册表失败: group_name={}, conn_id={}, 标签数={}, 原因={}",
                config.group_name(),
                runtime.connId,
                tagList.size(),
                connTagsStatus.error_message());
    } else {
      LOG_INFO("AVC 恢复控制组时已同步 DataCenter 连接标签注册表: group_name={}, conn_id={}, 标签数={}",
               config.group_name(),
               runtime.connId,
               tagList.size());
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      groupsByName_[config.group_name()] = std::move(runtime);
    }
    publishDefaultLimitPoints(config.group_name(), "控制组持久化恢复");
    return connTagsStatus;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    groupsByName_[config.group_name()] = std::move(runtime);
  }
  publishDefaultLimitPoints(config.group_name(), "控制组持久化恢复");
  return grpc::Status::OK;
}

grpc::Status GroupManager::LoadPersistedConfig() {
  AVCProto::GroupsConfig config;
  auto status = groupStore_.Load(&config);
  if (!status.ok()) {
    LOG_ERROR("AVC 控制组配置加载失败: 原因={}", status.error_message());
    return status;
  }
  LOG_INFO("AVC 控制组持久化配置载入摘要: persisted_groups={}, legacy_groups={}",
           config.persisted_groups_size(),
           config.groups_size());
  if (config.groups_size() == 0 && config.persisted_groups_size() == 0) {
    LOG_INFO("AVC 未发现本地控制组配置");
    return grpc::Status::OK;
  }

  size_t restored = 0;
  size_t failed = 0;
  if (config.persisted_groups_size() > 0) {
    for (const auto& persisted : config.persisted_groups()) {
      if (!persisted.has_config()) {
        ++failed;
        LOG_ERROR("AVC 恢复控制组失败: group_name=<空>, 原因=持久化记录缺少 config");
        continue;
      }
      const auto restoredState = persisted.pending_delete() ? AVCProto::GROUP_STATE_PENDING_DELETE
                                                            : AVCProto::GROUP_STATE_STOPPED;
      status = restoreGroupFromConfig(persisted.config(), restoredState);
      if (!status.ok()) {
        ++failed;
        LOG_ERROR("AVC 恢复控制组失败: group_name={}, 原因={}",
                  persisted.config().group_name(),
                  status.error_message());
        continue;
      }
      ++restored;
      LOG_INFO("AVC 已恢复控制组配置: group_name={}, 状态={}",
               persisted.config().group_name(),
               groupStateToString(restoredState));
    }
  } else {
    for (const auto& group : config.groups()) {
      status = restoreGroupFromConfig(group, AVCProto::GROUP_STATE_STOPPED);
      if (!status.ok()) {
        ++failed;
        LOG_ERROR("AVC 恢复控制组失败: group_name={}, 原因={}", group.group_name(), status.error_message());
        continue;
      }
      ++restored;
      LOG_INFO("AVC 已恢复控制组配置: group_name={}, 状态=已停止(兼容旧持久化格式)", group.group_name());
    }
  }

  LOG_INFO("AVC 控制组配置恢复完成: 成功={}, 失败={}", restored, failed);
  TryAutoStartReadyGroups("持久化恢复完成后");
  if (failed > 0) {
    return grpc::Status(grpc::StatusCode::INTERNAL, "部分控制组恢复失败");
  }
  return grpc::Status::OK;
}

grpc::Status GroupManager::UpsertGroup(const AVCProto::UpsertGroupRequest& request, AVCProto::GroupInfo* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  auto status = validateGroupConfig(request.config());
  if (!status.ok()) {
    return status;
  }
  const auto groupName = request.config().group_name();

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it != groupsByName_.end()) {
      if (request.create_only()) {
        return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "group_name 已存在");
      }
      if (it->second.state == AVCProto::GROUP_STATE_RUNNING) {
        return makePreconditionFailed("更新配置前请先停止控制组");
      }
      if (it->second.state == AVCProto::GROUP_STATE_PENDING_DELETE) {
        return makePreconditionFailed("控制组处于待删除状态");
      }

      it->second.config = request.config();
      rebuildTagCache(&it->second);
      it->second.lastError.clear();
      fillGroupInfoLocked(it->second, out);
    } else {
      if (request.create_only()) {
        bool exists = false;
        status = dataCenter_.ConnectionExists(groupName, &exists);
        if (!status.ok()) {
          return status;
        }
        if (exists) {
          return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "group_name 已存在");
        }
      }

      DataCenterProto::ConnectionInfo connInfo;
      status = dataCenter_.GetOrCreateConnection(groupName, &connInfo);
      if (!status.ok()) {
        return status;
      }
      if (connInfo.conn_id() == 0) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "DataCenter 返回 conn_id=0");
      }

      auto [pos, inserted] = groupsByName_.try_emplace(groupName);
      auto& group = pos->second;
      group.config = request.config();
      group.connId = connInfo.conn_id();
      group.state = AVCProto::GROUP_STATE_STOPPED;
      group.lastError.clear();
      rebuildTagCache(&group);
      fillGroupInfoLocked(group, out);
      (void)inserted;
    }

    status = saveGroupsLocked();
    if (!status.ok()) {
      auto saveIt = groupsByName_.find(groupName);
      if (saveIt != groupsByName_.end()) {
        saveIt->second.lastError = status.error_message();
      }
      return status;
    }
  }

  const auto tags = collectAllTags(request.config());
  if (!tags.empty()) {
    uint32_t connId = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = groupsByName_.find(groupName);
      if (it != groupsByName_.end()) {
        connId = it->second.connId;
      }
    }
    if (connId != 0) {
      std::vector<std::string> tagList;
      tagList.reserve(tags.size());
      for (const auto& tag : tags) {
        tagList.emplace_back(tag);
      }
      status = dataCenter_.UpsertConnTags(connId, tagList, true);
      if (!status.ok()) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = groupsByName_.find(groupName);
        if (it != groupsByName_.end()) {
          it->second.lastError = status.error_message();
        }
        return status;
      }
    }
  }
  publishDefaultLimitPoints(groupName, "控制组配置更新成功");
  (void)tryAutoStartGroup(groupName, "控制组配置更新成功");
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      return makeNotFound(groupName);
    }
    status = fillGroupInfoLocked(it->second, out);
    if (!status.ok()) {
      return status;
    }
  }
  return grpc::Status::OK;
}

grpc::Status GroupManager::RenameGroup(const std::string& oldGroupName, const std::string& newGroupName, AVCProto::GroupInfo* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  auto status = validateGroupName(oldGroupName);
  if (!status.ok()) {
    return status;
  }
  status = validateGroupName(newGroupName);
  if (!status.ok()) {
    return status;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(oldGroupName);
    if (it == groupsByName_.end()) {
      return makeNotFound(oldGroupName);
    }
    if (oldGroupName == newGroupName) {
      return fillGroupInfoLocked(it->second, out);
    }
    if (groupsByName_.contains(newGroupName)) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "group_name 已存在");
    }
    if (it->second.state == AVCProto::GROUP_STATE_RUNNING) {
      return makePreconditionFailed("更新配置前请先停止控制组");
    }
    if (it->second.state == AVCProto::GROUP_STATE_PENDING_DELETE) {
      return makePreconditionFailed("控制组处于待删除状态");
    }
  }

  DataCenterProto::ConnectionInfo connInfo;
  status = dataCenter_.RenameConnection(oldGroupName, newGroupName, &connInfo);
  if (!status.ok()) {
    return status;
  }
  if (connInfo.conn_id() == 0) {
    return grpc::Status(grpc::StatusCode::INTERNAL, "DataCenter 返回 conn_id=0");
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto node = groupsByName_.extract(oldGroupName);
    if (node.empty()) {
      return makeNotFound(oldGroupName);
    }
    node.key() = newGroupName;
    node.mapped().config.set_group_name(newGroupName);
    node.mapped().connId = connInfo.conn_id();
    node.mapped().lastError.clear();
    groupsByName_.insert(std::move(node));

    status = saveGroupsLocked();
    if (!status.ok()) {
      auto it = groupsByName_.find(newGroupName);
      if (it != groupsByName_.end()) {
        it->second.lastError = status.error_message();
      }
      return status;
    }

    auto it = groupsByName_.find(newGroupName);
    if (it == groupsByName_.end()) {
      return makeNotFound(newGroupName);
    }
    return fillGroupInfoLocked(it->second, out);
  }
}

grpc::Status GroupManager::GetGroup(const std::string& groupName, AVCProto::GroupInfo* out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  auto status = validateGroupName(groupName);
  if (!status.ok()) {
    return status;
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto it = groupsByName_.find(groupName);
  if (it == groupsByName_.end()) {
    return makeNotFound(groupName);
  }
  return fillGroupInfoLocked(it->second, out);
}

grpc::Status GroupManager::ListGroups(AVCProto::ListGroupsResponse* out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  std::lock_guard<std::mutex> lock(mu_);
  out->Clear();
  for (const auto& [_, group] : groupsByName_) {
    auto* elem = out->add_groups();
    fillGroupInfoLocked(group, elem);
  }
  return grpc::Status::OK;
}

void GroupManager::startThreadsLocked(const std::string& groupName, GroupRuntime* group) {
  if (group == nullptr) {
    return;
  }
  rebuildTagCache(group);

  const auto connId = group->connId;
  auto tags = group->subscribeTags;
  if (connId == 0 || tags.empty()) {
    LOG_WARNING("AVC 控制组启动事件触发控制功能失败: group_name={}, conn_id={}, 原因=订阅标签为空或连接无效", groupName, connId);
    return;
  }

  group->controlTrigger = std::make_shared<ControlTrigger>();
  auto trigger = group->controlTrigger;

  group->controlThread = ModuleManager::StartModuleThread(
      AVCLibInfo.LIB_NAME,
      [this, groupName, trigger](std::stop_token st) {
        std::stop_callback cb(st, [trigger]() { trigger->signal.release(); });

        while (true) {
          trigger->signal.acquire();
          if (st.stop_requested()) {
            break;
          }
          if (!trigger->pending.exchange(false)) {
            continue;
          }
          controlTick(groupName);
        }
      });

  group->dcSubscribeContext = std::make_shared<grpc::ClientContext>();
  auto ctx = group->dcSubscribeContext;

  group->dcSubscribeThread = ModuleManager::StartModuleThread(
      AVCLibInfo.LIB_NAME,
      [this, groupName, ctx, connId, tags](std::stop_token st) {
        std::stop_callback cb(st, [&ctx]() { ctx->TryCancel(); });

        auto reader = dataCenter_.Subscribe(ctx.get(), connId, tags, false);
        if (!reader) {
          LOG_ERROR("AVC 建立 DataCenter 订阅失败: group_name={}, conn_id={}, 标签数={}", groupName, connId, tags.size());
          return;
        }

        DataCenterProto::PointUpdate update;
        while (reader->Read(&update)) {
          bool publishCommandEcho = false;
          uint32_t commandEchoConnId = 0;
          AVCProto::SignalSpec commandEchoSignal;
          auto commandEchoMode = AVCProto::VALUE_MODE_ABSOLUTE;
          {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = groupsByName_.find(groupName);
            if (it == groupsByName_.end()) {
              break;
            }
            if (!it->second.commandTag.empty() && update.dst_tag() == it->second.commandTag) {
              publishCommandEcho = true;
              commandEchoConnId = it->second.connId;
              if (it->second.voltageMode) {
                commandEchoSignal = it->second.config.voltage_cmd();
                commandEchoMode = AVCProto::VALUE_MODE_ABSOLUTE;
              } else {
                commandEchoSignal = it->second.config.q_total_cmd().signal();
                commandEchoMode = it->second.config.q_total_cmd().mode();
              }
            }
            if (handleUpdateLocked(&it->second, update)) {
              requestControlLocked(groupName, &it->second, "订阅输入点更新", update.dst_tag());
            }
          }
          if (publishCommandEcho && commandEchoConnId != 0) {
            publishCommandEchoPoint(commandEchoConnId, commandEchoSignal, commandEchoMode, update);
          }
        }

        auto finishStatus = reader->Finish();
        if (!finishStatus.ok() && !st.stop_requested()) {
          std::lock_guard<std::mutex> lock(mu_);
          auto it = groupsByName_.find(groupName);
          if (it != groupsByName_.end()) {
            it->second.lastError = finishStatus.error_message();
          }
        }
      });
  LOG_INFO("AVC 控制组已启用事件触发控制功能: group_name={}, conn_id={}, 订阅标签数={}", groupName, connId, tags.size());
}

grpc::Status GroupManager::StartGroup(const std::string& groupName) {
  auto status = validateGroupName(groupName);
  if (!status.ok()) {
    return status;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      return makeNotFound(groupName);
    }
    if (it->second.state == AVCProto::GROUP_STATE_RUNNING) {
      LOG_INFO("AVC 启动控制组请求幂等成功: group_name={}, 原因=控制组已在运行", groupName);
      return grpc::Status::OK;
    }
    status = checkStartPreconditionsLocked(it->second);
    if (!status.ok()) {
      it->second.lastError = status.error_message();
      return status;
    }
    startThreadsLocked(groupName, &it->second);
    it->second.state = AVCProto::GROUP_STATE_RUNNING;
    it->second.lastError.clear();
  }
  primeControlInputs(groupName);
  LOG_INFO("AVC 控制组已启动事件触发控制功能: group_name={}", groupName);
  return grpc::Status::OK;
}

grpc::Status GroupManager::StopGroup(const std::string& groupName) {
  auto status = validateGroupName(groupName);
  if (!status.ok()) {
    return status;
  }

  std::jthread dcSubscribeThread;
  std::jthread controlThread;
  std::shared_ptr<ControlTrigger> controlTrigger;
  bool pendingDelete = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      return makeNotFound(groupName);
    }
    dcSubscribeThread = std::move(it->second.dcSubscribeThread);
    controlThread = std::move(it->second.controlThread);
    controlTrigger = std::move(it->second.controlTrigger);
    it->second.dcSubscribeContext.reset();
    pendingDelete = (it->second.state == AVCProto::GROUP_STATE_PENDING_DELETE);
    it->second.state = pendingDelete ? AVCProto::GROUP_STATE_PENDING_DELETE : AVCProto::GROUP_STATE_STOPPED;
  }
  if (dcSubscribeThread.joinable()) {
    dcSubscribeThread.request_stop();
  }
  if (controlThread.joinable()) {
    controlThread.request_stop();
  }
  if (controlTrigger) {
    controlTrigger->signal.release();
  }
  if (dcSubscribeThread.joinable()) {
    dcSubscribeThread.join();
  }
  if (controlThread.joinable()) {
    controlThread.join();
  }
  if (pendingDelete) {
    LOG_INFO("AVC 控制组已停止并保持待删除状态: group_name={}", groupName);
  } else {
    LOG_INFO("AVC 控制组已停止: group_name={}", groupName);
  }
  return grpc::Status::OK;
}

grpc::Status GroupManager::DeleteGroup(const std::string& groupName) {
  auto status = validateGroupName(groupName);
  if (!status.ok()) {
    return status;
  }

  status = StopGroup(groupName);
  if (!status.ok() && status.error_code() != grpc::StatusCode::NOT_FOUND) {
    return status;
  }

  auto dc = dataCenter_.DeleteConnection(groupName);
  if (!dc.ok() && dc.error_code() != grpc::StatusCode::NOT_FOUND) {
    AVCProto::GroupsConfig groupsConfig;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = groupsByName_.find(groupName);
      if (it != groupsByName_.end()) {
        it->second.state = AVCProto::GROUP_STATE_PENDING_DELETE;
        it->second.lastError = dc.error_message();
        groupsConfig = dumpGroupsConfigLocked();
      }
    }
    if (groupsConfig.persisted_groups_size() > 0) {
      auto saveStatus = groupStore_.Save(groupsConfig);
      if (!saveStatus.ok()) {
        LOG_ERROR("AVC 待删除控制组配置落盘失败: group_name={}, 原因={}", groupName, saveStatus.error_message());
        return saveStatus;
      }
    }
    LOG_WARNING("AVC 删除控制组失败，已标记待删除: group_name={}, 原因={}", groupName, dc.error_message());
    return dc;
  }

  std::lock_guard<std::mutex> lock(mu_);
  groupsByName_.erase(groupName);
  status = saveGroupsLocked();
  if (!status.ok()) {
    return status;
  }
  return grpc::Status::OK;
}

grpc::Status GroupManager::ExecuteCommand(
    const DataCenterProto::ExecuteCommandRequest& request,
    DataCenterProto::ExecuteCommandResponse* response) {
  if (response == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "response 为空");
  }
  response->Clear();
  if (!request.has_dst()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "dst 不能为空");
  }
  if (request.value().kind_case() == DataCenterProto::PointValue::KIND_NOT_SET) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "value 不能为空");
  }
  *response->mutable_dst() = request.dst();

  double raw = 0.0;
  if (!pointValueToDouble(request.value(), &raw)) {
    response->set_status(DataCenterProto::COMMAND_REJECTED);
    response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSUPPORTED_POINT);
    response->set_reason("命令点值类型不支持");
    return grpc::Status::OK;
  }

  const auto groupName = request.dst().conn_name();
  AVCProto::GroupConfig config;
  uint32_t connId = 0;
  bool voltageMode = false;
  AVCProto::SignalSpec commandSignal;
  auto commandMode = AVCProto::VALUE_MODE_ABSOLUTE;
  ControlInput input;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      response->set_status(DataCenterProto::COMMAND_REJECTED);
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_BAD_CONFIG);
      response->set_reason("未找到 AVC 控制组");
      return grpc::Status::OK;
    }
    if (it->second.state != AVCProto::GROUP_STATE_RUNNING) {
      response->set_status(DataCenterProto::COMMAND_REJECTED);
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_GROUP_NOT_RUNNING);
      response->set_reason("AVC 控制组未运行");
      return grpc::Status::OK;
    }
    if (it->second.commandTag.empty() || request.dst().tag() != it->second.commandTag) {
      response->set_status(DataCenterProto::COMMAND_REJECTED);
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSUPPORTED_POINT);
      response->set_reason("目的点不是 AVC 命令点");
      return grpc::Status::OK;
    }

    config = it->second.config;
    connId = it->second.connId;
    voltageMode = it->second.voltageMode;
    input.hasVoltageMeasRaw = it->second.hasVoltageMeasRaw;
    input.voltageMeasRaw = it->second.voltageMeasRaw;
    input.hasVoltageCmdRaw = it->second.hasVoltageCmdRaw;
    input.voltageCmdRaw = it->second.voltageCmdRaw;
    input.hasQTotalCmdRaw = it->second.hasQTotalCmdRaw;
    input.qTotalCmdRaw = it->second.qTotalCmdRaw;
    input.baseRawByTag = it->second.baseRawByTag;
    input.hasMemberQMeasRaw = it->second.hasMemberQMeasRaw;
    input.memberQMeasRaw = it->second.memberQMeasRaw;
    input.hasLastMemberTargetQKvar = it->second.hasLastMemberTargetQKvar;
    input.lastMemberTargetQKvar = it->second.lastMemberTargetQKvar;
    input.hasLastDesiredTotalQKvar = it->second.hasLastDesiredTotalQKvar;
    input.lastDesiredTotalQKvar = it->second.lastDesiredTotalQKvar;

    if (voltageMode) {
      input.hasVoltageCmdRaw = true;
      input.voltageCmdRaw = raw;
      commandSignal = config.voltage_cmd();
      commandMode = AVCProto::VALUE_MODE_ABSOLUTE;
    } else {
      input.hasQTotalCmdRaw = true;
      input.qTotalCmdRaw = raw;
      commandSignal = config.q_total_cmd().signal();
      commandMode = config.q_total_cmd().mode();
    }
  }

  const auto defaultOutput = ComputeDefaultPointOutput(config, input);
  response->set_lower_limit(defaultOutput.dynamicLowerQKvar);
  response->set_upper_limit(defaultOutput.dynamicUpperQKvar);

  auto outputOpt = ComputeControlOutput(config, input, weightedStrategy_);
  if (!outputOpt) {
    response->set_status(DataCenterProto::COMMAND_REJECTED);
    response->set_reject_code(voltageMode ? DataCenterProto::COMMAND_REJECT_MISSING_MEASUREMENT
                                          : DataCenterProto::COMMAND_REJECT_BAD_CONFIG);
    response->set_reason(voltageMode ? "AVC 缺少目标电压模式所需量测，无法校验命令"
                                     : "AVC 控制计算无法生成输出");
    return grpc::Status::OK;
  }
  const auto& output = *outputOpt;
  response->set_requested_value(output.rawDesiredTotalQKvar);
  response->set_accepted_value(output.actualTargetQKvar);

  constexpr double kEps = 1e-6;
  if (output.rawDesiredTotalQKvar > defaultOutput.dynamicUpperQKvar + kEps ||
      output.rawDesiredTotalQKvar < defaultOutput.dynamicLowerQKvar - kEps) {
    response->set_status(DataCenterProto::COMMAND_REJECTED);
    if (output.rawDesiredTotalQKvar > defaultOutput.dynamicUpperQKvar + kEps) {
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_OVER_UPPER_LIMIT);
      response->set_reason("总无功目标超过当前可调上限");
    } else {
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_BELOW_LOWER_LIMIT);
      response->set_reason("总无功目标低于当前可调下限");
    }
    LOG_WARNING("AVC 拒绝同步命令: group_name={}, raw_desired_q_kvar={}, lower_q_kvar={}, upper_q_kvar={}, clamped_target_q_kvar={}",
                groupName,
                output.rawDesiredTotalQKvar,
                defaultOutput.dynamicLowerQKvar,
                defaultOutput.dynamicUpperQKvar,
                output.actualTargetQKvar);
    return grpc::Status::OK;
  }

  DataCenterProto::PointUpdate commandUpdate;
  commandUpdate.set_src_conn_id(request.src().conn_id());
  commandUpdate.set_src_tag(request.src().tag());
  commandUpdate.set_dst_conn_id(connId);
  commandUpdate.set_dst_tag(request.dst().tag());
  commandUpdate.mutable_value()->CopyFrom(request.value());
  commandUpdate.set_ts_ms(request.ts_ms());
  commandUpdate.set_quality(request.quality());

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end() || it->second.state != AVCProto::GROUP_STATE_RUNNING) {
      response->set_status(DataCenterProto::COMMAND_REJECTED);
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_GROUP_NOT_RUNNING);
      response->set_reason("AVC 控制组状态已变化，命令未执行");
      return grpc::Status::OK;
    }
    if (voltageMode) {
      it->second.voltageCmdRaw = raw;
      it->second.hasVoltageCmdRaw = true;
    } else {
      it->second.qTotalCmdRaw = raw;
      it->second.hasQTotalCmdRaw = true;
    }
    it->second.hasLastDesiredTotalQKvar = output.hasLastDesiredTotalQKvar;
    it->second.lastDesiredTotalQKvar = output.nextLastDesiredTotalQKvar;
    it->second.hasLastMemberTargetQKvar = output.hasLastMemberTargetQKvar;
    it->second.lastMemberTargetQKvar = output.nextLastMemberTargetQKvar;
    it->second.hasLastUnallocatedQKvar = false;
    it->second.lastUnallocatedQKvar = 0.0;
  }

  publishCommandEchoPoint(connId, commandSignal, commandMode, commandUpdate);
  publishDefaultLimitPoints(groupName, "同步命令执行");

  const auto quality = DataCenterProto::QUALITY_GOOD;
  if (output.hasVoltageMeas) {
    (void)dataCenter_.PublishDouble(connId, std::string(defaultPointTag(AVCProto::DEFAULT_POINT_KIND_CURRENT_VOLTAGE)), output.voltageMeas, quality, 0);
  }
  (void)dataCenter_.PublishDouble(connId, std::string(defaultPointTag(AVCProto::DEFAULT_POINT_KIND_TOTAL_Q_MEAS)), output.totalQMeasKvar, quality, 0);
  (void)dataCenter_.PublishDouble(connId, std::string(defaultPointTag(AVCProto::DEFAULT_POINT_KIND_TOTAL_Q_TARGET)), output.actualTargetQKvar, quality, 0);
  (void)dataCenter_.PublishDouble(connId, std::string(defaultPointTag(AVCProto::DEFAULT_POINT_KIND_TOTAL_Q_ERROR)), output.totalQErrorKvar, quality, 0);
  if (output.hasVoltageError) {
    (void)dataCenter_.PublishDouble(connId, std::string(defaultPointTag(AVCProto::DEFAULT_POINT_KIND_VOLTAGE_ERROR)), output.voltageError, quality, 0);
  }

  for (size_t i = 0; i < output.memberPublish.size() && i < output.memberPublishKvar.size(); ++i) {
    if (!output.memberPublish[i]) {
      continue;
    }
    const auto& member = config.members(static_cast<int>(i));
    if (!member.has_q_set() || !member.q_set().has_signal() || member.q_set().signal().tag().empty()) {
      continue;
    }
    (void)dataCenter_.PublishDouble(connId, member.q_set().signal().tag(), output.memberPublishKvar[i], quality, 0);
  }

  response->set_status(DataCenterProto::COMMAND_ACCEPTED);
  response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSPECIFIED);
  response->set_reason("AVC 同步命令已接受并执行");
  LOG_INFO("AVC 已执行同步命令: group_name={}, raw_desired_q_kvar={}, actual_target_q_kvar={}",
           groupName,
           output.rawDesiredTotalQKvar,
           output.actualTargetQKvar);
  return grpc::Status::OK;
}

bool GroupManager::pointValueToDouble(const DataCenterProto::PointValue& value, double* out) {
  if (out == nullptr) {
    return false;
  }
  switch (value.kind_case()) {
  case DataCenterProto::PointValue::kDoubleValue:
    *out = value.double_value();
    return true;
  case DataCenterProto::PointValue::kIntValue:
    *out = static_cast<double>(value.int_value());
    return true;
  case DataCenterProto::PointValue::kBoolValue:
    *out = value.bool_value() ? 1.0 : 0.0;
    return true;
  default:
    return false;
  }
}

std::unordered_set<std::string> GroupManager::collectAllTags(const AVCProto::GroupConfig& config) {
  std::unordered_set<std::string> tags;
  for (const auto& point : DefaultPointDefinitions()) {
    tags.emplace(point.tag);
  }

  if (config.has_voltage_meas() && !config.voltage_meas().tag().empty()) {
    tags.emplace(config.voltage_meas().tag());
  }

  switch (config.command_case()) {
  case AVCProto::GroupConfig::kVoltageCmd:
    if (!config.voltage_cmd().tag().empty()) {
      tags.emplace(config.voltage_cmd().tag());
    }
    break;
  case AVCProto::GroupConfig::kQTotalCmd:
    if (config.q_total_cmd().has_signal() && !config.q_total_cmd().signal().tag().empty()) {
      tags.emplace(config.q_total_cmd().signal().tag());
    }
    if (config.q_total_cmd().mode() == AVCProto::VALUE_MODE_DELTA &&
        config.q_total_cmd().delta_base() == AVCProto::DELTA_BASE_BASE_TAG &&
        !config.q_total_cmd().base_tag().empty()) {
      tags.emplace(config.q_total_cmd().base_tag());
    }
    break;
  case AVCProto::GroupConfig::COMMAND_NOT_SET:
  default:
    break;
  }

  for (const auto& member : config.members()) {
    if (member.has_q_meas() && !member.q_meas().tag().empty()) {
      tags.emplace(member.q_meas().tag());
    }
    if (member.has_q_set() && member.q_set().has_signal() && !member.q_set().signal().tag().empty()) {
      tags.emplace(member.q_set().signal().tag());
      if (member.q_set().mode() == AVCProto::VALUE_MODE_DELTA &&
          member.q_set().delta_base() == AVCProto::DELTA_BASE_BASE_TAG &&
          !member.q_set().base_tag().empty()) {
        tags.emplace(member.q_set().base_tag());
      }
    }
  }
  return tags;
}

void GroupManager::rebuildTagCache(GroupRuntime* group) {
  if (group == nullptr) {
    return;
  }
  group->voltageMode = (group->config.command_case() == AVCProto::GroupConfig::kVoltageCmd);
  group->commandTag.clear();
  group->voltageMeasTag.clear();
  group->memberIndexByQMeasTag.clear();
  group->baseTags.clear();
  group->subscribeTags.clear();

  group->hasVoltageMeasRaw = false;
  group->voltageMeasRaw = 0.0;
  group->hasVoltageCmdRaw = false;
  group->voltageCmdRaw = 0.0;
  group->hasQTotalCmdRaw = false;
  group->qTotalCmdRaw = 0.0;
  group->baseRawByTag.clear();
  group->hasLastDesiredTotalQKvar = false;
  group->lastDesiredTotalQKvar = 0.0;
  group->hasLastUnallocatedQKvar = false;
  group->lastUnallocatedQKvar = 0.0;

  const auto memberCount = static_cast<size_t>(group->config.members_size());
  group->hasMemberQMeasRaw.assign(memberCount, false);
  group->memberQMeasRaw.assign(memberCount, 0.0);
  group->hasLastMemberTargetQKvar.assign(memberCount, false);
  group->lastMemberTargetQKvar.assign(memberCount, 0.0);

  std::unordered_set<std::string> seenSubscribeTags;
  auto addSubscribeTag = [&seenSubscribeTags, group](const std::string& tag) {
    if (tag.empty()) {
      return;
    }
    if (seenSubscribeTags.emplace(tag).second) {
      group->subscribeTags.emplace_back(tag);
    }
  };

  if (group->config.has_voltage_meas() && !group->config.voltage_meas().tag().empty()) {
    group->voltageMeasTag = group->config.voltage_meas().tag();
    addSubscribeTag(group->voltageMeasTag);
  }

  if (group->voltageMode) {
    if (!group->config.voltage_cmd().tag().empty()) {
      group->commandTag = group->config.voltage_cmd().tag();
      addSubscribeTag(group->commandTag);
    }
  } else if (group->config.command_case() == AVCProto::GroupConfig::kQTotalCmd) {
    if (group->config.q_total_cmd().has_signal() && !group->config.q_total_cmd().signal().tag().empty()) {
      group->commandTag = group->config.q_total_cmd().signal().tag();
      addSubscribeTag(group->commandTag);
    }
    if (group->config.q_total_cmd().mode() == AVCProto::VALUE_MODE_DELTA &&
        group->config.q_total_cmd().delta_base() == AVCProto::DELTA_BASE_BASE_TAG &&
        !group->config.q_total_cmd().base_tag().empty()) {
      group->baseTags.emplace(group->config.q_total_cmd().base_tag());
    }
  }

  for (int i = 0; i < group->config.members_size(); ++i) {
    const auto& member = group->config.members(i);
    if (member.has_q_meas() && !member.q_meas().tag().empty()) {
      group->memberIndexByQMeasTag.emplace(member.q_meas().tag(), static_cast<size_t>(i));
      addSubscribeTag(member.q_meas().tag());
    }
    if (member.has_q_set() &&
        member.q_set().mode() == AVCProto::VALUE_MODE_DELTA &&
        member.q_set().delta_base() == AVCProto::DELTA_BASE_BASE_TAG &&
        !member.q_set().base_tag().empty()) {
      group->baseTags.emplace(member.q_set().base_tag());
    }
  }

  for (const auto& tag : group->baseTags) {
    addSubscribeTag(tag);
  }
}

void GroupManager::primeControlInputs(const std::string& groupName) {
  uint32_t connId = 0;
  std::vector<std::string> tags;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end() || it->second.state != AVCProto::GROUP_STATE_RUNNING) {
      return;
    }
    connId = it->second.connId;
    tags = it->second.subscribeTags;
  }

  if (connId == 0 || tags.empty()) {
    LOG_DEBUG("AVC 启动控制组时跳过初始输入快照加载: group_name={}, conn_id={}, 标签数={}", groupName, connId, tags.size());
    return;
  }

  DataCenterProto::GetLatestResponse resp;
  auto status = dataCenter_.GetLatest(connId, tags, &resp);
  if (!status.ok()) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = groupsByName_.find(groupName);
      if (it != groupsByName_.end()) {
        it->second.lastError = status.error_message();
      }
    }
    LOG_WARNING("AVC 启动控制组时读取初始输入快照失败: group_name={}, conn_id={}, 标签数={}, 原因={}",
                groupName,
                connId,
                tags.size(),
                status.error_message());
    return;
  }

  size_t changed = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end() || it->second.state != AVCProto::GROUP_STATE_RUNNING) {
      return;
    }
    for (const auto& update : resp.updates()) {
      if (handleUpdateLocked(&it->second, update)) {
        ++changed;
      }
    }
    if (resp.updates_size() > 0) {
      requestControlLocked(groupName, &it->second, "启动时加载初始输入快照", "");
    }
  }

  LOG_INFO("AVC 启动控制组时已加载初始输入快照: group_name={}, conn_id={}, updates={}, changed={}",
           groupName,
           connId,
           resp.updates_size(),
           changed);
}

void GroupManager::requestControlLocked(
    const std::string& groupName, GroupRuntime* group, std::string_view reason, std::string_view tag) {
  if (group == nullptr || group->state != AVCProto::GROUP_STATE_RUNNING || !group->controlTrigger) {
    return;
  }
  if (!group->controlTrigger->pending.exchange(true)) {
    group->controlTrigger->signal.release();
    if (tag.empty()) {
      LOG_DEBUG("AVC 控制组已请求一次事件触发控制: group_name={}, 原因={}", groupName, reason);
    } else {
      LOG_DEBUG("AVC 控制组已请求一次事件触发控制: group_name={}, 原因={}, tag={}", groupName, reason, tag);
    }
  }
}

bool GroupManager::handleUpdateLocked(GroupRuntime* group, const DataCenterProto::PointUpdate& update) {
  if (group == nullptr) {
    return false;
  }
  const auto& tag = update.dst_tag();
  double raw = 0.0;
  if (!pointValueToDouble(update.value(), &raw)) {
    return false;
  }

  if (!group->voltageMeasTag.empty() && tag == group->voltageMeasTag) {
    const auto changed = !group->hasVoltageMeasRaw || !sameValue(group->voltageMeasRaw, raw);
    group->voltageMeasRaw = raw;
    group->hasVoltageMeasRaw = true;
    return changed;
  }

  if (!group->commandTag.empty() && tag == group->commandTag) {
    if (group->voltageMode) {
      const auto changed = !group->hasVoltageCmdRaw || !sameValue(group->voltageCmdRaw, raw);
      group->voltageCmdRaw = raw;
      group->hasVoltageCmdRaw = true;
      return changed;
    }
    const auto changed = !group->hasQTotalCmdRaw || !sameValue(group->qTotalCmdRaw, raw);
    group->qTotalCmdRaw = raw;
    group->hasQTotalCmdRaw = true;
    return changed;
  }

  if (group->baseTags.contains(tag)) {
    auto it = group->baseRawByTag.find(tag);
    const auto changed = (it == group->baseRawByTag.end()) || !sameValue(it->second, raw);
    group->baseRawByTag[tag] = raw;
    return changed;
  }

  auto memberIt = group->memberIndexByQMeasTag.find(tag);
  if (memberIt != group->memberIndexByQMeasTag.end()) {
    const auto idx = memberIt->second;
    if (idx < group->memberQMeasRaw.size()) {
      const auto changed = !group->hasMemberQMeasRaw[idx] || !sameValue(group->memberQMeasRaw[idx], raw);
      group->memberQMeasRaw[idx] = raw;
      group->hasMemberQMeasRaw[idx] = true;
      return changed;
    }
  }
  return false;
}

void GroupManager::publishDefaultLimitPoints(const std::string& groupName, std::string_view trigger) {
  AVCProto::GroupConfig config;
  uint32_t connId = 0;
  ControlInput input;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      return;
    }
    config = it->second.config;
    connId = it->second.connId;
    input.hasMemberQMeasRaw = it->second.hasMemberQMeasRaw;
    input.memberQMeasRaw = it->second.memberQMeasRaw;
  }
  if (connId == 0) {
    return;
  }

  const auto defaultOutput = ComputeDefaultPointOutput(config, input);
  const auto theoreticalLowerTag = defaultPointTag(AVCProto::DEFAULT_POINT_KIND_THEORETICAL_LOWER);
  const auto theoreticalUpperTag = defaultPointTag(AVCProto::DEFAULT_POINT_KIND_THEORETICAL_UPPER);
  const auto dynamicLowerTag = defaultPointTag(AVCProto::DEFAULT_POINT_KIND_DYNAMIC_LOWER);
  const auto dynamicUpperTag = defaultPointTag(AVCProto::DEFAULT_POINT_KIND_DYNAMIC_UPPER);
  const auto theoreticalQuality = DataCenterProto::QUALITY_GOOD;

  auto status = dataCenter_.PublishDouble(connId, std::string(theoreticalLowerTag), defaultOutput.theoreticalLowerQKvar, theoreticalQuality, 0);
  if (!status.ok()) {
    LOG_ERROR("AVC 发布默认点失败: group_name={}, tag={}, 触发来源={}, 原因={}", groupName, theoreticalLowerTag, trigger, status.error_message());
  }
  status = dataCenter_.PublishDouble(connId, std::string(theoreticalUpperTag), defaultOutput.theoreticalUpperQKvar, theoreticalQuality, 0);
  if (!status.ok()) {
    LOG_ERROR("AVC 发布默认点失败: group_name={}, tag={}, 触发来源={}, 原因={}", groupName, theoreticalUpperTag, trigger, status.error_message());
  }
  status = dataCenter_.PublishDouble(connId, std::string(dynamicLowerTag), defaultOutput.dynamicLowerQKvar, defaultOutput.dynamicQuality, 0);
  if (!status.ok()) {
    LOG_ERROR("AVC 发布默认点失败: group_name={}, tag={}, 触发来源={}, 原因={}", groupName, dynamicLowerTag, trigger, status.error_message());
  }
  status = dataCenter_.PublishDouble(connId, std::string(dynamicUpperTag), defaultOutput.dynamicUpperQKvar, defaultOutput.dynamicQuality, 0);
  if (!status.ok()) {
    LOG_ERROR("AVC 发布默认点失败: group_name={}, tag={}, 触发来源={}, 原因={}", groupName, dynamicUpperTag, trigger, status.error_message());
  }
  LOG_DEBUG(
      "AVC 已发布默认限值点: group_name={}, 触发来源={}, 理论下限={}, 理论上限={}, 当前下限={}, 当前上限={}, 当前质量={}, 不可控成员数={}, 缺测不可控成员数={}",
      groupName,
      trigger,
      defaultOutput.theoreticalLowerQKvar,
      defaultOutput.theoreticalUpperQKvar,
      defaultOutput.dynamicLowerQKvar,
      defaultOutput.dynamicUpperQKvar,
      static_cast<int>(defaultOutput.dynamicQuality),
      defaultOutput.uncontrollableMemberCount,
      defaultOutput.missingUncontrollableMemberCount);
}

void GroupManager::publishCommandEchoPoint(
    uint32_t connId,
    const AVCProto::SignalSpec& commandSignal,
    AVCProto::ValueMode commandMode,
    const DataCenterProto::PointUpdate& update) {
  const auto commandEchoTag = defaultPointTag(AVCProto::DEFAULT_POINT_KIND_COMMAND_ECHO);
  double value = 0.0;
  if (!pointValueToDouble(update.value(), &value)) {
    LOG_WARNING("AVC 发布调节返回值跳过: conn_id={}, tag={}, 原因=命令点值类型不支持", connId, commandEchoTag);
    return;
  }

  const auto echoValue = commandEchoEngineeringValue(commandSignal, commandMode, value);
  auto status = dataCenter_.PublishDouble(connId, std::string(commandEchoTag), echoValue, update.quality(), update.ts_ms());
  if (!status.ok()) {
    LOG_ERROR("AVC 发布调节返回值失败: conn_id={}, tag={}, 原因={}", connId, commandEchoTag, status.error_message());
  } else {
    LOG_DEBUG("AVC 已发布调节返回值: conn_id={}, tag={}, value={}, echo_value={}, 质量={}, ts_ms={}",
              connId,
              commandEchoTag,
              value,
              echoValue,
              static_cast<int>(update.quality()),
              update.ts_ms());
  }
}

void GroupManager::controlTick(const std::string& groupName) {
  AVCProto::GroupConfig config;
  uint32_t connId = 0;
  ControlInput input;

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      return;
    }
    if (it->second.state != AVCProto::GROUP_STATE_RUNNING) {
      return;
    }

    config = it->second.config;
    connId = it->second.connId;
    input.hasVoltageMeasRaw = it->second.hasVoltageMeasRaw;
    input.voltageMeasRaw = it->second.voltageMeasRaw;
    input.hasVoltageCmdRaw = it->second.hasVoltageCmdRaw;
    input.voltageCmdRaw = it->second.voltageCmdRaw;
    input.hasQTotalCmdRaw = it->second.hasQTotalCmdRaw;
    input.qTotalCmdRaw = it->second.qTotalCmdRaw;
    input.baseRawByTag = it->second.baseRawByTag;
    input.hasMemberQMeasRaw = it->second.hasMemberQMeasRaw;
    input.memberQMeasRaw = it->second.memberQMeasRaw;
    input.hasLastMemberTargetQKvar = it->second.hasLastMemberTargetQKvar;
    input.lastMemberTargetQKvar = it->second.lastMemberTargetQKvar;
    input.hasLastDesiredTotalQKvar = it->second.hasLastDesiredTotalQKvar;
    input.lastDesiredTotalQKvar = it->second.lastDesiredTotalQKvar;
  }

  if (connId == 0) {
    return;
  }

  const auto quality = DataCenterProto::QUALITY_GOOD;
  if (const auto voltageMeas = ComputeVoltageMeas(config, input)) {
    auto status = dataCenter_.PublishDouble(connId, std::string(defaultPointTag(AVCProto::DEFAULT_POINT_KIND_CURRENT_VOLTAGE)), *voltageMeas, quality, 0);
    if (!status.ok()) {
      LOG_ERROR("AVC 发布当前电压失败: group_name={}, conn_id={}, 原因={}", groupName, connId, status.error_message());
    } else {
      LOG_DEBUG("AVC 已发布当前电压: group_name={}, conn_id={}, value={}", groupName, connId, *voltageMeas);
    }
  }

  const auto totalQMeasKvar = ComputeTotalQMeasKvar(config, input);
  {
    auto status = dataCenter_.PublishDouble(connId, std::string(defaultPointTag(AVCProto::DEFAULT_POINT_KIND_TOTAL_Q_MEAS)), totalQMeasKvar, quality, 0);
    if (!status.ok()) {
      LOG_ERROR("AVC 发布总无功实测失败: group_name={}, conn_id={}, value={}, 原因={}",
                groupName,
                connId,
                totalQMeasKvar,
                status.error_message());
    }
  }
  publishDefaultLimitPoints(groupName, "事件触发控制");

  const auto outputOpt = ComputeControlOutput(config, input, weightedStrategy_);
  if (!outputOpt) {
    return;
  }
  const auto& output = *outputOpt;

  auto status = dataCenter_.PublishDouble(connId, std::string(defaultPointTag(AVCProto::DEFAULT_POINT_KIND_TOTAL_Q_TARGET)), output.actualTargetQKvar, quality, 0);
  if (!status.ok()) {
    LOG_ERROR("AVC 发布总无功目标失败: group_name={}, conn_id={}, value={}, 原因={}",
              groupName,
              connId,
              output.actualTargetQKvar,
              status.error_message());
  }

  status = dataCenter_.PublishDouble(connId, std::string(defaultPointTag(AVCProto::DEFAULT_POINT_KIND_TOTAL_Q_ERROR)), output.totalQErrorKvar, quality, 0);
  if (!status.ok()) {
    LOG_ERROR("AVC 发布总无功偏差失败: group_name={}, conn_id={}, value={}, 原因={}",
              groupName,
              connId,
              output.totalQErrorKvar,
              status.error_message());
  }

  if (output.hasVoltageError) {
    status = dataCenter_.PublishDouble(connId, std::string(defaultPointTag(AVCProto::DEFAULT_POINT_KIND_VOLTAGE_ERROR)), output.voltageError, quality, 0);
    if (!status.ok()) {
      LOG_ERROR("AVC 发布电压偏差失败: group_name={}, conn_id={}, value={}, 原因={}",
                groupName,
                connId,
                output.voltageError,
                status.error_message());
    }
  }

  for (size_t i = 0; i < output.memberPublish.size() && i < output.memberPublishKvar.size(); ++i) {
    if (!output.memberPublish[i]) {
      continue;
    }
    const auto& member = config.members(static_cast<int>(i));
    if (!member.has_q_set() || !member.q_set().has_signal() || member.q_set().signal().tag().empty()) {
      continue;
    }
    status = dataCenter_.PublishDouble(connId, member.q_set().signal().tag(), output.memberPublishKvar[i], quality, 0);
    if (!status.ok()) {
      LOG_ERROR("AVC 下发成员无功设定失败: group_name={}, member_name={}, tag={}, publish_kvar={}, 原因={}",
                groupName,
                member.member_name(),
                member.q_set().signal().tag(),
                output.memberPublishKvar[i],
                status.error_message());
    }
  }

  bool shouldLogUnallocated = false;
  const auto unallocatedQKvar = output.unallocatedQKvar;
  const auto targetControllableQKvar = output.targetControllableQKvar;
  const auto passiveQKvar = output.passiveQKvar;
  const auto desiredTotalQKvar = output.desiredTotalQKvar;
  const auto actualTargetQKvar = output.actualTargetQKvar;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      return;
    }

    it->second.hasLastDesiredTotalQKvar = output.hasLastDesiredTotalQKvar;
    it->second.lastDesiredTotalQKvar = output.nextLastDesiredTotalQKvar;
    it->second.hasLastMemberTargetQKvar = output.hasLastMemberTargetQKvar;
    it->second.lastMemberTargetQKvar = output.nextLastMemberTargetQKvar;

    constexpr double kEps = 1e-6;
    if (std::fabs(unallocatedQKvar) > kEps) {
      if (!it->second.hasLastUnallocatedQKvar || std::fabs(unallocatedQKvar - it->second.lastUnallocatedQKvar) > kEps) {
        shouldLogUnallocated = true;
        it->second.hasLastUnallocatedQKvar = true;
        it->second.lastUnallocatedQKvar = unallocatedQKvar;
      }
    } else {
      it->second.hasLastUnallocatedQKvar = false;
      it->second.lastUnallocatedQKvar = 0.0;
    }
  }

  if (shouldLogUnallocated) {
    LOG_WARNING("AVC 分配受限: group_name={}, unallocated_q_kvar={}, target_controllable_q_kvar={}, passive_q_kvar={}, desired_total_q_kvar={}, actual_target_q_kvar={}",
                groupName,
                unallocatedQKvar,
                targetControllableQKvar,
                passiveQKvar,
                desiredTotalQKvar,
                actualTargetQKvar);
  }
}

}  // namespace AVC
