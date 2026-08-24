#include "AGCGroupManager.h"

#include <grpcpp/client_context.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <format>
#include <unordered_set>
#include <utility>

#include "AGCControl.h"
#include "AGCDefaultPoints.h"
#include "AGCGroupValidation.h"
#include "AGCLibInfo.h"
#include "Logger.h"
#include "ThreadUtil.hpp"

namespace AGC {
namespace {
grpc::Status makeNotFound(const std::string &groupName) {
  return grpc::Status(grpc::StatusCode::NOT_FOUND, std::format("未找到控制组: {}", groupName));
}

grpc::Status makeInvalid(std::string message) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::move(message));
}

grpc::Status makePreconditionFailed(std::string message) {
  return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::move(message));
}

const char *groupStateToString(AGCProto::GroupState state) {
  switch (state) {
  case AGCProto::GROUP_STATE_RUNNING:
    return "运行中";
  case AGCProto::GROUP_STATE_PENDING_DELETE:
    return "待删除";
  case AGCProto::GROUP_STATE_STOPPED:
    return "已停止";
  case AGCProto::GROUP_STATE_UNSPECIFIED:
  default:
    return "未指定";
  }
}

constexpr double kValueChangeEps = 1e-6;

bool sameValue(double lhs, double rhs) {
  return std::fabs(lhs - rhs) <= kValueChangeEps;
}

double effectiveScale(const AGCProto::SignalSpec &signal) {
  return signal.scale() == 0.0 ? 1.0 : signal.scale();
}

double commandEchoEngineeringValue(const AGCProto::ValueSpec &spec, double value) {
  const auto scale = effectiveScale(spec.signal());
  if (spec.mode() == AGCProto::VALUE_MODE_DELTA) {
    return value * scale;
  }
  return value * scale + spec.signal().offset();
}

std::string_view defaultPointTag(AGCProto::DefaultPointKind kind) {
  for (const auto &point : DefaultPointDefinitions()) {
    if (point.kind == kind) {
      return point.tag;
    }
  }
  return {};
}
}  // namespace

GroupManager::GroupManager(std::string moduleName, std::filesystem::path configDbPath) :
  groupStore_(configDbPath),
  controlProfileStore_(std::move(configDbPath)),
  dataCenter_(std::move(moduleName)) {}

void GroupManager::setDataCenterServerAddress(std::string address) {
  dataCenter_.setServerAddress(std::move(address));
}

void GroupManager::setDataCenterStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub) {
  dataCenter_.setStub(std::move(stub));
}

grpc::Status GroupManager::validateGroupName(const std::string &groupName) const {
  if (groupName.empty()) {
    return makeInvalid("group_name 不能为空");
  }
  return grpc::Status::OK;
}

grpc::Status GroupManager::validateGroupConfig(const AGCProto::GroupConfig &config) const {
  return AGC::ValidateGroupConfig(config);
}

grpc::Status GroupManager::fillGroupInfoLocked(const GroupRuntime &g, AGCProto::GroupInfo *out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  out->Clear();
  *out->mutable_config() = g.config;
  out->set_conn_id(g.connId);
  out->set_state(g.state);
  out->set_last_error(g.lastError);
  out->set_function_enabled(g.functionEnabled);
  out->set_remote_enabled(g.remoteEnabled);
  FillDefaultPointInfos(out->mutable_default_points());
  return grpc::Status::OK;
}

AGCProto::GroupControlProfile GroupManager::makeDefaultControlProfileLocked(const GroupRuntime &g) const {
  AGCProto::GroupControlProfile profile;
  profile.set_group_name(g.config.group_name());
  const auto persisted = controlProfilesByGroup_.find(g.config.group_name());
  for (const auto &member : g.config.members()) {
    if (!member.controllable()) {
      continue;
    }
    const AGCProto::MemberControlProfile *stored = nullptr;
    if (persisted != controlProfilesByGroup_.end()) {
      for (const auto &candidate : persisted->second.members()) {
        if (candidate.member_name() == member.member_name()) {
          stored = &candidate;
          break;
        }
      }
    }
    auto *target = profile.add_members();
    if (stored != nullptr) {
      *target = *stored;
    } else {
      target->set_member_name(member.member_name());
    }
  }
  if (persisted != controlProfilesByGroup_.end()) {
    profile.set_version(persisted->second.version());
    profile.set_confirmed_at_ms(persisted->second.confirmed_at_ms());
  }
  return profile;
}

grpc::Status GroupManager::fillTuningStatusLocked(const GroupRuntime &g, AGCProto::TuningStatus *out) const {
  if (out == nullptr) {
    return makeInvalid("out 为空");
  }
  *out = g.tuningStatus;
  out->set_group_name(g.config.group_name());
  if (out->state() == AGCProto::TUNING_STATE_RUNNING && out->started_at_ms() > 0) {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    out->set_elapsed_ms(now > static_cast<int64_t>(out->started_at_ms())
                            ? static_cast<uint64_t>(now) - out->started_at_ms()
                            : 0);
  }
  return grpc::Status::OK;
}

grpc::Status GroupManager::validateTuningConfig(const AGCProto::TuningConfig &config) const {
  if (!std::isfinite(config.target_lower_kw()) || !std::isfinite(config.target_upper_kw()) ||
      config.target_lower_kw() >= config.target_upper_kw()) {
    return makeInvalid("调试目标范围必须是有限数值且下限小于上限");
  }
  if (config.total_time_minutes() == 0 || config.attempt_max_time_minutes() == 0 ||
      config.target_entry_time_seconds() == 0 || config.stable_hold_time_seconds() == 0) {
    return makeInvalid("调试总时间、单轮时间、进入目标时间和稳定保持时间必须大于 0");
  }
  if (config.min_up_tests() < 3 || config.min_down_tests() < 3) {
    return makeInvalid("调试至少需要 3 次有效上调和 3 次有效下调");
  }
  if (!std::isfinite(config.total_tolerance_kw()) || config.total_tolerance_kw() <= 0.0) {
    return makeInvalid("调试总量精度必须是大于 0 的有限数值");
  }
  const uint64_t requiredMinutes = static_cast<uint64_t>(config.min_up_tests()) + config.min_down_tests();
  if (static_cast<uint64_t>(config.total_time_minutes()) < requiredMinutes * config.attempt_max_time_minutes()) {
    return makeInvalid("调试总时间不足以覆盖配置的最低上调/下调次数和单轮最大时间");
  }
  return grpc::Status::OK;
}

grpc::Status GroupManager::saveControlProfilesLocked() {
  AGCProto::ControlProfilesConfig config;
  for (const auto &[_, profile] : controlProfilesByGroup_) {
    *config.add_profiles() = profile;
  }
  const auto status = controlProfileStore_.Save(config);
  if (!status.ok()) {
    LOG_ERROR("AGC 固定控制参数落盘失败: 原因={}", status.error_message());
  }
  return status;
}

grpc::Status GroupManager::checkStartPreconditionsLocked(const GroupRuntime &g) const {
  if (g.state == AGCProto::GROUP_STATE_PENDING_DELETE) {
    return makePreconditionFailed("控制组处于待删除状态");
  }
  auto status = validateGroupConfig(g.config);
  if (!status.ok()) {
    return makePreconditionFailed(std::format("控制组配置未通过当前校验: {}", status.error_message()));
  }
  if (g.connId == 0) {
    return makePreconditionFailed("控制组 conn_id 无效");
  }
  if (g.subscribeTags.empty()) {
    return makePreconditionFailed("控制组订阅标签为空，当前规则要求控制组配置完整后才启动控制组功能");
  }
  return grpc::Status::OK;
}

grpc::Status GroupManager::tryAutoStartGroup(const std::string &groupName, std::string_view trigger) {
  size_t memberCount = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      return makeNotFound(groupName);
    }
    memberCount = static_cast<size_t>(it->second.config.members_size());
    if (it->second.state == AGCProto::GROUP_STATE_RUNNING) {
      LOG_INFO("AGC 自动启动控制组跳过: group_name={}, 触发来源={}, 原因=控制组已在运行", groupName, trigger);
      return grpc::Status::OK;
    }
    auto status = checkStartPreconditionsLocked(it->second);
    if (!status.ok()) {
      it->second.lastError = status.error_message();
      LOG_INFO("AGC 自动启动控制组跳过: group_name={}, 触发来源={}, 成员数={}, 原因={}", groupName, trigger, memberCount, status.error_message());
      return status;
    }
  }

  LOG_INFO("AGC 自动启动控制组: group_name={}, 触发来源={}, 规则=控制组配置通过当前校验且非待删除, 成员数={}", groupName, trigger, memberCount);
  auto status = StartGroup(groupName);
  if (!status.ok()) {
    LOG_WARNING("AGC 自动启动控制组失败: group_name={}, 触发来源={}, 原因={}", groupName, trigger, status.error_message());
  } else {
    LOG_INFO("AGC 自动启动控制组成功: group_name={}, 触发来源={}", groupName, trigger);
  }
  return status;
}

void GroupManager::TryAutoStartReadyGroups(std::string_view trigger) {
  std::vector<std::string> groupNames;
  {
    std::lock_guard<std::mutex> lock(mu_);
    groupNames.reserve(groupsByName_.size());
    for (const auto &[groupName, group] : groupsByName_) {
      if (group.state == AGCProto::GROUP_STATE_STOPPED) {
        groupNames.push_back(groupName);
      }
    }
  }

  if (groupNames.empty()) {
    LOG_INFO("AGC 自动启动检查完成: 触发来源={}, 当前无可评估控制组", trigger);
    return;
  }

  for (const auto &groupName : groupNames) {
    (void)tryAutoStartGroup(groupName, trigger);
  }
}

AGCProto::GroupsConfig GroupManager::dumpGroupsConfigLocked() const {
  AGCProto::GroupsConfig config;
  for (const auto &[_, group] : groupsByName_) {
    auto *persisted = config.add_persisted_groups();
    *persisted->mutable_config() = group.config;
    persisted->set_pending_delete(group.state == AGCProto::GROUP_STATE_PENDING_DELETE);
  }
  return config;
}

grpc::Status GroupManager::saveGroupsLocked() {
  auto config = dumpGroupsConfigLocked();
  auto status = groupStore_.Save(config);
  if (!status.ok()) {
    LOG_ERROR("AGC 控制组配置落盘失败: 原因={}", status.error_message());
  }
  return status;
}

grpc::Status GroupManager::restoreGroupFromConfig(const AGCProto::GroupConfig &config, AGCProto::GroupState restoredState) {
  auto status = validateGroupConfig(config);
  if (!status.ok()) {
    return status;
  }
  LOG_INFO("AGC 开始恢复控制组持久化记录: group_name={}, 状态={}, 成员数={}", config.group_name(), groupStateToString(restoredState), config.members_size());

  DataCenterProto::ConnectionInfo connInfo;
  status = dataCenter_.GetOrCreateConnection(config.group_name(), &connInfo);
  if (!status.ok()) {
    LOG_ERROR("AGC 恢复控制组时获取 DataCenter 连接失败: group_name={}, 成员数={}, 原因={}", config.group_name(), config.members_size(), status.error_message());
    return status;
  }
  if (connInfo.conn_id() == 0) {
    LOG_ERROR("AGC 恢复控制组时 DataCenter 返回无效 conn_id: group_name={}", config.group_name());
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
  runtime.controlProfile = makeDefaultControlProfileLocked(runtime);
  runtime.integralMemoryKw.assign(static_cast<size_t>(config.members_size()), 0.0);
  runtime.tuningStatus.set_group_name(config.group_name());
  runtime.tuningStatus.set_state(AGCProto::TUNING_STATE_IDLE);

  const auto tags = collectAllTags(config);
  if (!tags.empty()) {
    std::vector<std::string> tagList;
    tagList.reserve(tags.size());
    for (const auto &tag : tags) {
      tagList.emplace_back(tag);
    }
    auto connTagsStatus = dataCenter_.UpsertConnTags(runtime.connId, tagList, true);
    if (!connTagsStatus.ok()) {
      runtime.lastError = connTagsStatus.error_message();
      LOG_ERROR("AGC 恢复控制组时同步 DataCenter 连接标签注册表失败: group_name={}, conn_id={}, 标签数={}, 原因={}", config.group_name(), runtime.connId, tagList.size(), connTagsStatus.error_message());
    } else {
      LOG_INFO("AGC 恢复控制组时已同步 DataCenter 连接标签注册表: group_name={}, conn_id={}, 标签数={}", config.group_name(), runtime.connId, tagList.size());
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
  AGCProto::ControlProfilesConfig profiles;
  auto profileStatus = controlProfileStore_.Load(&profiles);
  if (!profileStatus.ok()) {
    LOG_ERROR("AGC 固定控制参数加载失败: 原因={}", profileStatus.error_message());
    return profileStatus;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    controlProfilesByGroup_.clear();
    for (const auto &profile : profiles.profiles()) {
      controlProfilesByGroup_[profile.group_name()] = profile;
    }
  }
  LOG_INFO("AGC 固定控制参数加载完成: 控制组数={}", profiles.profiles_size());

  AGCProto::GroupsConfig config;
  auto status = groupStore_.Load(&config);
  if (!status.ok()) {
    LOG_ERROR("AGC 控制组配置加载失败: 原因={}", status.error_message());
    return status;
  }
  LOG_INFO("AGC 控制组持久化配置载入摘要: persisted_groups={}, legacy_groups={}", config.persisted_groups_size(), config.groups_size());
  if (config.groups_size() == 0 && config.persisted_groups_size() == 0) {
    LOG_INFO("AGC 未发现本地控制组配置");
    return grpc::Status::OK;
  }

  size_t restored = 0;
  size_t failed = 0;
  if (config.persisted_groups_size() > 0) {
    for (const auto &persisted : config.persisted_groups()) {
      if (!persisted.has_config()) {
        ++failed;
        LOG_ERROR("AGC 恢复控制组失败: group_name=<空>, 原因=持久化记录缺少 config");
        continue;
      }
      const auto restoredState = persisted.pending_delete() ? AGCProto::GROUP_STATE_PENDING_DELETE
                                                            : AGCProto::GROUP_STATE_STOPPED;
      status = restoreGroupFromConfig(persisted.config(), restoredState);
      if (!status.ok()) {
        ++failed;
        LOG_ERROR("AGC 恢复控制组失败: group_name={}, 原因={}", persisted.config().group_name(), status.error_message());
        continue;
      }
      ++restored;
      LOG_INFO("AGC 已恢复控制组配置: group_name={}, 状态={}", persisted.config().group_name(), groupStateToString(restoredState));
    }
  } else {
    for (const auto &group : config.groups()) {
      status = restoreGroupFromConfig(group, AGCProto::GROUP_STATE_STOPPED);
      if (!status.ok()) {
        ++failed;
        LOG_ERROR("AGC 恢复控制组失败: group_name={}, 原因={}", group.group_name(), status.error_message());
        continue;
      }
      ++restored;
      LOG_INFO("AGC 已恢复控制组配置: group_name={}, 状态=已停止(兼容旧持久化格式)", group.group_name());
    }
  }

  LOG_INFO("AGC 控制组配置恢复完成: 成功={}, 失败={}", restored, failed);
  TryAutoStartReadyGroups("持久化恢复完成后");
  if (failed > 0) {
    return grpc::Status(grpc::StatusCode::INTERNAL, "部分控制组恢复失败");
  }
  return grpc::Status::OK;
}

grpc::Status GroupManager::UpsertGroup(const AGCProto::UpsertGroupRequest &request, AGCProto::GroupInfo *out) {
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
      if (it->second.state == AGCProto::GROUP_STATE_RUNNING) {
        return makePreconditionFailed("更新配置前请先停止控制组");
      }
      if (it->second.state == AGCProto::GROUP_STATE_PENDING_DELETE) {
        return makePreconditionFailed("控制组处于待删除状态");
      }

      it->second.config = request.config();
      rebuildTagCache(&it->second);
      it->second.controlProfile = makeDefaultControlProfileLocked(it->second);
      it->second.integralMemoryKw.assign(static_cast<size_t>(request.config().members_size()), 0.0);
      it->second.lastError.clear();
      fillGroupInfoLocked(it->second, out);
    } else {
      // create_only 在 DataCenter 已存在同名连接时应失败。
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
      auto &g = pos->second;
      g.config = request.config();
      g.connId = connInfo.conn_id();
      g.state = AGCProto::GROUP_STATE_STOPPED;
      g.lastError.clear();
      g.functionEnabled = true;
      g.remoteEnabled = true;
      rebuildTagCache(&g);
      g.controlProfile = makeDefaultControlProfileLocked(g);
      g.integralMemoryKw.assign(static_cast<size_t>(request.config().members_size()), 0.0);
      g.tuningStatus.set_group_name(groupName);
      g.tuningStatus.set_state(AGCProto::TUNING_STATE_IDLE);
      fillGroupInfoLocked(g, out);
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

  // 尽力而为：将 tags 注册到 DataCenter 连接标签注册表（不回滚）。
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
      for (const auto &t : tags) {
        tagList.emplace_back(t);
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

grpc::Status GroupManager::GetGroup(const std::string &groupName, AGCProto::GroupInfo *out) const {
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

grpc::Status GroupManager::ListGroups(AGCProto::ListGroupsResponse *out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  std::lock_guard<std::mutex> lock(mu_);
  out->Clear();
  for (const auto &[_, g] : groupsByName_) {
    auto *elem = out->add_groups();
    fillGroupInfoLocked(g, elem);
  }
  return grpc::Status::OK;
}

void GroupManager::startThreadsLocked(const std::string &groupName, GroupRuntime *g) {
  if (g == nullptr) {
    return;
  }
  rebuildTagCache(g);

  const auto connId = g->connId;
  auto tags = g->subscribeTags;
  if (connId == 0 || tags.empty()) {
    LOG_WARNING("AGC 控制组启动事件触发控制功能失败: group_name={}, conn_id={}, 原因=订阅标签为空或连接无效", groupName, connId);
    return;
  }

  g->controlTrigger = std::make_shared<ControlTrigger>();
  auto trigger = g->controlTrigger;

  g->controlThread = ModuleManager::StartModuleThread(
      AGCLibInfo.LIB_NAME,
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

  g->dcSubscribeContext = std::make_shared<grpc::ClientContext>();
  auto ctx = g->dcSubscribeContext;

  g->dcSubscribeThread = ModuleManager::StartModuleThread(
      AGCLibInfo.LIB_NAME,
      [this, groupName, ctx, connId, tags](std::stop_token st) {
        std::stop_callback cb(st, [&ctx]() { ctx->TryCancel(); });

        auto reader = dataCenter_.Subscribe(ctx.get(), connId, tags, false);
        if (!reader) {
          LOG_ERROR("AGC 建立 DataCenter 订阅失败: group_name={}, conn_id={}, 标签数={}", groupName, connId, tags.size());
          return;
        }

        DataCenterProto::PointUpdate update;
        while (reader->Read(&update)) {
          bool publishCommandEcho = false;
          uint32_t commandEchoConnId = 0;
          AGCProto::ValueSpec commandEchoSpec;
          {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = groupsByName_.find(groupName);
            if (it == groupsByName_.end()) {
              break;
            }
            if (!it->second.cmdTag.empty() && update.dst_tag() == it->second.cmdTag) {
              publishCommandEcho = true;
              commandEchoConnId = it->second.connId;
              commandEchoSpec = it->second.config.p_cmd();
            }
            if (handleUpdateLocked(&it->second, update)) {
              requestControlLocked(groupName, &it->second, "订阅输入点更新", update.dst_tag());
            }
          }
          if (publishCommandEcho && commandEchoConnId != 0) {
            publishCommandEchoPoint(commandEchoConnId, commandEchoSpec, update);
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
  LOG_INFO("AGC 控制组已启用事件触发控制功能: group_name={}, conn_id={}, 订阅标签数={}", groupName, connId, tags.size());
}

grpc::Status GroupManager::StartGroup(const std::string &groupName) {
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
    if (it->second.state == AGCProto::GROUP_STATE_RUNNING) {
      alreadyRunning = true;
    } else {
      status = checkStartPreconditionsLocked(it->second);
      if (!status.ok()) {
        it->second.lastError = status.error_message();
        return status;
      }
      startThreadsLocked(groupName, &it->second);
      it->second.state = AGCProto::GROUP_STATE_RUNNING;
      it->second.lastError.clear();
    }
  }
  if (alreadyRunning) {
    publishControlStatePoints(groupName, "控制组启动幂等请求");
    LOG_INFO("AGC 启动控制组请求幂等成功: group_name={}, 原因=控制组已在运行", groupName);
    return grpc::Status::OK;
  }
  publishControlStatePoints(groupName, "控制组启动");
  primeControlInputs(groupName);
  LOG_INFO("AGC 控制组已启动事件触发控制功能: group_name={}", groupName);
  return grpc::Status::OK;
}

grpc::Status GroupManager::StopGroup(const std::string &groupName) {
  auto status = validateGroupName(groupName);
  if (!status.ok()) {
    return status;
  }

  std::jthread dcSubscribeThread;
  std::jthread controlThread;
  std::jthread tuningThread;
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
    tuningThread = std::move(it->second.tuningThread);
    controlTrigger = std::move(it->second.controlTrigger);
    it->second.dcSubscribeContext.reset();
    pendingDelete = (it->second.state == AGCProto::GROUP_STATE_PENDING_DELETE);
    it->second.state = pendingDelete ? AGCProto::GROUP_STATE_PENDING_DELETE : AGCProto::GROUP_STATE_STOPPED;
    std::fill(it->second.integralMemoryKw.begin(), it->second.integralMemoryKw.end(), 0.0);
    if (it->second.tuningStatus.state() == AGCProto::TUNING_STATE_RUNNING) {
      it->second.tuningStatus.set_state(AGCProto::TUNING_STATE_STOPPED);
      it->second.tuningStatus.set_last_error("控制组停止导致自动调试中止");
    }
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
  if (tuningThread.joinable()) {
    tuningThread.request_stop();
    tuningThread.join();
  }
  if (pendingDelete) {
    LOG_INFO("AGC 控制组已停止并保持待删除状态: group_name={}", groupName);
  } else {
    LOG_INFO("AGC 控制组已停止: group_name={}", groupName);
  }
  return grpc::Status::OK;
}

grpc::Status GroupManager::DeleteGroup(const std::string &groupName) {
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
    AGCProto::GroupsConfig groupsConfig;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = groupsByName_.find(groupName);
      if (it != groupsByName_.end()) {
        it->second.state = AGCProto::GROUP_STATE_PENDING_DELETE;
        it->second.lastError = dc.error_message();
        groupsConfig = dumpGroupsConfigLocked();
      }
    }
    if (groupsConfig.persisted_groups_size() > 0) {
      auto saveStatus = groupStore_.Save(groupsConfig);
      if (!saveStatus.ok()) {
        LOG_ERROR("AGC 待删除控制组配置落盘失败: group_name={}, 原因={}", groupName, saveStatus.error_message());
        return saveStatus;
      }
    }
    LOG_WARNING("AGC 删除控制组失败，已标记待删除: group_name={}, 原因={}", groupName, dc.error_message());
    return dc;
  }

  std::lock_guard<std::mutex> lock(mu_);
  groupsByName_.erase(groupName);
  status = saveGroupsLocked();
  if (!status.ok()) {
    return status;
  }
  controlProfilesByGroup_.erase(groupName);
  status = saveControlProfilesLocked();
  if (!status.ok()) {
    LOG_WARNING("AGC 控制组已删除，但清理固定控制参数失败: group_name={}, 原因={}", groupName, status.error_message());
  }
  return grpc::Status::OK;
}

grpc::Status GroupManager::ExecuteCommand(
    const DataCenterProto::ExecuteCommandRequest &request,
    DataCenterProto::ExecuteCommandResponse *response) {
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
  const auto functionTag = defaultPointTag(AGCProto::DEFAULT_POINT_KIND_FUNCTION_ENABLE);
  const auto remoteTag = defaultPointTag(AGCProto::DEFAULT_POINT_KIND_REMOTE_OPERATION);
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
        response->set_reason("未找到 AGC 控制组");
        return grpc::Status::OK;
      }
      if (it->second.state == AGCProto::GROUP_STATE_PENDING_DELETE) {
        response->set_status(DataCenterProto::COMMAND_REJECTED);
        response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSPECIFIED);
        response->set_reason("AGC 控制组处于待删除状态");
        LOG_WARNING("AGC 拒绝控制状态命令: group_name={}, tag={}, 原因=控制组处于待删除状态", groupName, request.dst().tag());
        return grpc::Status::OK;
      }
      if (!request.value().has_bool_value()) {
        response->set_status(DataCenterProto::COMMAND_REJECTED);
        response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSUPPORTED_POINT);
        response->set_reason("AGC 控制点仅接受 BOOL 类型");
        LOG_WARNING("AGC 拒绝控制状态命令: group_name={}, tag={}, 原因=控制点仅接受 BOOL 类型", groupName, request.dst().tag());
        return grpc::Status::OK;
      }
      const bool value = request.value().bool_value();
      if (isFunctionPoint && !it->second.remoteEnabled) {
        response->set_status(DataCenterProto::COMMAND_REJECTED);
        response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSPECIFIED);
        response->set_reason("AGC 当前不允许远方操作");
        LOG_WARNING("AGC 拒绝功能投入命令: group_name={}, value={}, 原因=AGC 当前不允许远方操作", groupName, value);
        return grpc::Status::OK;
      }
      if (isFunctionPoint) {
        if (it->second.functionEnabled != value) {
          it->second.functionEnabled = value;
          publishState = true;
          requestControl = value && it->second.state == AGCProto::GROUP_STATE_RUNNING;
        }
      } else if (it->second.remoteEnabled != value) {
        it->second.remoteEnabled = value;
        publishState = true;
      }
      response->set_status(DataCenterProto::COMMAND_ACCEPTED);
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSPECIFIED);
      response->set_requested_value(value ? 1.0 : 0.0);
      response->set_accepted_value(value ? 1.0 : 0.0);
      response->set_reason(isFunctionPoint ? "AGC 功能投入状态已更新" : "AGC 远方操作状态已更新");
      LOG_INFO("AGC 已接受控制状态命令: group_name={}, tag={}, value={}, 状态已变化={}", groupName, request.dst().tag(), value, publishState);
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

  double raw = 0.0;
  if (!pointValueToDouble(request.value(), &raw)) {
    response->set_status(DataCenterProto::COMMAND_REJECTED);
    response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSUPPORTED_POINT);
    response->set_reason("命令点值类型不支持");
    return grpc::Status::OK;
  }

  AGCProto::GroupConfig config;
  uint32_t connId = 0;
  ControlInput input;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      response->set_status(DataCenterProto::COMMAND_REJECTED);
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_BAD_CONFIG);
      response->set_reason("未找到 AGC 控制组");
      return grpc::Status::OK;
    }
    if (it->second.state != AGCProto::GROUP_STATE_RUNNING) {
      response->set_status(DataCenterProto::COMMAND_REJECTED);
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_GROUP_NOT_RUNNING);
      response->set_reason("AGC 控制组未运行");
      return grpc::Status::OK;
    }
    if (it->second.tuningStatus.state() == AGCProto::TUNING_STATE_RUNNING) {
      response->set_status(DataCenterProto::COMMAND_REJECTED);
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSPECIFIED);
      response->set_reason("AGC 正在自动调试，暂不接受正式总目标命令");
      return grpc::Status::OK;
    }
    if (!it->second.remoteEnabled) {
      response->set_status(DataCenterProto::COMMAND_REJECTED);
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSPECIFIED);
      response->set_reason("AGC 当前不允许远方操作");
      LOG_WARNING("AGC 拒绝同步总有功命令: group_name={}, 原因=AGC 当前不允许远方操作", groupName);
      return grpc::Status::OK;
    }
    if (!it->second.functionEnabled) {
      response->set_status(DataCenterProto::COMMAND_REJECTED);
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSPECIFIED);
      response->set_reason("AGC 功能未投入");
      LOG_WARNING("AGC 拒绝同步总有功命令: group_name={}, 原因=AGC 功能未投入", groupName);
      return grpc::Status::OK;
    }
    if (it->second.cmdTag.empty() || request.dst().tag() != it->second.cmdTag) {
      response->set_status(DataCenterProto::COMMAND_REJECTED);
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSUPPORTED_POINT);
      response->set_reason("目的点不是 AGC 总有功命令点");
      return grpc::Status::OK;
    }

    config = it->second.config;
    connId = it->second.connId;
    input.hasCmdRaw = true;
    input.cmdRaw = raw;
    input.baseRawByTag = it->second.baseRawByTag;
    input.hasMemberMeasRaw = it->second.hasMemberMeasRaw;
    input.memberMeasRaw = it->second.memberMeasRaw;
    input.hasLastMemberTargetKw = it->second.hasLastMemberTargetKw;
    input.lastMemberTargetKw = it->second.lastMemberTargetKw;
    input.hasLastDesiredTotalKw = it->second.hasLastDesiredTotalKw;
    input.lastDesiredTotalKw = it->second.lastDesiredTotalKw;
    input.controlProfile = it->second.controlProfile;
    input.integralMemoryKw = it->second.integralMemoryKw;
  }

  const auto defaultOutput = ComputeDefaultPointOutput(config, input);
  response->set_lower_limit(defaultOutput.dynamicLowerKw);
  response->set_upper_limit(defaultOutput.dynamicUpperKw);

  auto outputOpt = ComputeControlOutput(config, input, weightedStrategy_);
  if (!outputOpt) {
    response->set_status(DataCenterProto::COMMAND_REJECTED);
    response->set_reject_code(DataCenterProto::COMMAND_REJECT_BAD_CONFIG);
    response->set_reason("AGC 控制计算无法生成输出");
    return grpc::Status::OK;
  }
  const auto &output = *outputOpt;
  response->set_requested_value(output.desiredTotalKw);
  response->set_accepted_value(output.actualTargetKw);

  constexpr double kEps = 1e-6;
  if (std::fabs(output.unallocatedKw) > kEps) {
    response->set_status(DataCenterProto::COMMAND_REJECTED);
    if (output.unallocatedKw > 0.0) {
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_OVER_UPPER_LIMIT);
      response->set_reason("总有功目标超过当前可调上限");
    } else {
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_BELOW_LOWER_LIMIT);
      response->set_reason("总有功目标低于当前可调下限");
    }
    LOG_WARNING("AGC 拒绝同步总有功命令: group_name={}, requested_kw={}, lower_kw={}, upper_kw={}, actual_target_kw={}, unallocated_kw={}",
                groupName,
                output.desiredTotalKw,
                defaultOutput.dynamicLowerKw,
                defaultOutput.dynamicUpperKw,
                output.actualTargetKw,
                output.unallocatedKw);
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
    if (it == groupsByName_.end() || it->second.state != AGCProto::GROUP_STATE_RUNNING) {
      response->set_status(DataCenterProto::COMMAND_REJECTED);
      response->set_reject_code(DataCenterProto::COMMAND_REJECT_GROUP_NOT_RUNNING);
      response->set_reason("AGC 控制组状态已变化，命令未执行");
      return grpc::Status::OK;
    }
    it->second.cmdRaw = raw;
    it->second.hasCmdRaw = true;
    it->second.hasLastDesiredTotalKw = output.hasLastDesiredTotalKw;
    it->second.lastDesiredTotalKw = output.nextLastDesiredTotalKw;
    it->second.hasLastMemberTargetKw = output.hasLastMemberTargetKw;
    it->second.lastMemberTargetKw = output.nextLastMemberTargetKw;
    it->second.integralMemoryKw = output.nextIntegralMemoryKw;
    it->second.hasLastUnallocatedKw = false;
    it->second.lastUnallocatedKw = 0.0;
  }

  publishCommandEchoPoint(connId, config.p_cmd(), commandUpdate);
  publishDefaultLimitPoints(groupName, "同步命令执行");

  const auto quality = DataCenterProto::QUALITY_GOOD;
  if (config.has_outputs()) {
    const auto &outputs = config.outputs();
    if (output.publishTotalMeas) {
      (void)dataCenter_.PublishDouble(connId, outputs.p_total_meas().tag(), output.totalMeasKw, quality, 0);
    }
    if (output.publishTotalTarget) {
      (void)dataCenter_.PublishDouble(connId, outputs.p_total_target().tag(), output.actualTargetKw, quality, 0);
    }
    if (output.publishTotalError) {
      (void)dataCenter_.PublishDouble(connId, outputs.p_total_error().tag(), output.totalErrorKw, quality, 0);
    }
  }

  const auto memberCount = static_cast<size_t>(config.members_size());
  for (size_t i = 0; i < memberCount && i < output.memberPublish.size() && i < output.memberPublishKw.size(); ++i) {
    if (!output.memberPublish[i]) {
      continue;
    }
    const auto &member = config.members(static_cast<int>(i));
    if (!member.has_p_set() || !member.p_set().has_signal() || member.p_set().signal().tag().empty()) {
      continue;
    }
    (void)dataCenter_.PublishDouble(connId, member.p_set().signal().tag(), output.memberPublishKw[i], quality, 0);
  }

  response->set_status(DataCenterProto::COMMAND_ACCEPTED);
  response->set_reject_code(DataCenterProto::COMMAND_REJECT_UNSPECIFIED);
  response->set_reason("AGC 同步总有功命令已接受并执行");
  LOG_INFO("AGC 已执行同步总有功命令: group_name={}, requested_kw={}, actual_target_kw={}",
           groupName,
           output.desiredTotalKw,
           output.actualTargetKw);
  return grpc::Status::OK;
}

bool GroupManager::pointValueToDouble(const DataCenterProto::PointValue &v, double *out) {
  if (out == nullptr) {
    return false;
  }
  switch (v.kind_case()) {
  case DataCenterProto::PointValue::kDoubleValue:
    *out = v.double_value();
    return true;
  case DataCenterProto::PointValue::kIntValue:
    *out = static_cast<double>(v.int_value());
    return true;
  case DataCenterProto::PointValue::kBoolValue:
    *out = v.bool_value() ? 1.0 : 0.0;
    return true;
  default:
    return false;
  }
}

std::unordered_set<std::string> GroupManager::collectAllTags(const AGCProto::GroupConfig &config) {
  std::unordered_set<std::string> tags;
  for (const auto &point : DefaultPointDefinitions()) {
    tags.emplace(point.tag);
  }
  if (config.has_p_cmd() && config.p_cmd().has_signal() && !config.p_cmd().signal().tag().empty()) {
    tags.emplace(config.p_cmd().signal().tag());
    if (config.p_cmd().mode() == AGCProto::VALUE_MODE_DELTA && config.p_cmd().delta_base() == AGCProto::DELTA_BASE_BASE_TAG &&
        !config.p_cmd().base_tag().empty()) {
      tags.emplace(config.p_cmd().base_tag());
    }
  }

  if (config.has_outputs()) {
    const auto &o = config.outputs();
    if (o.has_p_total_meas() && !o.p_total_meas().tag().empty()) {
      tags.emplace(o.p_total_meas().tag());
    }
    if (o.has_p_total_target() && !o.p_total_target().tag().empty()) {
      tags.emplace(o.p_total_target().tag());
    }
    if (o.has_p_total_error() && !o.p_total_error().tag().empty()) {
      tags.emplace(o.p_total_error().tag());
    }
  }

  for (const auto &m : config.members()) {
    if (m.has_p_meas() && !m.p_meas().tag().empty()) {
      tags.emplace(m.p_meas().tag());
    }
    if (m.has_p_set() && m.p_set().has_signal() && !m.p_set().signal().tag().empty()) {
      tags.emplace(m.p_set().signal().tag());
      if (m.p_set().mode() == AGCProto::VALUE_MODE_DELTA && m.p_set().delta_base() == AGCProto::DELTA_BASE_BASE_TAG &&
          !m.p_set().base_tag().empty()) {
        tags.emplace(m.p_set().base_tag());
      }
    }
  }
  return tags;
}

void GroupManager::rebuildTagCache(GroupRuntime *g) {
  if (g == nullptr) {
    return;
  }
  g->cmdTag.clear();
  g->memberIndexByMeasTag.clear();
  g->baseTags.clear();
  g->subscribeTags.clear();

  const auto memberCount = static_cast<size_t>(g->config.members_size());
  g->hasMemberMeasRaw.assign(memberCount, false);
  g->memberMeasRaw.assign(memberCount, 0.0);
  g->hasLastMemberTargetKw.assign(memberCount, false);
  g->lastMemberTargetKw.assign(memberCount, 0.0);
  g->hasLastControlMemberMeasKw.assign(memberCount, false);
  g->lastControlMemberMeasKw.assign(memberCount, 0.0);

  if (g->config.has_p_cmd() && g->config.p_cmd().has_signal()) {
    g->cmdTag = g->config.p_cmd().signal().tag();
    if (!g->cmdTag.empty()) {
      g->subscribeTags.emplace_back(g->cmdTag);
    }
    if (g->config.p_cmd().mode() == AGCProto::VALUE_MODE_DELTA && g->config.p_cmd().delta_base() == AGCProto::DELTA_BASE_BASE_TAG &&
        !g->config.p_cmd().base_tag().empty()) {
      g->baseTags.emplace(g->config.p_cmd().base_tag());
    }
  }

  for (int i = 0; i < g->config.members_size(); ++i) {
    const auto &m = g->config.members(i);
    if (m.has_p_meas() && !m.p_meas().tag().empty()) {
      g->memberIndexByMeasTag.emplace(m.p_meas().tag(), static_cast<size_t>(i));
      g->subscribeTags.emplace_back(m.p_meas().tag());
    }
    if (m.has_p_set() && m.p_set().mode() == AGCProto::VALUE_MODE_DELTA && m.p_set().delta_base() == AGCProto::DELTA_BASE_BASE_TAG &&
        !m.p_set().base_tag().empty()) {
      g->baseTags.emplace(m.p_set().base_tag());
    }
  }

  for (const auto &t : g->baseTags) {
    g->subscribeTags.emplace_back(t);
  }
}

void GroupManager::primeControlInputs(const std::string &groupName) {
  uint32_t connId = 0;
  std::vector<std::string> tags;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end() || it->second.state != AGCProto::GROUP_STATE_RUNNING) {
      return;
    }
    connId = it->second.connId;
    tags = it->second.subscribeTags;
  }

  if (connId == 0 || tags.empty()) {
    LOG_DEBUG("AGC 启动控制组时跳过初始输入快照加载: group_name={}, conn_id={}, 标签数={}", groupName, connId, tags.size());
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
    LOG_WARNING("AGC 启动控制组时读取初始输入快照失败: group_name={}, conn_id={}, 标签数={}, 原因={}", groupName, connId, tags.size(), status.error_message());
    return;
  }

  size_t changed = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end() || it->second.state != AGCProto::GROUP_STATE_RUNNING) {
      return;
    }
    for (const auto &update : resp.updates()) {
      if (handleUpdateLocked(&it->second, update)) {
        ++changed;
      }
    }
    if (resp.updates_size() > 0) {
      requestControlLocked(groupName, &it->second, "启动时加载初始输入快照", "");
    }
  }

  LOG_INFO("AGC 启动控制组时已加载初始输入快照: group_name={}, conn_id={}, updates={}, changed={}", groupName, connId, resp.updates_size(), changed);
}

void GroupManager::requestControlLocked(
    const std::string &groupName, GroupRuntime *g, std::string_view reason, std::string_view tag) {
  if (g == nullptr || g->state != AGCProto::GROUP_STATE_RUNNING || !g->controlTrigger) {
    return;
  }
  if (!g->controlTrigger->pending.exchange(true)) {
    g->controlTrigger->signal.release();
    if (tag.empty()) {
      LOG_DEBUG("AGC 控制组已请求一次事件触发控制: group_name={}, 原因={}", groupName, reason);
    } else {
      LOG_DEBUG("AGC 控制组已请求一次事件触发控制: group_name={}, 原因={}, tag={}", groupName, reason, tag);
    }
  }
}

void GroupManager::publishControlStatePoints(const std::string &groupName, std::string_view trigger) {
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

  const auto functionTag = defaultPointTag(AGCProto::DEFAULT_POINT_KIND_FUNCTION_ENABLE);
  const auto remoteTag = defaultPointTag(AGCProto::DEFAULT_POINT_KIND_REMOTE_OPERATION);
  DataCenterProto::PointValue functionValue;
  functionValue.set_bool_value(functionEnabled);
  auto status = dataCenter_.PublishValue(connId, std::string(functionTag), functionValue, DataCenterProto::QUALITY_GOOD, 0);
  if (!status.ok()) {
    LOG_ERROR("AGC 发布功能投入状态失败: group_name={}, conn_id={}, tag={}, value={}, 触发来源={}, 原因={}",
              groupName,
              connId,
              functionTag,
              functionEnabled,
              trigger,
              status.error_message());
  } else {
    LOG_DEBUG("AGC 已发布功能投入状态: group_name={}, conn_id={}, tag={}, value={}, 触发来源={}",
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
    LOG_ERROR("AGC 发布远方操作状态失败: group_name={}, conn_id={}, tag={}, value={}, 触发来源={}, 原因={}",
              groupName,
              connId,
              remoteTag,
              remoteEnabled,
              trigger,
              status.error_message());
  } else {
    LOG_DEBUG("AGC 已发布远方操作状态: group_name={}, conn_id={}, tag={}, value={}, 触发来源={}",
              groupName,
              connId,
              remoteTag,
              remoteEnabled,
              trigger);
  }
}

bool GroupManager::handleUpdateLocked(GroupRuntime *g, const DataCenterProto::PointUpdate &update) {
  if (g == nullptr) {
    return false;
  }
  const auto &tag = update.dst_tag();
  double raw = 0.0;
  if (!pointValueToDouble(update.value(), &raw)) {
    return false;
  }

  if (!g->cmdTag.empty() && tag == g->cmdTag) {
    const auto changed = !g->hasCmdRaw || !sameValue(g->cmdRaw, raw);
    g->cmdRaw = raw;
    g->hasCmdRaw = true;
    if (changed) {
      std::fill(g->integralMemoryKw.begin(), g->integralMemoryKw.end(), 0.0);
      LOG_DEBUG("AGC 收到新的总目标，已清空本次运行积分记忆: group_name={}, tag={}", g->config.group_name(), tag);
    }
    return changed;
  }

  if (g->baseTags.contains(tag)) {
    auto it = g->baseRawByTag.find(tag);
    const auto changed = (it == g->baseRawByTag.end()) || !sameValue(it->second, raw);
    g->baseRawByTag[tag] = raw;
    return changed;
  }

  auto it = g->memberIndexByMeasTag.find(tag);
  if (it != g->memberIndexByMeasTag.end()) {
    const auto idx = it->second;
    if (idx < g->memberMeasRaw.size()) {
      const auto changed = !g->hasMemberMeasRaw[idx] || !sameValue(g->memberMeasRaw[idx], raw);
      g->memberMeasRaw[idx] = raw;
      g->hasMemberMeasRaw[idx] = true;
      return changed;
    }
    return false;
  }
  return false;
}

void GroupManager::publishDefaultLimitPoints(const std::string &groupName, std::string_view trigger) {
  AGCProto::GroupConfig config;
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
    input.hasMemberMeasRaw = it->second.hasMemberMeasRaw;
    input.memberMeasRaw = it->second.memberMeasRaw;
  }
  if (connId == 0) {
    return;
  }

  const auto defaultOutput = ComputeDefaultPointOutput(config, input);
  const auto theoreticalLowerTag = defaultPointTag(AGCProto::DEFAULT_POINT_KIND_THEORETICAL_LOWER);
  const auto theoreticalUpperTag = defaultPointTag(AGCProto::DEFAULT_POINT_KIND_THEORETICAL_UPPER);
  const auto dynamicLowerTag = defaultPointTag(AGCProto::DEFAULT_POINT_KIND_DYNAMIC_LOWER);
  const auto dynamicUpperTag = defaultPointTag(AGCProto::DEFAULT_POINT_KIND_DYNAMIC_UPPER);
  const auto installedCapacityTag = defaultPointTag(AGCProto::DEFAULT_POINT_KIND_INSTALLED_CAPACITY);
  const auto theoreticalQuality = DataCenterProto::QUALITY_GOOD;

  const auto installedCapacityKw = ComputeInstalledCapacityKw(config);
  auto status = dataCenter_.PublishDouble(connId, std::string(installedCapacityTag), installedCapacityKw, theoreticalQuality, 0);
  if (!status.ok()) {
    LOG_ERROR("AGC 发布装机容量失败: group_name={}, tag={}, 触发来源={}, capacity_kw={}, 原因={}",
              groupName,
              installedCapacityTag,
              trigger,
              installedCapacityKw,
              status.error_message());
  } else {
    LOG_DEBUG("AGC 已发布装机容量: group_name={}, tag={}, 触发来源={}, capacity_kw={}",
              groupName,
              installedCapacityTag,
              trigger,
              installedCapacityKw);
  }

  status = dataCenter_.PublishDouble(connId, std::string(theoreticalLowerTag), defaultOutput.theoreticalLowerKw, theoreticalQuality, 0);
  if (!status.ok()) {
    LOG_ERROR("AGC 发布默认点失败: group_name={}, tag={}, 触发来源={}, 原因={}", groupName, theoreticalLowerTag, trigger, status.error_message());
  }
  status = dataCenter_.PublishDouble(connId, std::string(theoreticalUpperTag), defaultOutput.theoreticalUpperKw, theoreticalQuality, 0);
  if (!status.ok()) {
    LOG_ERROR("AGC 发布默认点失败: group_name={}, tag={}, 触发来源={}, 原因={}", groupName, theoreticalUpperTag, trigger, status.error_message());
  }
  status = dataCenter_.PublishDouble(connId, std::string(dynamicLowerTag), defaultOutput.dynamicLowerKw, defaultOutput.dynamicQuality, 0);
  if (!status.ok()) {
    LOG_ERROR("AGC 发布默认点失败: group_name={}, tag={}, 触发来源={}, 原因={}", groupName, dynamicLowerTag, trigger, status.error_message());
  }
  status = dataCenter_.PublishDouble(connId, std::string(dynamicUpperTag), defaultOutput.dynamicUpperKw, defaultOutput.dynamicQuality, 0);
  if (!status.ok()) {
    LOG_ERROR("AGC 发布默认点失败: group_name={}, tag={}, 触发来源={}, 原因={}", groupName, dynamicUpperTag, trigger, status.error_message());
  }
  LOG_DEBUG(
      "AGC 已发布默认限值点: group_name={}, 触发来源={}, 理论下限={}, 理论上限={}, 当前下限={}, 当前上限={}, 当前质量={}, 不可控成员数={}, 缺测不可控成员数={}",
      groupName,
      trigger,
      defaultOutput.theoreticalLowerKw,
      defaultOutput.theoreticalUpperKw,
      defaultOutput.dynamicLowerKw,
      defaultOutput.dynamicUpperKw,
      static_cast<int>(defaultOutput.dynamicQuality),
      defaultOutput.uncontrollableMemberCount,
      defaultOutput.missingUncontrollableMemberCount);
}

void GroupManager::publishCommandEchoPoint(
    uint32_t connId, const AGCProto::ValueSpec &commandSpec, const DataCenterProto::PointUpdate &update) {
  const auto commandEchoTag = defaultPointTag(AGCProto::DEFAULT_POINT_KIND_COMMAND_ECHO);
  double value = 0.0;
  if (!pointValueToDouble(update.value(), &value)) {
    LOG_WARNING("AGC 发布调节返回值跳过: conn_id={}, tag={}, 原因=命令点值类型不支持", connId, commandEchoTag);
    return;
  }

  const auto echoValue = commandEchoEngineeringValue(commandSpec, value);
  auto status = dataCenter_.PublishDouble(connId, std::string(commandEchoTag), echoValue, update.quality(), update.ts_ms());
  if (!status.ok()) {
    LOG_ERROR("AGC 发布调节返回值失败: conn_id={}, tag={}, 原因={}", connId, commandEchoTag, status.error_message());
  } else {
    LOG_DEBUG(
        "AGC 已发布调节返回值: conn_id={}, tag={}, value={}, echo_kw={}, 质量={}, ts_ms={}",
        connId,
        commandEchoTag,
        value,
        echoValue,
        static_cast<int>(update.quality()),
        update.ts_ms());
  }
}

void GroupManager::controlTick(const std::string &groupName) {
  AGCProto::GroupConfig config;
  uint32_t connId = 0;
  bool functionEnabled = true;
  ControlInput input;

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      return;
    }
    if (it->second.state != AGCProto::GROUP_STATE_RUNNING) {
      return;
    }

    config = it->second.config;
    connId = it->second.connId;
    functionEnabled = it->second.functionEnabled;
    input.hasCmdRaw = it->second.hasCmdRaw;
    input.cmdRaw = it->second.cmdRaw;
    input.baseRawByTag = it->second.baseRawByTag;
    input.hasMemberMeasRaw = it->second.hasMemberMeasRaw;
    input.memberMeasRaw = it->second.memberMeasRaw;
    input.hasLastMemberTargetKw = it->second.hasLastMemberTargetKw;
    input.lastMemberTargetKw = it->second.lastMemberTargetKw;
    input.hasLastDesiredTotalKw = it->second.hasLastDesiredTotalKw;
    input.lastDesiredTotalKw = it->second.lastDesiredTotalKw;
    input.controlProfile = it->second.controlProfile;
    input.integralMemoryKw = it->second.integralMemoryKw;
    const auto now = std::chrono::steady_clock::now();
    if (it->second.lastControlTickAt.time_since_epoch().count() > 0) {
      input.controlPeriodSeconds = std::clamp(
          std::chrono::duration_cast<std::chrono::duration<double>>(now - it->second.lastControlTickAt).count(), 0.05, 30.0);
    }
    it->second.lastControlTickAt = now;
    bool hasComparableMeasurement = false;
    bool allMembersDeclining = true;
    bool hasMeasurement = false;
    bool allMembersNearZero = true;
    for (int i = 0; i < config.members_size(); ++i) {
      const auto &member = config.members(i);
      if (!member.controllable() || static_cast<size_t>(i) >= it->second.memberMeasRaw.size() ||
          static_cast<size_t>(i) >= it->second.hasMemberMeasRaw.size() || !it->second.hasMemberMeasRaw[static_cast<size_t>(i)]) {
        continue;
      }
      const auto scale = member.p_meas().scale() == 0.0 ? 1.0 : member.p_meas().scale();
      const auto measuredKw = it->second.memberMeasRaw[static_cast<size_t>(i)] * scale + member.p_meas().offset();
      hasMeasurement = true;
      allMembersNearZero = allMembersNearZero && measuredKw <= std::max(1.0, member.capacity_kw() * 0.05);
      if (static_cast<size_t>(i) < it->second.hasLastControlMemberMeasKw.size() &&
          it->second.hasLastControlMemberMeasKw[static_cast<size_t>(i)]) {
        hasComparableMeasurement = true;
        if (measuredKw > it->second.lastControlMemberMeasKw[static_cast<size_t>(i)] - 0.5) {
          allMembersDeclining = false;
        }
      } else {
        allMembersDeclining = false;
      }
      if (static_cast<size_t>(i) < it->second.lastControlMemberMeasKw.size()) {
        it->second.lastControlMemberMeasKw[static_cast<size_t>(i)] = measuredKw;
        it->second.hasLastControlMemberMeasKw[static_cast<size_t>(i)] = true;
      }
    }
    input.integralEnabled = !(hasMeasurement &&
                              ((hasComparableMeasurement && allMembersDeclining) || allMembersNearZero));
    if (!input.integralEnabled) {
      LOG_DEBUG("AGC 暂停积分修正: group_name={}, 原因=检测到全体成员共同下降或接近零出力，疑似光照资源下降", groupName);
    }
    if (it->second.tuningStatus.state() == AGCProto::TUNING_STATE_RUNNING) {
      input.controlProfile = it->second.tuningStatus.candidate_profile();
      input.hasDesiredTotalOverride = true;
      input.desiredTotalOverrideKw = it->second.tuningStatus.current_target_kw();
    }
  }

  if (connId == 0) {
    return;
  }
  const auto quality = DataCenterProto::QUALITY_GOOD;
  double totalMeasKw = 0.0;
  if (const auto totalMeasPublishKw = ComputeTotalMeasKw(config, input, &totalMeasKw)) {
    auto status = dataCenter_.PublishDouble(connId, config.outputs().p_total_meas().tag(), *totalMeasPublishKw, quality, 0);
    if (!status.ok()) {
      LOG_ERROR(
          "AGC 发布总实时测量值失败: group_name={}, conn_id={}, tag={}, total_meas_kw={}, 原因={}",
          groupName,
          connId,
          config.outputs().p_total_meas().tag(),
          totalMeasKw,
          status.error_message());
    } else {
      LOG_DEBUG(
          "AGC 已发布总实时测量值: group_name={}, conn_id={}, tag={}, total_meas_kw={}, publish_kw={}",
          groupName,
          connId,
          config.outputs().p_total_meas().tag(),
          totalMeasKw,
          *totalMeasPublishKw);
    }
  }
  publishDefaultLimitPoints(groupName, "事件触发控制");

  if (!functionEnabled) {
    LOG_DEBUG("AGC 跳过控制输出: group_name={}, 原因=AGC 功能未投入", groupName);
    return;
  }

  const auto outputOpt = ComputeControlOutput(config, input, weightedStrategy_);
  if (!outputOpt) {
    return;
  }
  const auto &output = *outputOpt;

  if (config.has_outputs()) {
    const auto &o = config.outputs();
    if (output.publishTotalTarget) {
      (void)dataCenter_.PublishDouble(connId, o.p_total_target().tag(), output.actualTargetKw, quality, 0);
    }
    if (output.publishTotalError) {
      (void)dataCenter_.PublishDouble(connId, o.p_total_error().tag(), output.totalErrorKw, quality, 0);
    }
  }

  // 下发成员设定值。
  const auto memberCount = static_cast<size_t>(config.members_size());
  for (size_t i = 0; i < memberCount && i < output.memberPublish.size() && i < output.memberPublishKw.size(); ++i) {
    if (!output.memberPublish[i]) {
      continue;
    }
    const auto &m = config.members(static_cast<int>(i));
    if (!m.has_p_set() || !m.p_set().has_signal() || m.p_set().signal().tag().empty()) {
      continue;
    }
    (void)dataCenter_.PublishDouble(connId, m.p_set().signal().tag(), output.memberPublishKw[i], quality, 0);
  }

  // 下发后更新状态（尽力而为）。
  bool shouldLogUnallocated = false;
  const auto unallocatedKw = output.unallocatedKw;
  const auto targetControllableKw = output.targetControllableKw;
  const auto passiveKw = output.passiveKw;
  const auto desiredTotalKw = output.desiredTotalKw;
  const auto actualTargetKw = output.actualTargetKw;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      return;
    }

    it->second.hasLastDesiredTotalKw = output.hasLastDesiredTotalKw;
    it->second.lastDesiredTotalKw = output.nextLastDesiredTotalKw;
    it->second.hasLastMemberTargetKw = output.hasLastMemberTargetKw;
    it->second.lastMemberTargetKw = output.nextLastMemberTargetKw;
    it->second.integralMemoryKw = output.nextIntegralMemoryKw;

    constexpr double kEps = 1e-6;
    if (std::fabs(unallocatedKw) > kEps) {
      if (!it->second.hasLastUnallocatedKw || std::fabs(unallocatedKw - it->second.lastUnallocatedKw) > kEps) {
        shouldLogUnallocated = true;
        it->second.hasLastUnallocatedKw = true;
        it->second.lastUnallocatedKw = unallocatedKw;
      }
    } else {
      it->second.hasLastUnallocatedKw = false;
      it->second.lastUnallocatedKw = 0.0;
    }
  }

  if (shouldLogUnallocated) {
    LOG_WARNING(
        "AGC 分配受限: group_name={}, unallocated_kw={}, target_controllable_kw={}, passive_kw={}, desired_total_kw={}, actual_target_kw={}",
        groupName,
        unallocatedKw,
        targetControllableKw,
        passiveKw,
        desiredTotalKw,
        actualTargetKw);
  }
}

grpc::Status GroupManager::GetControlProfile(
    const std::string &groupName, AGCProto::GroupControlProfile *out) const {
  if (out == nullptr) {
    return makeInvalid("out 为空");
  }
  auto status = validateGroupName(groupName);
  if (!status.ok()) {
    return status;
  }
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = groupsByName_.find(groupName);
  if (it == groupsByName_.end()) {
    return makeNotFound(groupName);
  }
  *out = makeDefaultControlProfileLocked(it->second);
  return grpc::Status::OK;
}

grpc::Status GroupManager::ConfirmControlProfile(
    const AGCProto::GroupControlProfile &profile, AGCProto::GroupControlProfile *out) {
  if (out == nullptr) {
    return makeInvalid("out 为空");
  }
  auto status = validateGroupName(profile.group_name());
  if (!status.ok()) {
    return status;
  }

  AGCProto::ControlProfilesConfig validation;
  *validation.add_profiles() = profile;
  status = ValidateControlProfilesConfig(validation);
  if (!status.ok()) {
    return status;
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto groupIt = groupsByName_.find(profile.group_name());
  if (groupIt == groupsByName_.end()) {
    return makeNotFound(profile.group_name());
  }
  if (groupIt->second.state == AGCProto::GROUP_STATE_RUNNING) {
    return makePreconditionFailed("确认固定控制参数前请先停止控制组");
  }

  std::unordered_set<std::string> controllableNames;
  for (const auto &member : groupIt->second.config.members()) {
    if (member.controllable()) {
      controllableNames.emplace(member.member_name());
    }
  }
  if (profile.members_size() != static_cast<int>(controllableNames.size())) {
    return makeInvalid("固定控制参数必须覆盖当前控制组全部可控成员");
  }
  std::unordered_set<std::string> profileNames;
  for (const auto &member : profile.members()) {
    if (!controllableNames.contains(member.member_name()) || !profileNames.emplace(member.member_name()).second) {
      return makeInvalid(std::format("固定控制参数成员与当前控制组不匹配: member_name={}", member.member_name()));
    }
  }

  const auto oldProfileIt = controlProfilesByGroup_.find(profile.group_name());
  const bool hadOldProfile = oldProfileIt != controlProfilesByGroup_.end();
  const auto oldRuntimeProfile = groupIt->second.controlProfile;
  const auto oldProfile = !hadOldProfile
                              ? AGCProto::GroupControlProfile{}
                              : oldProfileIt->second;
  auto confirmed = profile;
  confirmed.set_group_name(profile.group_name());
  confirmed.set_version(oldProfile.version() + 1);
  confirmed.set_confirmed_at_ms(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count()));
  for (auto &member : *confirmed.mutable_members()) {
    member.set_version(confirmed.version());
    member.set_confirmed_at_ms(confirmed.confirmed_at_ms());
  }
  controlProfilesByGroup_[profile.group_name()] = confirmed;
  groupIt->second.controlProfile = makeDefaultControlProfileLocked(groupIt->second);
  std::fill(groupIt->second.integralMemoryKw.begin(), groupIt->second.integralMemoryKw.end(), 0.0);

  status = saveControlProfilesLocked();
  if (!status.ok()) {
    if (!hadOldProfile) {
      controlProfilesByGroup_.erase(profile.group_name());
    } else {
      controlProfilesByGroup_[profile.group_name()] = oldProfile;
    }
    groupIt->second.controlProfile = oldRuntimeProfile;
    return status;
  }
  *out = groupIt->second.controlProfile;
  LOG_INFO("AGC 已确认并保存固定控制参数: group_name={}, version={}, 成员数={}",
           profile.group_name(), confirmed.version(), confirmed.members_size());
  return grpc::Status::OK;
}

grpc::Status GroupManager::StartTuning(
    const AGCProto::StartTuningRequest &request, AGCProto::TuningStatus *out) {
  if (out == nullptr) {
    return makeInvalid("out 为空");
  }
  auto status = validateGroupName(request.group_name());
  if (!status.ok()) {
    return status;
  }
  if (!request.has_config()) {
    return makeInvalid("调试配置不能为空");
  }
  status = validateTuningConfig(request.config());
  if (!status.ok()) {
    return status;
  }

  std::unique_lock<std::mutex> lock(mu_);
  auto it = groupsByName_.find(request.group_name());
  if (it == groupsByName_.end()) {
    return makeNotFound(request.group_name());
  }
  if (it->second.state == AGCProto::GROUP_STATE_RUNNING) {
    return makePreconditionFailed("启动调试前请先停止正式控制组");
  }
  status = checkStartPreconditionsLocked(it->second);
  if (!status.ok()) {
    return status;
  }
  double installedCapacityKw = 0.0;
  for (const auto &member : it->second.config.members()) {
    installedCapacityKw += member.capacity_kw();
  }
  const auto maximumToleranceKw = std::min(300.0, installedCapacityKw * 0.003);
  if (request.config().target_lower_kw() < 0.0 || request.config().target_upper_kw() > installedCapacityKw) {
    return makeInvalid("调试目标范围必须落在控制组装机容量范围内");
  }
  if (request.config().total_tolerance_kw() > maximumToleranceKw) {
    return makeInvalid(std::format("调试总量精度不能超过 300kW 或装机容量 0.3%% 中的较小值: 最大允许={}kW", maximumToleranceKw));
  }
  if (it->second.tuningStatus.state() == AGCProto::TUNING_STATE_RUNNING) {
    return makePreconditionFailed("该控制组已有调试任务在运行");
  }
  if (it->second.tuningThread.joinable() || it->second.controlThread.joinable() || it->second.dcSubscribeThread.joinable()) {
    return makePreconditionFailed("上一次调试任务尚未完成线程清理，请先停止调试任务");
  }

  const auto groupName = request.group_name();
  it->second.tuningConfig = request.config();
  it->second.state = AGCProto::GROUP_STATE_RUNNING;
  it->second.tuningStatus.Clear();
  it->second.tuningStatus.set_group_name(groupName);
  it->second.tuningStatus.set_state(AGCProto::TUNING_STATE_RUNNING);
  it->second.tuningStatus.set_direction(AGCProto::TUNING_DIRECTION_UP);
  it->second.tuningStatus.set_started_at_ms(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count()));
  it->second.tuningStatus.set_current_target_kw(request.config().target_upper_kw());
  *it->second.tuningStatus.mutable_candidate_profile() = makeDefaultControlProfileLocked(it->second);
  it->second.tuningPhaseStartedAt = std::chrono::steady_clock::now();
  it->second.tuningTaskStartedAt = it->second.tuningPhaseStartedAt;
  it->second.tuningEnteredRangeAt = {};
  it->second.tuningInRange = false;
  startThreadsLocked(groupName, &it->second);
  it->second.tuningThread = ModuleManager::StartModuleThread(
      AGCLibInfo.LIB_NAME,
      [this, groupName](std::stop_token st) {
        while (!st.stop_requested()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
          bool finish = false;
          {
            std::lock_guard<std::mutex> stateLock(mu_);
            auto groupIt = groupsByName_.find(groupName);
            if (groupIt == groupsByName_.end() || groupIt->second.tuningStatus.state() != AGCProto::TUNING_STATE_RUNNING) {
              break;
            }
            auto &group = groupIt->second;
            const auto now = std::chrono::steady_clock::now();
            const auto elapsedSeconds = std::chrono::duration_cast<std::chrono::duration<double>>(now - group.tuningPhaseStartedAt).count();
            const auto totalElapsedSeconds = std::chrono::duration_cast<std::chrono::duration<double>>(
                now - group.tuningTaskStartedAt).count();
            double totalMeasKw = 0.0;
            for (int i = 0; i < group.config.members_size(); ++i) {
              if (static_cast<size_t>(i) >= group.memberMeasRaw.size() ||
                  static_cast<size_t>(i) >= group.hasMemberMeasRaw.size() || !group.hasMemberMeasRaw[static_cast<size_t>(i)]) {
                continue;
              }
              const auto &signal = group.config.members(i).p_meas();
              const auto scale = signal.scale() == 0.0 ? 1.0 : signal.scale();
              totalMeasKw += group.memberMeasRaw[static_cast<size_t>(i)] * scale + signal.offset();
            }
            if (!group.tuningInitialCaptured) {
              group.tuningPhaseInitialMeasKw.assign(group.memberMeasRaw.size(), 0.0);
              for (int i = 0; i < group.config.members_size(); ++i) {
                if (static_cast<size_t>(i) < group.memberMeasRaw.size() &&
                    static_cast<size_t>(i) < group.hasMemberMeasRaw.size() && group.hasMemberMeasRaw[static_cast<size_t>(i)]) {
                  const auto &signal = group.config.members(i).p_meas();
                  const auto scale = signal.scale() == 0.0 ? 1.0 : signal.scale();
                  group.tuningPhaseInitialMeasKw[static_cast<size_t>(i)] = group.memberMeasRaw[static_cast<size_t>(i)] * scale + signal.offset();
                }
              }
              group.tuningInitialCaptured = true;
              group.tuningPreviousTargetKw = totalMeasKw;
            }
            group.tuningStatus.set_current_total_meas_kw(totalMeasKw);
            const auto errorKw = group.tuningStatus.current_target_kw() - totalMeasKw;
            const auto tolerance = group.tuningConfig.total_tolerance_kw();
            if (std::fabs(errorKw) <= tolerance) {
              if (!group.tuningInRange) {
                group.tuningInRange = true;
                group.tuningEnteredRangeAt = now;
                group.tuningStatus.set_target_entry_elapsed_seconds(elapsedSeconds);
              }
              const auto stableSeconds = std::chrono::duration_cast<std::chrono::duration<double>>(now - group.tuningEnteredRangeAt).count();
              group.tuningStatus.set_stable_elapsed_seconds(stableSeconds);
              if (stableSeconds >= group.tuningConfig.stable_hold_time_seconds()) {
                if (group.tuningStatus.direction() == AGCProto::TUNING_DIRECTION_UP) {
                  group.tuningStatus.set_completed_up_tests(group.tuningStatus.completed_up_tests() + 1);
                } else {
                  group.tuningStatus.set_completed_down_tests(group.tuningStatus.completed_down_tests() + 1);
                }
                auto *candidate = group.tuningStatus.mutable_candidate_profile();
                double totalWeight = 0.0;
                for (const auto &member : group.config.members()) {
                  if (member.controllable()) {
                    totalWeight += member.weight() > 0.0 ? member.weight() : 1.0;
                  }
                }
                for (int i = 0; i < group.config.members_size(); ++i) {
                  const auto &member = group.config.members(i);
                  if (!member.controllable()) {
                    continue;
                  }
                  AGCProto::MemberControlProfile *profile = nullptr;
                  for (int candidateIndex = 0; candidateIndex < candidate->members_size(); ++candidateIndex) {
                    if (candidate->members(candidateIndex).member_name() == member.member_name()) {
                      profile = candidate->mutable_members(candidateIndex);
                      break;
                    }
                  }
                  if (profile == nullptr) {
                    continue;
                  }
                  const auto share = member.weight() > 0.0 ? member.weight() : 1.0;
                  const auto correction = errorKw * (totalWeight > 0.0 ? share / totalWeight : 0.0);
                  const auto currentMeasured = static_cast<size_t>(i) < group.memberMeasRaw.size()
                                                   ? group.memberMeasRaw[static_cast<size_t>(i)] *
                                                         (group.config.members(i).p_meas().scale() == 0.0 ? 1.0 : group.config.members(i).p_meas().scale()) +
                                                         group.config.members(i).p_meas().offset()
                                                   : 0.0;
                  const auto initialMeasured = static_cast<size_t>(i) < group.tuningPhaseInitialMeasKw.size() ? group.tuningPhaseInitialMeasKw[static_cast<size_t>(i)] : currentMeasured;
                  if (std::fabs(group.tuningStatus.current_target_kw() - group.tuningPreviousTargetKw) > 1e-6 &&
                      std::fabs(currentMeasured - initialMeasured) > 1e-3) {
                    const auto expectedMemberDelta = (group.tuningStatus.current_target_kw() - group.tuningPreviousTargetKw) *
                                                     (totalWeight > 0.0 ? share / totalWeight : 0.0);
                    const auto estimatedGain = std::clamp(std::fabs(expectedMemberDelta / (currentMeasured - initialMeasured)), 0.05, 10.0);
                    if (group.tuningStatus.direction() == AGCProto::TUNING_DIRECTION_UP) {
                      profile->set_up_p_gain(estimatedGain);
                    } else {
                      profile->set_down_p_gain(estimatedGain);
                    }
                  }
                  if (group.tuningStatus.direction() == AGCProto::TUNING_DIRECTION_UP) {
                    profile->set_up_bias_kw(std::max(0.0, profile->up_bias_kw() + correction));
                  } else {
                    profile->set_down_bias_kw(std::max(0.0, profile->down_bias_kw() - correction));
                  }
                }
                const bool enough = group.tuningStatus.completed_up_tests() >= group.tuningConfig.min_up_tests() &&
                                    group.tuningStatus.completed_down_tests() >= group.tuningConfig.min_down_tests();
                if (enough) {
                  group.tuningStatus.set_state(AGCProto::TUNING_STATE_COMPLETED);
                  group.state = AGCProto::GROUP_STATE_STOPPED;
                  finish = true;
                } else {
                  const auto previousTargetKw = group.tuningStatus.current_target_kw();
                  const auto nextDirection = group.tuningStatus.direction() == AGCProto::TUNING_DIRECTION_UP
                                                  ? AGCProto::TUNING_DIRECTION_DOWN
                                                  : AGCProto::TUNING_DIRECTION_UP;
                  group.tuningStatus.set_direction(nextDirection);
                  group.tuningStatus.set_current_target_kw(nextDirection == AGCProto::TUNING_DIRECTION_UP
                                                                ? group.tuningConfig.target_upper_kw()
                                                                : group.tuningConfig.target_lower_kw());
                  group.tuningPhaseStartedAt = now;
                  group.tuningPreviousTargetKw = previousTargetKw;
                  group.tuningInitialCaptured = false;
                  group.tuningInRange = false;
                  group.tuningStatus.set_target_entry_elapsed_seconds(0.0);
                  group.tuningStatus.set_stable_elapsed_seconds(0.0);
                }
              }
            } else {
              group.tuningInRange = false;
              group.tuningStatus.set_stable_elapsed_seconds(0.0);
            }
            if (!finish && elapsedSeconds > static_cast<double>(group.tuningConfig.attempt_max_time_minutes()) * 60.0) {
              const auto previousTargetKw = group.tuningStatus.current_target_kw();
              const auto nextDirection = group.tuningStatus.direction() == AGCProto::TUNING_DIRECTION_UP
                                              ? AGCProto::TUNING_DIRECTION_DOWN
                                              : AGCProto::TUNING_DIRECTION_UP;
              group.tuningStatus.set_direction(nextDirection);
              group.tuningStatus.set_current_target_kw(nextDirection == AGCProto::TUNING_DIRECTION_UP
                                                            ? group.tuningConfig.target_upper_kw()
                                                            : group.tuningConfig.target_lower_kw());
              group.tuningPhaseStartedAt = now;
              group.tuningInRange = false;
              group.tuningStatus.set_target_entry_elapsed_seconds(0.0);
              group.tuningStatus.set_stable_elapsed_seconds(0.0);
              group.tuningPreviousTargetKw = previousTargetKw;
              group.tuningInitialCaptured = false;
              LOG_WARNING("AGC 自动调试单轮超时，切换方向: group_name={}, direction={}", groupName,
                          static_cast<int>(group.tuningStatus.direction()));
            }
            const auto totalLimit = static_cast<double>(group.tuningConfig.total_time_minutes()) * 60.0;
            if (!finish && totalElapsedSeconds > totalLimit) {
              const bool enough = group.tuningStatus.completed_up_tests() >= group.tuningConfig.min_up_tests() &&
                                  group.tuningStatus.completed_down_tests() >= group.tuningConfig.min_down_tests();
              group.tuningStatus.set_state(enough ? AGCProto::TUNING_STATE_COMPLETED : AGCProto::TUNING_STATE_FAILED);
              if (!enough) {
                group.tuningStatus.set_last_error("调试总时间到期但未完成最低上调/下调次数");
              }
              group.state = AGCProto::GROUP_STATE_STOPPED;
              finish = true;
            }
            if (finish) {
              if (group.dcSubscribeThread.joinable()) {
                group.dcSubscribeThread.request_stop();
              }
              if (group.controlThread.joinable()) {
                group.controlThread.request_stop();
              }
              if (group.controlTrigger) {
                group.controlTrigger->signal.release();
              }
            } else {
              requestControlLocked(groupName, &group, "自动调试周期", "");
            }
          }
          if (finish) {
            LOG_INFO("AGC 自动调试任务结束: group_name={}", groupName);
            break;
          }
        }
      });
  *out = it->second.tuningStatus;
  lock.unlock();
  primeControlInputs(groupName);
  LOG_INFO("AGC 已创建自动调试任务: group_name={}, 目标范围=[{}, {}]kW, 总时间={}min, 最低上调/下调={}/{}",
           request.group_name(), request.config().target_lower_kw(), request.config().target_upper_kw(),
           request.config().total_time_minutes(), request.config().min_up_tests(), request.config().min_down_tests());
  return grpc::Status::OK;
}

grpc::Status GroupManager::StopTuning(const std::string &groupName, AGCProto::TuningStatus *out) {
  if (out == nullptr) {
    return makeInvalid("out 为空");
  }
  auto status = validateGroupName(groupName);
  if (!status.ok()) {
    return status;
  }
  bool wasRunning = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      return makeNotFound(groupName);
    }
    wasRunning = it->second.tuningStatus.state() == AGCProto::TUNING_STATE_RUNNING;
    if (wasRunning) {
      it->second.tuningStatus.set_state(AGCProto::TUNING_STATE_STOPPED);
      it->second.tuningStatus.set_last_error("调试任务由上位机停止");
      LOG_INFO("AGC 已停止自动调试任务: group_name={}", groupName);
    }
  }
  bool needsCleanup = wasRunning;
  if (!needsCleanup) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    needsCleanup = it != groupsByName_.end() &&
                   (it->second.tuningThread.joinable() || it->second.controlThread.joinable() || it->second.dcSubscribeThread.joinable());
  }
  if (needsCleanup) {
    status = StopGroup(groupName);
    if (!status.ok()) {
      return status;
    }
  }
  return GetTuningStatus(groupName, out);
}

grpc::Status GroupManager::GetTuningStatus(const std::string &groupName, AGCProto::TuningStatus *out) const {
  if (out == nullptr) {
    return makeInvalid("out 为空");
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
  return fillTuningStatusLocked(it->second, out);
}

}  // namespace AGC
