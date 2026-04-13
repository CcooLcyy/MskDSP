#include "AGCGroupManager.h"

#include <grpcpp/client_context.h>

#include <cmath>
#include <format>
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

std::string_view defaultPointTag(AGCProto::DefaultPointKind kind) {
  for (const auto &point : DefaultPointDefinitions()) {
    if (point.kind == kind) {
      return point.tag;
    }
  }
  return {};
}
}  // namespace

GroupManager::GroupManager(std::string moduleName, std::filesystem::path groupsPath) :
  groupStore_(std::move(groupsPath)),
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
  FillDefaultPointInfos(out->mutable_default_points());
  return grpc::Status::OK;
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
  rebuildTagCache(&runtime);

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
      rebuildTagCache(&g);
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
          {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = groupsByName_.find(groupName);
            if (it == groupsByName_.end()) {
              break;
            }
            if (!it->second.cmdTag.empty() && update.dst_tag() == it->second.cmdTag) {
              publishCommandEcho = true;
              commandEchoConnId = it->second.connId;
            }
            if (handleUpdateLocked(&it->second, update)) {
              requestControlLocked(groupName, &it->second, "订阅输入点更新", update.dst_tag());
            }
          }
          if (publishCommandEcho && commandEchoConnId != 0) {
            publishCommandEchoPoint(commandEchoConnId, update);
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

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      return makeNotFound(groupName);
    }
    if (it->second.state == AGCProto::GROUP_STATE_RUNNING) {
      LOG_INFO("AGC 启动控制组请求幂等成功: group_name={}, 原因=控制组已在运行", groupName);
      return grpc::Status::OK;
    }
    status = checkStartPreconditionsLocked(it->second);
    if (!status.ok()) {
      it->second.lastError = status.error_message();
      return status;
    }
    startThreadsLocked(groupName, &it->second);
    it->second.state = AGCProto::GROUP_STATE_RUNNING;
    it->second.lastError.clear();
  }
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
    pendingDelete = (it->second.state == AGCProto::GROUP_STATE_PENDING_DELETE);
    it->second.state = pendingDelete ? AGCProto::GROUP_STATE_PENDING_DELETE : AGCProto::GROUP_STATE_STOPPED;
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
  const auto theoreticalQuality = DataCenterProto::QUALITY_GOOD;

  auto status = dataCenter_.PublishDouble(connId, std::string(theoreticalLowerTag), defaultOutput.theoreticalLowerKw, theoreticalQuality, 0);
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

void GroupManager::publishCommandEchoPoint(uint32_t connId, const DataCenterProto::PointUpdate &update) {
  const auto commandEchoTag = defaultPointTag(AGCProto::DEFAULT_POINT_KIND_COMMAND_ECHO);
  auto status = dataCenter_.PublishValue(connId, std::string(commandEchoTag), update.value(), update.quality(), update.ts_ms());
  if (!status.ok()) {
    LOG_ERROR("AGC 发布调节返回值失败: conn_id={}, tag={}, 原因={}", connId, commandEchoTag, status.error_message());
  } else {
    LOG_DEBUG("AGC 已发布调节返回值: conn_id={}, tag={}, 质量={}, ts_ms={}", connId, commandEchoTag, static_cast<int>(update.quality()), update.ts_ms());
  }
}

void GroupManager::controlTick(const std::string &groupName) {
  AGCProto::GroupConfig config;
  uint32_t connId = 0;
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
    input.hasCmdRaw = it->second.hasCmdRaw;
    input.cmdRaw = it->second.cmdRaw;
    input.baseRawByTag = it->second.baseRawByTag;
    input.hasMemberMeasRaw = it->second.hasMemberMeasRaw;
    input.memberMeasRaw = it->second.memberMeasRaw;
    input.hasLastMemberTargetKw = it->second.hasLastMemberTargetKw;
    input.lastMemberTargetKw = it->second.lastMemberTargetKw;
    input.hasLastDesiredTotalKw = it->second.hasLastDesiredTotalKw;
    input.lastDesiredTotalKw = it->second.lastDesiredTotalKw;
  }

  if (connId == 0) {
    return;
  }
  const auto quality = DataCenterProto::QUALITY_GOOD;
  double totalMeasKw = 0.0;
  if (const auto totalMeasRaw = ComputeTotalMeasRaw(config, input, &totalMeasKw)) {
    auto status = dataCenter_.PublishDouble(connId, config.outputs().p_total_meas().tag(), *totalMeasRaw, quality, 0);
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
          "AGC 已发布总实时测量值: group_name={}, conn_id={}, tag={}, total_meas_kw={}, raw={}",
          groupName,
          connId,
          config.outputs().p_total_meas().tag(),
          totalMeasKw,
          *totalMeasRaw);
    }
  }
  publishDefaultLimitPoints(groupName, "事件触发控制");

  const auto outputOpt = ComputeControlOutput(config, input, weightedStrategy_);
  if (!outputOpt) {
    return;
  }
  const auto &output = *outputOpt;

  if (config.has_outputs()) {
    const auto &o = config.outputs();
    if (output.publishTotalTarget) {
      (void)dataCenter_.PublishDouble(connId, o.p_total_target().tag(), output.totalTargetRaw, quality, 0);
    }
    if (output.publishTotalError) {
      (void)dataCenter_.PublishDouble(connId, o.p_total_error().tag(), output.totalErrorRaw, quality, 0);
    }
  }

  // 下发成员设定值。
  const auto memberCount = static_cast<size_t>(config.members_size());
  for (size_t i = 0; i < memberCount && i < output.memberPublish.size() && i < output.memberPublishRaw.size(); ++i) {
    if (!output.memberPublish[i]) {
      continue;
    }
    const auto &m = config.members(static_cast<int>(i));
    if (!m.has_p_set() || !m.p_set().has_signal() || m.p_set().signal().tag().empty()) {
      continue;
    }
    (void)dataCenter_.PublishDouble(connId, m.p_set().signal().tag(), output.memberPublishRaw[i], quality, 0);
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

}  // namespace AGC
