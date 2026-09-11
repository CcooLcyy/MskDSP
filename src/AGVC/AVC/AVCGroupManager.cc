#include "AVCGroupManager.h"

#include <grpcpp/client_context.h>

#include <algorithm>
#include <cmath>
#include <expected>
#include <format>
#include <utility>

#include "AVCControl.h"
#include "AVCDefaultPoints.h"
#include "AVCGroupValidation.h"
#include "AVCLibInfo.h"
#include "Logger.h"
#include "ThreadUtil.hpp"
#include "mskdsp/Decimal20.hpp"

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

bool sameValue(const Decimal& lhs, const Decimal& rhs) {
  return lhs == rhs;
}

std::expected<Decimal, numeric::DecimalError> engineeringValue(
    const AVCProto::SignalSpec& signal, const Decimal& value) {
  auto scale = numeric::Scale(signal);
  auto offset = numeric::Offset(signal);
  if (!scale.has_value() || !offset.has_value()) {
    return std::unexpected(!scale.has_value() ? scale.error() : offset.error());
  }
  return mskdsp::numeric::ApplyEngineering(value, *scale, *offset);
}

std::expected<Decimal, numeric::DecimalError> commandEchoEngineeringValue(
    const AVCProto::SignalSpec& signal, AVCProto::ValueMode mode,
    const Decimal& value) {
  auto scale = numeric::Scale(signal);
  if (!scale.has_value()) {
    return std::unexpected(scale.error());
  }
  if (mode == AVCProto::VALUE_MODE_DELTA) {
    return value.Multiply(*scale);
  }
  return engineeringValue(signal, value);
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
  out->set_function_enabled(group.functionEnabled);
  out->set_remote_enabled(group.remoteEnabled);
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
  runtime.functionEnabled = true;
  runtime.remoteEnabled = true;
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
    publishControlStatePoints(config.group_name(), "控制组持久化恢复");
    publishDefaultLimitPoints(config.group_name(), "控制组持久化恢复");
    return connTagsStatus;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    groupsByName_[config.group_name()] = std::move(runtime);
  }
  publishControlStatePoints(config.group_name(), "控制组持久化恢复");
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
      group.functionEnabled = true;
      group.remoteEnabled = true;
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
  publishControlStatePoints(groupName, "控制组配置更新成功");
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
    LOG_WARNING("AVC 控制组启动控制功能失败: group_name={}, conn_id={}, 原因=订阅标签为空或连接无效", groupName, connId);
    return;
  }

  group->controlTrigger = std::make_shared<ControlTrigger>();
  auto trigger = group->controlTrigger;

  group->controlThread = ModuleManager::StartModuleThread(
      AVCLibInfo.LIB_NAME,
      [this, groupName, trigger](std::stop_token st) {
        std::stop_callback cb(st, [trigger]() { trigger->signal.release(); });

        bool cyclic = false;
        double calcPeriod = 1.0;
        {
          std::lock_guard<std::mutex> lock(mu_);
          auto it = groupsByName_.find(groupName);
          if (it != groupsByName_.end()) {
            cyclic = it->second.config.control_mode() == AVCProto::CONTROL_MODE_DIRECT_CYCLIC;
            const auto configuredPeriod = it->second.config.calculation_execution_period_seconds();
            if (std::isfinite(configuredPeriod) && configuredPeriod > 0.0) {
              calcPeriod = std::clamp(configuredPeriod, 1.0, 15.0);
            }
          }
        }

        if (!cyclic) {
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
          return;
        }

        // 等待初始快照或首个订阅输入后立即执行第一轮，避免在线程启动时用空输入计算。
        trigger->signal.acquire();
        if (st.stop_requested()) {
          return;
        }
        trigger->pending.store(false);
        controlTick(groupName);

        const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(calcPeriod));
        auto nextTick = std::chrono::steady_clock::now() + period;
        while (!st.stop_requested()) {
          // 即使输入通知持续到达，也不能推迟已经到期的周期计算。
          if (std::chrono::steady_clock::now() >= nextTick) {
            controlTick(groupName);
            nextTick = std::chrono::steady_clock::now() + period;
            continue;
          }
          if (trigger->signal.try_acquire_until(nextTick)) {
            trigger->pending.store(false);
            continue;
          }
          controlTick(groupName);
          nextTick = std::chrono::steady_clock::now() + period;
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
  LOG_INFO("AVC 控制组已启用控制功能: group_name={}, conn_id={}, 订阅标签数={}, 控制方式={}",
           groupName,
           connId,
           tags.size(),
           group->config.control_mode() == AVCProto::CONTROL_MODE_DIRECT_CYCLIC ? "周期直分配" : "PI 事件触发");
}

grpc::Status GroupManager::StartGroup(const std::string& groupName) {
  auto status = validateGroupName(groupName);
  if (!status.ok()) {
    return status;
  }

  bool alreadyRunning = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      return makeNotFound(groupName);
    }
    if (it->second.state == AVCProto::GROUP_STATE_RUNNING) {
      alreadyRunning = true;
    } else {
      status = checkStartPreconditionsLocked(it->second);
      if (!status.ok()) {
        it->second.lastError = status.error_message();
        return status;
      }
      startThreadsLocked(groupName, &it->second);
      it->second.state = AVCProto::GROUP_STATE_RUNNING;
      it->second.lastError.clear();
    }
  }
  if (alreadyRunning) {
    publishControlStatePoints(groupName, "控制组启动幂等请求");
    LOG_INFO("AVC 启动控制组请求幂等成功: group_name={}, 原因=控制组已在运行", groupName);
    return grpc::Status::OK;
  }
  publishControlStatePoints(groupName, "控制组启动");
  primeControlInputs(groupName);
  LOG_INFO("AVC 控制组已启动控制功能: group_name={}", groupName);
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
    it->second.hasLastDesiredTotalQKvar = false;
    it->second.lastDesiredTotalQKvar = Decimal{};
    it->second.commandRevision = 0;
    it->second.directResolvedCommandRevision = 0;
    it->second.hasDirectResolvedDesiredTotalQKvar = false;
    it->second.directResolvedDesiredTotalQKvar = Decimal{};
    std::fill(it->second.hasLastMemberTargetQKvar.begin(), it->second.hasLastMemberTargetQKvar.end(), false);
    std::fill(it->second.lastMemberTargetQKvar.begin(), it->second.lastMemberTargetQKvar.end(), Decimal{});
    it->second.hasLastCommandPublishedAt = false;
    it->second.lastCommandPublishedAt = std::chrono::steady_clock::time_point{};
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

  const auto groupName = request.dst().conn_name();
  const auto functionTag = defaultPointTag(AVCProto::DEFAULT_POINT_KIND_FUNCTION_ENABLE);
  const auto remoteTag = defaultPointTag(AVCProto::DEFAULT_POINT_KIND_REMOTE_OPERATION);
  const bool isFunctionPoint = request.dst().tag() == functionTag;
  const bool isRemotePoint = request.dst().tag() == remoteTag;
  if (isFunctionPoint || isRemotePoint) {
    bool publishState = false;
    bool requestControl = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = groupsByName_.find(groupName);
      if (it == groupsByName_.end()) {
        response->set_status(DataCenterProto::COMMAND_REJECTED);
        response->set_reject_code(DataCenterProto::COMMAND_REJECT_BAD_CONFIG);
        response->set_reason("未找到 AVC 控制组");
        return grpc::Status::OK;
      }
      if (it->second.state == AVCProto::GROUP_STATE_PENDING_DELETE) {
        response->set_status(DataCenterProto::COMMAND_REJECTED);
        response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSPECIFIED);
        response->set_reason("AVC 控制组处于待删除状态");
        LOG_WARNING("AVC 拒绝控制状态命令: group_name={}, tag={}, 原因=控制组处于待删除状态", groupName, request.dst().tag());
        return grpc::Status::OK;
      }
      if (!request.value().has_bool_value()) {
        response->set_status(DataCenterProto::COMMAND_REJECTED);
        response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSUPPORTED_POINT);
        response->set_reason("AVC 控制点仅接受 BOOL 类型");
        LOG_WARNING("AVC 拒绝控制状态命令: group_name={}, tag={}, 原因=控制点仅接受 BOOL 类型", groupName, request.dst().tag());
        return grpc::Status::OK;
      }
      const bool value = request.value().bool_value();
      // 远方操作点本身用于切换远方权限，因此即使当前为就地也允许修改它。
      if (isFunctionPoint && !it->second.remoteEnabled) {
        response->set_status(DataCenterProto::COMMAND_REJECTED);
        response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSPECIFIED);
        response->set_reason("AVC 当前不允许远方操作");
        LOG_WARNING("AVC 拒绝功能投入命令: group_name={}, value={}, 原因=AVC 当前不允许远方操作", groupName, value);
        return grpc::Status::OK;
      }
      if (isFunctionPoint) {
        if (it->second.functionEnabled != value) {
          it->second.functionEnabled = value;
          publishState = true;
          requestControl = value && it->second.state == AVCProto::GROUP_STATE_RUNNING;
        }
      } else if (it->second.remoteEnabled != value) {
        it->second.remoteEnabled = value;
        publishState = true;
      }
      response->set_status(DataCenterProto::COMMAND_ACCEPTED);
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSPECIFIED);
      response->set_requested_value(value ? 1.0 : 0.0);
      response->set_accepted_value(value ? 1.0 : 0.0);
      response->set_reason(isFunctionPoint ? "AVC 功能投入状态已更新" : "AVC 远方操作状态已更新");
      LOG_INFO("AVC 已接受控制状态命令: group_name={}, tag={}, value={}, 状态已变化={}", groupName, request.dst().tag(), value, publishState);
    }
    if (publishState) {
      publishControlStatePoints(groupName, isFunctionPoint ? "同步功能投入命令" : "同步远方操作命令");
    }
    if (requestControl) {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = groupsByName_.find(groupName);
      if (it != groupsByName_.end()) {
        requestControlLocked(groupName, &it->second, "功能重新投入", functionTag);
      }
    }
    return grpc::Status::OK;
  }

  Decimal raw;
  if (!pointValueToDecimal(request.value(), &raw)) {
    response->set_status(DataCenterProto::COMMAND_REJECTED);
    response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSUPPORTED_POINT);
    response->set_reason("命令点值类型不支持");
    return grpc::Status::OK;
  }

  AVCProto::GroupConfig config;
  uint32_t connId = 0;
  bool voltageMode = false;
  AVCProto::SignalSpec commandSignal;
  auto commandMode = AVCProto::VALUE_MODE_ABSOLUTE;
  bool directCyclic = false;
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
    if (!it->second.remoteEnabled) {
      response->set_status(DataCenterProto::COMMAND_REJECTED);
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSPECIFIED);
      response->set_reason("AVC 当前不允许远方操作");
      return grpc::Status::OK;
    }
    if (!it->second.functionEnabled) {
      response->set_status(DataCenterProto::COMMAND_REJECTED);
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSPECIFIED);
      response->set_reason("AVC 功能未投入");
      return grpc::Status::OK;
    }
    if (it->second.commandTag.empty() || request.dst().tag() != it->second.commandTag) {
      response->set_status(DataCenterProto::COMMAND_REJECTED);
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSUPPORTED_POINT);
      response->set_reason("目的点不是 AVC 命令点");
      return grpc::Status::OK;
    }

    config = it->second.config;
    directCyclic = config.control_mode() == AVCProto::CONTROL_MODE_DIRECT_CYCLIC;
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
  response->set_lower_limit_decimal(
      defaultOutput.dynamicLowerQKvar.ToFixedString());
  response->set_upper_limit_decimal(
      defaultOutput.dynamicUpperQKvar.ToFixedString());
  response->set_lower_limit(
      numeric::ToLegacyDouble(defaultOutput.dynamicLowerQKvar));
  response->set_upper_limit(
      numeric::ToLegacyDouble(defaultOutput.dynamicUpperQKvar));

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

  response->set_requested_value_decimal(
      output.rawDesiredTotalQKvar.ToFixedString());
  response->set_accepted_value_decimal(
      output.actualTargetQKvar.ToFixedString());
  response->set_requested_value(
      numeric::ToLegacyDouble(output.rawDesiredTotalQKvar));
  response->set_accepted_value(
      numeric::ToLegacyDouble(output.actualTargetQKvar));

  if (output.rawDesiredTotalQKvar > defaultOutput.dynamicUpperQKvar ||
      output.rawDesiredTotalQKvar < defaultOutput.dynamicLowerQKvar) {
    response->set_status(DataCenterProto::COMMAND_REJECTED);
    if (output.rawDesiredTotalQKvar > defaultOutput.dynamicUpperQKvar) {
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_OVER_UPPER_LIMIT);
      response->set_reason("总无功目标超过当前可调上限");
    } else {
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_BELOW_LOWER_LIMIT);
      response->set_reason("总无功目标低于当前可调下限");
    }
    LOG_WARNING("AVC 拒绝同步命令: group_name={}, raw_desired_q_kvar={}, lower_q_kvar={}, upper_q_kvar={}, clamped_target_q_kvar={}",
                groupName,
                output.rawDesiredTotalQKvar.ToFixedString(),
                defaultOutput.dynamicLowerQKvar.ToFixedString(),
                defaultOutput.dynamicUpperQKvar.ToFixedString(),
                output.actualTargetQKvar.ToFixedString());
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

  if (directCyclic) {
    // 已完成与 PI_EVENT 相同的缺测、限值校验；周期线程只保留最新命令并负责实际下发。
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = groupsByName_.find(groupName);
      if (it == groupsByName_.end() || it->second.state != AVCProto::GROUP_STATE_RUNNING) {
        response->set_status(DataCenterProto::COMMAND_REJECTED);
        response->set_reject_code(DataCenterProto::COMMAND_REJECT_GROUP_NOT_RUNNING);
        response->set_reason("AVC 控制组状态已变化，命令未排队");
        return grpc::Status::OK;
      }
      if (voltageMode) {
        it->second.voltageCmdRaw = raw;
        it->second.hasVoltageCmdRaw = true;
      } else {
        it->second.qTotalCmdRaw = raw;
        it->second.hasQTotalCmdRaw = true;
      }
      ++it->second.commandRevision;
      it->second.hasDirectResolvedDesiredTotalQKvar = false;
      requestControlLocked(groupName, &it->second, "周期模式同步命令", request.dst().tag());
    }
    publishCommandEchoPoint(connId, commandSignal, commandMode, commandUpdate);
    response->set_status(DataCenterProto::COMMAND_ACCEPTED);
    response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSPECIFIED);
    response->set_reason("AVC 周期模式命令已接受，等待命令控制周期执行");
    return grpc::Status::OK;
  }

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
    it->second.lastUnallocatedQKvar = Decimal{};
  }

  publishCommandEchoPoint(connId, commandSignal, commandMode, commandUpdate);
  publishDefaultLimitPoints(groupName, "同步命令执行");

  const auto quality = DataCenterProto::QUALITY_GOOD;
  if (output.hasVoltageMeas) {
    (void)dataCenter_.PublishDecimal(
        connId,
        std::string(defaultPointTag(
            AVCProto::DEFAULT_POINT_KIND_CURRENT_VOLTAGE)),
        output.voltageMeas.ToFixedString(), quality, 0);
  }
  (void)dataCenter_.PublishDecimal(
      connId,
      std::string(defaultPointTag(
          AVCProto::DEFAULT_POINT_KIND_TOTAL_Q_MEAS)),
      output.totalQMeasKvar.ToFixedString(), quality, 0);
  (void)dataCenter_.PublishDecimal(
      connId,
      std::string(defaultPointTag(
          AVCProto::DEFAULT_POINT_KIND_TOTAL_Q_TARGET)),
      output.actualTargetQKvar.ToFixedString(), quality, 0);
  (void)dataCenter_.PublishDecimal(
      connId,
      std::string(defaultPointTag(
          AVCProto::DEFAULT_POINT_KIND_TOTAL_Q_ERROR)),
      output.totalQErrorKvar.ToFixedString(), quality, 0);
  if (output.hasVoltageError) {
    (void)dataCenter_.PublishDecimal(
        connId,
        std::string(defaultPointTag(
            AVCProto::DEFAULT_POINT_KIND_VOLTAGE_ERROR)),
        output.voltageError.ToFixedString(), quality, 0);
  }

  for (size_t i = 0; i < output.memberPublish.size() && i < output.memberPublishKvar.size(); ++i) {
    if (!output.memberPublish[i]) {
      continue;
    }
    const auto& member = config.members(static_cast<int>(i));
    if (!member.has_q_set() || !member.q_set().has_signal() || member.q_set().signal().tag().empty()) {
      continue;
    }
    (void)dataCenter_.PublishDecimal(
        connId, member.q_set().signal().tag(),
        output.memberPublishKvar[i].ToFixedString(), quality, 0);
  }

  response->set_status(DataCenterProto::COMMAND_ACCEPTED);
  response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSPECIFIED);
  response->set_reason("AVC 同步命令已接受并执行");
  LOG_INFO("AVC 已执行同步命令: group_name={}, raw_desired_q_kvar={}, actual_target_q_kvar={}",
           groupName,
           output.rawDesiredTotalQKvar.ToFixedString(),
           output.actualTargetQKvar.ToFixedString());
  return grpc::Status::OK;
}

bool GroupManager::pointValueToDecimal(
    const DataCenterProto::PointValue& value, Decimal* out) {
  if (out == nullptr) {
    return false;
  }
  switch (value.kind_case()) {
  case DataCenterProto::PointValue::kDoubleValue: {
    auto decimal = Decimal::FromDouble(value.double_value());
    if (!decimal.has_value()) {
      LOG_WARNING("AVC 拒绝非有限 double 点值");
      return false;
    }
    *out = *decimal;
    return true;
  }
  case DataCenterProto::PointValue::kIntValue: {
    auto decimal = Decimal::FromInt64(value.int_value());
    if (!decimal.has_value()) {
      LOG_WARNING("AVC 整数点值超出 Decimal20 范围");
      return false;
    }
    *out = *decimal;
    return true;
  }
  case DataCenterProto::PointValue::kBoolValue:
    *out = value.bool_value() ? numeric::One() : numeric::Zero();
    return true;
  case DataCenterProto::PointValue::kDecimalValue: {
    auto decimal =
        mskdsp::numeric::Decimal20::Parse(value.decimal_value());
    if (!decimal.has_value()) {
      LOG_WARNING("AVC decimal_value 解析失败: {}",
                  mskdsp::numeric::DecimalErrorMessage(decimal.error()));
      return false;
    }
    *out = *decimal;
    return true;
  }
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
  group->voltageMeasRaw = Decimal{};
  group->hasVoltageCmdRaw = false;
  group->voltageCmdRaw = Decimal{};
  group->hasQTotalCmdRaw = false;
  group->qTotalCmdRaw = Decimal{};
  group->commandRevision = 0;
  group->directResolvedCommandRevision = 0;
  group->hasDirectResolvedDesiredTotalQKvar = false;
  group->directResolvedDesiredTotalQKvar = Decimal{};
  group->baseRawByTag.clear();
  group->hasLastDesiredTotalQKvar = false;
  group->lastDesiredTotalQKvar = Decimal{};
  group->hasLastUnallocatedQKvar = false;
  group->lastUnallocatedQKvar = Decimal{};
  group->hasLastCommandPublishedAt = false;
  group->lastCommandPublishedAt = std::chrono::steady_clock::time_point{};

  const auto memberCount = static_cast<size_t>(group->config.members_size());
  group->hasMemberQMeasRaw.assign(memberCount, false);
  group->memberQMeasRaw.assign(memberCount, Decimal{});
  group->hasLastMemberTargetQKvar.assign(memberCount, false);
  group->lastMemberTargetQKvar.assign(memberCount, Decimal{});

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

void GroupManager::publishControlStatePoints(const std::string& groupName, std::string_view trigger) {
  uint32_t connId = 0;
  bool functionEnabled = true;
  bool remoteEnabled = true;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      return;
    }
    connId = it->second.connId;
    functionEnabled = it->second.functionEnabled;
    remoteEnabled = it->second.remoteEnabled;
  }
  if (connId == 0) {
    return;
  }

  const auto functionTag = defaultPointTag(AVCProto::DEFAULT_POINT_KIND_FUNCTION_ENABLE);
  const auto remoteTag = defaultPointTag(AVCProto::DEFAULT_POINT_KIND_REMOTE_OPERATION);
  DataCenterProto::PointValue functionValue;
  functionValue.set_bool_value(functionEnabled);
  auto status = dataCenter_.PublishValue(connId, std::string(functionTag), functionValue, DataCenterProto::QUALITY_GOOD, 0);
  if (!status.ok()) {
    LOG_ERROR("AVC 发布功能投入状态失败: group_name={}, conn_id={}, tag={}, value={}, 触发来源={}, 原因={}",
              groupName,
              connId,
              functionTag,
              functionEnabled,
              trigger,
              status.error_message());
  } else {
    LOG_DEBUG("AVC 已发布功能投入状态: group_name={}, conn_id={}, tag={}, value={}, 触发来源={}",
              groupName,
              connId,
              functionTag,
              functionEnabled,
              trigger);
  }

  DataCenterProto::PointValue remoteValue;
  remoteValue.set_bool_value(remoteEnabled);
  status = dataCenter_.PublishValue(connId, std::string(remoteTag), remoteValue, DataCenterProto::QUALITY_GOOD, 0);
  if (!status.ok()) {
    LOG_ERROR("AVC 发布远方操作状态失败: group_name={}, conn_id={}, tag={}, value={}, 触发来源={}, 原因={}",
              groupName,
              connId,
              remoteTag,
              remoteEnabled,
              trigger,
              status.error_message());
  } else {
    LOG_DEBUG("AVC 已发布远方操作状态: group_name={}, conn_id={}, tag={}, value={}, 触发来源={}",
              groupName,
              connId,
              remoteTag,
              remoteEnabled,
              trigger);
  }
}

bool GroupManager::handleUpdateLocked(GroupRuntime* group, const DataCenterProto::PointUpdate& update) {
  if (group == nullptr) {
    return false;
  }
  const auto& tag = update.dst_tag();
  Decimal raw;
  if (!pointValueToDecimal(update.value(), &raw)) {
    return false;
  }

  if (!group->voltageMeasTag.empty() && tag == group->voltageMeasTag) {
    const auto changed = !group->hasVoltageMeasRaw || !sameValue(group->voltageMeasRaw, raw);
    group->voltageMeasRaw = raw;
    group->hasVoltageMeasRaw = true;
    return changed;
  }

  if (!group->commandTag.empty() && tag == group->commandTag) {
    ++group->commandRevision;
    group->hasDirectResolvedDesiredTotalQKvar = false;
    if (group->voltageMode) {
      // 命令点即使重复下发同值，也要重新触发一次控制计算。
      group->voltageCmdRaw = raw;
      group->hasVoltageCmdRaw = true;
      return true;
    }
    // 总无功命令点即使重复下发同值，也要重新触发一次控制计算。
    group->qTotalCmdRaw = raw;
    group->hasQTotalCmdRaw = true;
    return true;
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

  auto status = dataCenter_.PublishDecimal(
      connId, std::string(theoreticalLowerTag),
      defaultOutput.theoreticalLowerQKvar.ToFixedString(), theoreticalQuality,
      0);
  if (!status.ok()) {
    LOG_ERROR("AVC 发布默认点失败: group_name={}, tag={}, 触发来源={}, 原因={}", groupName, theoreticalLowerTag, trigger, status.error_message());
  }
  status = dataCenter_.PublishDecimal(
      connId, std::string(theoreticalUpperTag),
      defaultOutput.theoreticalUpperQKvar.ToFixedString(), theoreticalQuality,
      0);
  if (!status.ok()) {
    LOG_ERROR("AVC 发布默认点失败: group_name={}, tag={}, 触发来源={}, 原因={}", groupName, theoreticalUpperTag, trigger, status.error_message());
  }
  status = dataCenter_.PublishDecimal(
      connId, std::string(dynamicLowerTag),
      defaultOutput.dynamicLowerQKvar.ToFixedString(),
      defaultOutput.dynamicQuality, 0);
  if (!status.ok()) {
    LOG_ERROR("AVC 发布默认点失败: group_name={}, tag={}, 触发来源={}, 原因={}", groupName, dynamicLowerTag, trigger, status.error_message());
  }
  status = dataCenter_.PublishDecimal(
      connId, std::string(dynamicUpperTag),
      defaultOutput.dynamicUpperQKvar.ToFixedString(),
      defaultOutput.dynamicQuality, 0);
  if (!status.ok()) {
    LOG_ERROR("AVC 发布默认点失败: group_name={}, tag={}, 触发来源={}, 原因={}", groupName, dynamicUpperTag, trigger, status.error_message());
  }
  LOG_DEBUG(
      "AVC 已发布默认限值点: group_name={}, 触发来源={}, 理论下限={}, 理论上限={}, 当前下限={}, 当前上限={}, 当前质量={}, 不可控成员数={}, 缺测不可控成员数={}",
      groupName,
      trigger,
      defaultOutput.theoreticalLowerQKvar.ToFixedString(),
      defaultOutput.theoreticalUpperQKvar.ToFixedString(),
      defaultOutput.dynamicLowerQKvar.ToFixedString(),
      defaultOutput.dynamicUpperQKvar.ToFixedString(),
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
  Decimal value;
  if (!pointValueToDecimal(update.value(), &value)) {
    LOG_WARNING("AVC 发布调节返回值跳过: conn_id={}, tag={}, 原因=命令点值类型不支持", connId, commandEchoTag);
    return;
  }

  const auto echoValue = commandEchoEngineeringValue(commandSignal, commandMode, value);
  if (!echoValue.has_value()) {
    LOG_WARNING("AVC 发布调节返回值跳过: conn_id={}, tag={}, 原因={}",
                connId, commandEchoTag,
                mskdsp::numeric::DecimalErrorMessage(echoValue.error()));
    return;
  }
  auto status = dataCenter_.PublishDecimal(
      connId, std::string(commandEchoTag), echoValue->ToFixedString(),
      update.quality(), update.ts_ms());
  if (!status.ok()) {
    LOG_ERROR("AVC 发布调节返回值失败: conn_id={}, tag={}, 原因={}", connId, commandEchoTag, status.error_message());
  } else {
    LOG_DEBUG("AVC 已发布调节返回值: conn_id={}, tag={}, value={}, echo_value={}, 质量={}, ts_ms={}",
              connId,
              commandEchoTag,
              value.ToFixedString(),
              echoValue->ToFixedString(),
              static_cast<int>(update.quality()),
              update.ts_ms());
  }
}

void GroupManager::controlTick(const std::string& groupName) {
  AVCProto::GroupConfig config;
  uint32_t connId = 0;
  bool functionEnabled = true;
  ControlInput input;
  uint64_t commandRevision = 0;

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
    functionEnabled = it->second.functionEnabled;
    commandRevision = it->second.commandRevision;
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
    const bool stableDeltaTarget = config.control_mode() == AVCProto::CONTROL_MODE_DIRECT_CYCLIC &&
                                   config.q_total_cmd().mode() == AVCProto::VALUE_MODE_DELTA &&
                                   config.q_total_cmd().delta_base() == AVCProto::DELTA_BASE_LAST_TARGET;
    if (stableDeltaTarget && it->second.hasDirectResolvedDesiredTotalQKvar &&
        it->second.directResolvedCommandRevision == it->second.commandRevision) {
      input.hasDesiredTotalOverride = true;
      input.desiredTotalOverrideQKvar = it->second.directResolvedDesiredTotalQKvar;
    }
  }

  if (connId == 0) {
    return;
  }

  const auto quality = DataCenterProto::QUALITY_GOOD;
  if (const auto voltageMeas = ComputeVoltageMeas(config, input)) {
    auto status = dataCenter_.PublishDecimal(
        connId,
        std::string(defaultPointTag(
            AVCProto::DEFAULT_POINT_KIND_CURRENT_VOLTAGE)),
        voltageMeas->ToFixedString(), quality, 0);
    if (!status.ok()) {
      LOG_ERROR("AVC 发布当前电压失败: group_name={}, conn_id={}, 原因={}", groupName, connId, status.error_message());
    } else {
      LOG_DEBUG("AVC 已发布当前电压: group_name={}, conn_id={}, value={}", groupName, connId, voltageMeas->ToFixedString());
    }
  }

  const auto totalQMeasKvar = ComputeTotalQMeasKvar(config, input);
  if (totalQMeasKvar.has_value()) {
    auto status = dataCenter_.PublishDecimal(
        connId,
        std::string(defaultPointTag(
            AVCProto::DEFAULT_POINT_KIND_TOTAL_Q_MEAS)),
        totalQMeasKvar->ToFixedString(), quality, 0);
    if (!status.ok()) {
      LOG_ERROR("AVC 发布总无功实测失败: group_name={}, conn_id={}, value={}, 原因={}",
                groupName,
                connId,
                totalQMeasKvar->ToFixedString(),
                status.error_message());
    }
  }
  publishDefaultLimitPoints(groupName,
                            config.control_mode() == AVCProto::CONTROL_MODE_DIRECT_CYCLIC ? "周期直分配控制" : "PI 事件触发控制");

  if (!functionEnabled) {
    LOG_DEBUG("AVC 跳过控制输出: group_name={}, 原因=AVC 功能未投入", groupName);
    return;
  }

  const auto outputOpt = ComputeControlOutput(config, input, weightedStrategy_);
  if (!outputOpt) {
    return;
  }
  const auto& output = *outputOpt;

  // 计算期间若收到更新命令，当前结果已过期，不得覆盖最新目标。
  {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end() || it->second.state != AVCProto::GROUP_STATE_RUNNING ||
        it->second.commandRevision != commandRevision) {
      LOG_DEBUG("AVC 丢弃已过期控制计算结果: group_name={}, command_revision={}", groupName, commandRevision);
      return;
    }
  }

  const bool stableDeltaTarget = config.control_mode() == AVCProto::CONTROL_MODE_DIRECT_CYCLIC &&
                                 config.q_total_cmd().mode() == AVCProto::VALUE_MODE_DELTA &&
                                 config.q_total_cmd().delta_base() == AVCProto::DELTA_BASE_LAST_TARGET;
  if (stableDeltaTarget && !input.hasDesiredTotalOverride) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it != groupsByName_.end() && it->second.state == AVCProto::GROUP_STATE_RUNNING &&
        it->second.commandRevision == commandRevision) {
      it->second.directResolvedCommandRevision = commandRevision;
      it->second.directResolvedDesiredTotalQKvar = output.rawDesiredTotalQKvar;
      it->second.hasDirectResolvedDesiredTotalQKvar = true;
      LOG_DEBUG("AVC 周期直分配已固定增量命令解析目标: group_name={}, command_revision={}, desired_total_q_kvar={}",
                groupName,
                commandRevision,
                output.rawDesiredTotalQKvar.ToFixedString());
    }
  }

  // 周期直分配模式按命令控制周期限制成员设定下发，状态量仍按计算周期刷新。
  const bool directCyclic = config.control_mode() == AVCProto::CONTROL_MODE_DIRECT_CYCLIC;
  bool allowMemberPublish = true;
  if (directCyclic) {
    const auto minInterval = std::chrono::duration<double>(config.command_control_period_seconds());
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it != groupsByName_.end() && it->second.hasLastCommandPublishedAt) {
      allowMemberPublish = (std::chrono::steady_clock::now() - it->second.lastCommandPublishedAt) >= minInterval;
    }
  }

  auto status = dataCenter_.PublishDecimal(
      connId,
      std::string(defaultPointTag(
          AVCProto::DEFAULT_POINT_KIND_TOTAL_Q_TARGET)),
      output.actualTargetQKvar.ToFixedString(), quality, 0);
  if (!status.ok()) {
    LOG_ERROR("AVC 发布总无功目标失败: group_name={}, conn_id={}, value={}, 原因={}",
              groupName,
              connId,
              output.actualTargetQKvar.ToFixedString(),
              status.error_message());
  }

  status = dataCenter_.PublishDecimal(
      connId,
      std::string(defaultPointTag(
          AVCProto::DEFAULT_POINT_KIND_TOTAL_Q_ERROR)),
      output.totalQErrorKvar.ToFixedString(), quality, 0);
  if (!status.ok()) {
    LOG_ERROR("AVC 发布总无功偏差失败: group_name={}, conn_id={}, value={}, 原因={}",
              groupName,
              connId,
              output.totalQErrorKvar.ToFixedString(),
              status.error_message());
  }

  if (output.hasVoltageError) {
    status = dataCenter_.PublishDecimal(
        connId,
        std::string(defaultPointTag(
            AVCProto::DEFAULT_POINT_KIND_VOLTAGE_ERROR)),
        output.voltageError.ToFixedString(), quality, 0);
    if (!status.ok()) {
      LOG_ERROR("AVC 发布电压偏差失败: group_name={}, conn_id={}, value={}, 原因={}",
                groupName,
                connId,
                output.voltageError.ToFixedString(),
                status.error_message());
    }
  }

  bool commandApplied = false;
  for (size_t i = 0; i < output.memberPublish.size() && i < output.memberPublishKvar.size(); ++i) {
    if (!allowMemberPublish || !output.memberPublish[i]) {
      continue;
    }
    const auto& member = config.members(static_cast<int>(i));
    if (!member.has_q_set() || !member.q_set().has_signal() || member.q_set().signal().tag().empty()) {
      continue;
    }
    status = dataCenter_.PublishDecimal(
        connId, member.q_set().signal().tag(),
        output.memberPublishKvar[i].ToFixedString(), quality, 0);
    if (!status.ok()) {
      LOG_ERROR("AVC 下发成员无功设定失败: group_name={}, member_name={}, tag={}, publish_kvar={}, 原因={}",
                groupName,
                member.member_name(),
                member.q_set().signal().tag(),
                output.memberPublishKvar[i].ToFixedString(),
                status.error_message());
    } else {
      commandApplied = true;
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
    if (it == groupsByName_.end() || it->second.state != AVCProto::GROUP_STATE_RUNNING ||
        it->second.commandRevision != commandRevision) {
      LOG_DEBUG("AVC 跳过过期控制状态写回: group_name={}, command_revision={}", groupName, commandRevision);
      return;
    }

    if (!directCyclic || commandApplied) {
      it->second.hasLastDesiredTotalQKvar = output.hasLastDesiredTotalQKvar;
      it->second.lastDesiredTotalQKvar = output.nextLastDesiredTotalQKvar;
      it->second.hasLastMemberTargetQKvar = output.hasLastMemberTargetQKvar;
      it->second.lastMemberTargetQKvar = output.nextLastMemberTargetQKvar;
    }
    if (directCyclic && commandApplied) {
      it->second.lastCommandPublishedAt = std::chrono::steady_clock::now();
      it->second.hasLastCommandPublishedAt = true;
    }

    if (!unallocatedQKvar.IsZero()) {
      if (!it->second.hasLastUnallocatedQKvar ||
          unallocatedQKvar != it->second.lastUnallocatedQKvar) {
        shouldLogUnallocated = true;
        it->second.hasLastUnallocatedQKvar = true;
        it->second.lastUnallocatedQKvar = unallocatedQKvar;
      }
    } else {
      it->second.hasLastUnallocatedQKvar = false;
      it->second.lastUnallocatedQKvar = Decimal{};
    }
  }

  if (directCyclic && !allowMemberPublish) {
    LOG_DEBUG("AVC 周期控制暂缓成员命令下发: group_name={}, 原因=命令控制周期未到", groupName);
  }

  if (shouldLogUnallocated) {
    LOG_WARNING("AVC 分配受限: group_name={}, unallocated_q_kvar={}, target_controllable_q_kvar={}, passive_q_kvar={}, desired_total_q_kvar={}, actual_target_q_kvar={}",
                groupName,
                unallocatedQKvar.ToFixedString(),
                targetControllableQKvar.ToFixedString(),
                passiveQKvar.ToFixedString(),
                desiredTotalQKvar.ToFixedString(),
                actualTargetQKvar.ToFixedString());
  }
}

}  // namespace AVC
