#include "AGCGroupManager.h"

#include <grpcpp/client_context.h>

#include <chrono>
#include <cmath>
#include <format>
#include <thread>
#include <utility>

#include "AGCControl.h"
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
  return grpc::Status::OK;
}

AGCProto::GroupsConfig GroupManager::dumpGroupsConfigLocked() const {
  AGCProto::GroupsConfig config;
  for (const auto &[_, group] : groupsByName_) {
    auto* persisted = config.add_persisted_groups();
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
  LOG_INFO("AGC 开始恢复控制组持久化记录: group_name={}, 状态={}, 成员数={}",
           config.group_name(),
           restoredState,
           config.members_size());

  DataCenterProto::ConnectionInfo connInfo;
  status = dataCenter_.GetOrCreateConnection(config.group_name(), &connInfo);
  if (!status.ok()) {
    LOG_ERROR("AGC 恢复控制组时获取 DataCenter 连接失败: group_name={}, 成员数={}, 原因={}",
              config.group_name(),
              config.members_size(),
              status.error_message());
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
      LOG_ERROR("AGC 恢复控制组时同步 DataCenter 连接标签注册表失败: group_name={}, conn_id={}, 标签数={}, 原因={}",
                config.group_name(),
                runtime.connId,
                tagList.size(),
                connTagsStatus.error_message());
    } else {
      LOG_INFO("AGC 恢复控制组时已同步 DataCenter 连接标签注册表: group_name={}, conn_id={}, 标签数={}",
               config.group_name(),
               runtime.connId,
               tagList.size());
    }
    std::lock_guard<std::mutex> lock(mu_);
    groupsByName_[config.group_name()] = std::move(runtime);
    return connTagsStatus;
  }

  std::lock_guard<std::mutex> lock(mu_);
  groupsByName_[config.group_name()] = std::move(runtime);
  return grpc::Status::OK;
}

grpc::Status GroupManager::RestorePersistedGroups() {
  AGCProto::GroupsConfig config;
  auto status = groupStore_.Load(&config);
  if (!status.ok()) {
    LOG_ERROR("AGC 控制组配置加载失败: 原因={}", status.error_message());
    return status;
  }
  LOG_INFO("AGC 控制组持久化配置载入摘要: persisted_groups={}, legacy_groups={}",
           config.persisted_groups_size(),
           config.groups_size());
  if (config.groups_size() == 0 && config.persisted_groups_size() == 0) {
    LOG_INFO("AGC 未发现本地控制组配置");
    return grpc::Status::OK;
  }

  size_t restored = 0;
  size_t failed = 0;
  if (config.persisted_groups_size() > 0) {
    for (const auto& persisted : config.persisted_groups()) {
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
      LOG_INFO("AGC 已恢复控制组配置: group_name={}, 状态={}",
               persisted.config().group_name(),
               persisted.pending_delete() ? "待删除" : "已停止");
    }
  } else {
    for (const auto& group : config.groups()) {
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
    return;
  }

  g->dcSubscribeContext = std::make_shared<grpc::ClientContext>();
  auto ctx = g->dcSubscribeContext;

  g->dcSubscribeThread = ModuleManager::StartModuleThread(
      AGCLibInfo.LIB_NAME,
      [this, groupName, ctx, connId, tags](std::stop_token st) {
    std::stop_callback cb(st, [&ctx]() { ctx->TryCancel(); });

    auto reader = dataCenter_.Subscribe(ctx.get(), connId, tags, true);
    if (!reader) {
      return;
    }

    DataCenterProto::PointUpdate update;
    while (reader->Read(&update)) {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = groupsByName_.find(groupName);
      if (it == groupsByName_.end()) {
        break;
      }
      handleUpdateLocked(&it->second, update);
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

  // 控制组运行期间缓存控制周期。
  uint32_t periodMs = g->config.has_loop() ? g->config.loop().period_ms() : 0;
  if (periodMs == 0) {
    periodMs = 200;
  }

  g->controlThread = ModuleManager::StartModuleThread(
      AGCLibInfo.LIB_NAME,
      [this, groupName, periodMs](std::stop_token st) {
        while (!st.stop_requested()) {
          controlTick(groupName);
          std::this_thread::sleep_for(std::chrono::milliseconds(periodMs));
        }
      });
}

grpc::Status GroupManager::StartGroup(const std::string &groupName) {
  auto status = validateGroupName(groupName);
  if (!status.ok()) {
    return status;
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto it = groupsByName_.find(groupName);
  if (it == groupsByName_.end()) {
    return makeNotFound(groupName);
  }
  if (it->second.state == AGCProto::GROUP_STATE_PENDING_DELETE) {
    return makePreconditionFailed("控制组处于待删除状态");
  }
  if (it->second.state == AGCProto::GROUP_STATE_RUNNING) {
    return makePreconditionFailed("控制组已在运行");
  }
  startThreadsLocked(groupName, &it->second);
  it->second.state = AGCProto::GROUP_STATE_RUNNING;
  it->second.lastError.clear();
  return grpc::Status::OK;
}

grpc::Status GroupManager::StopGroup(const std::string &groupName) {
  auto status = validateGroupName(groupName);
  if (!status.ok()) {
    return status;
  }

  std::jthread dcSubscribeThread;
  std::jthread controlThread;
  bool pendingDelete = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      return makeNotFound(groupName);
    }
    dcSubscribeThread = std::move(it->second.dcSubscribeThread);
    controlThread = std::move(it->second.controlThread);
    it->second.dcSubscribeContext.reset();
    pendingDelete = (it->second.state == AGCProto::GROUP_STATE_PENDING_DELETE);
    it->second.state = pendingDelete ? AGCProto::GROUP_STATE_PENDING_DELETE : AGCProto::GROUP_STATE_STOPPED;
  }
  if (dcSubscribeThread.joinable()) {
    dcSubscribeThread.request_stop();
    dcSubscribeThread.join();
  }
  if (controlThread.joinable()) {
    controlThread.request_stop();
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

void GroupManager::handleUpdateLocked(GroupRuntime *g, const DataCenterProto::PointUpdate &update) {
  if (g == nullptr) {
    return;
  }
  const auto &tag = update.dst_tag();
  double raw = 0.0;
  if (!pointValueToDouble(update.value(), &raw)) {
    return;
  }

  if (!g->cmdTag.empty() && tag == g->cmdTag) {
    g->cmdRaw = raw;
    g->hasCmdRaw = true;
    return;
  }

  if (g->baseTags.contains(tag)) {
    g->baseRawByTag[tag] = raw;
    return;
  }

  auto it = g->memberIndexByMeasTag.find(tag);
  if (it != g->memberIndexByMeasTag.end()) {
    const auto idx = it->second;
    if (idx < g->memberMeasRaw.size()) {
      g->memberMeasRaw[idx] = raw;
      g->hasMemberMeasRaw[idx] = true;
    }
    return;
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
    input.hasLastTotalTargetKw = it->second.hasLastTotalTargetKw;
    input.lastTotalTargetKw = it->second.lastTotalTargetKw;
  }

  if (connId == 0) {
    return;
  }

  const auto outputOpt = ComputeControlOutput(config, input, weightedStrategy_);
  if (!outputOpt) {
    return;
  }
  const auto &output = *outputOpt;

  const auto quality = DataCenterProto::QUALITY_GOOD;
  if (config.has_outputs()) {
    const auto &o = config.outputs();
    if (output.publishTotalMeas) {
      (void)dataCenter_.PublishDouble(connId, o.p_total_meas().tag(), output.totalMeasRaw, quality, 0);
    }
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
    it->second.hasLastTotalTargetKw = output.hasLastTotalTargetKw;
    it->second.lastTotalTargetKw = output.nextLastTotalTargetKw;
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
