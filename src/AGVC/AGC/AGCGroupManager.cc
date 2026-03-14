#include "AGCGroupManager.h"

#include <grpcpp/client_context.h>

#include <chrono>
#include <cmath>
#include <format>
#include <thread>
#include <utility>

#include "AGCControl.h"
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

GroupManager::GroupManager(std::string moduleName) :
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
  auto st = validateGroupName(config.group_name());
  if (!st.ok()) {
    return st;
  }
  if (!config.has_p_cmd() || !config.p_cmd().has_signal()) {
    return makeInvalid("p_cmd.signal 不能为空");
  }
  if (config.p_cmd().signal().tag().empty()) {
    return makeInvalid("p_cmd.signal.tag 不能为空");
  }
  if (config.members_size() <= 0) {
    return makeInvalid("members 不能为空");
  }

  std::unordered_set<std::string> memberNames;
  memberNames.reserve(static_cast<size_t>(config.members_size()));
  for (const auto &m : config.members()) {
    if (m.member_name().empty()) {
      return makeInvalid("members.member_name 不能为空");
    }
    if (!memberNames.emplace(m.member_name()).second) {
      return makeInvalid(std::format("member_name 重复: {}", m.member_name()));
    }
    if (!m.has_p_meas() || m.p_meas().tag().empty()) {
      return makeInvalid(std::format("members[{}].p_meas.tag 不能为空", m.member_name()));
    }
    if (m.controllable()) {
      if (!m.has_p_set() || !m.p_set().has_signal() || m.p_set().signal().tag().empty()) {
        return makeInvalid(std::format("members[{}].p_set.signal.tag 不能为空（可控成员）", m.member_name()));
      }
    }
  }
  return grpc::Status::OK;
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
  }

  // 尽力而为：将 tags 注册到 DataCenter 点表（不回滚）。
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
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      return makeNotFound(groupName);
    }
    dcSubscribeThread = std::move(it->second.dcSubscribeThread);
    controlThread = std::move(it->second.controlThread);
    it->second.dcSubscribeContext.reset();
    it->second.state = AGCProto::GROUP_STATE_STOPPED;
  }
  if (dcSubscribeThread.joinable()) {
    dcSubscribeThread.request_stop();
    dcSubscribeThread.join();
  }
  if (controlThread.joinable()) {
    controlThread.request_stop();
    controlThread.join();
  }
  LOG_INFO("AGC 控制组已停止: group_name={}", groupName);
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
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it != groupsByName_.end()) {
      it->second.state = AGCProto::GROUP_STATE_PENDING_DELETE;
      it->second.lastError = dc.error_message();
    }
    return dc;
  }

  std::lock_guard<std::mutex> lock(mu_);
  groupsByName_.erase(groupName);
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
