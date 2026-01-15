#include "AGCGroupManager.h"

#include <chrono>
#include <cmath>
#include <format>
#include <numeric>
#include <thread>
#include <utility>

#include <grpcpp/client_context.h>

namespace AGC {
namespace {
grpc::Status makeNotFound(const std::string& groupName) {
  return grpc::Status(grpc::StatusCode::NOT_FOUND, std::format("group not found: {}", groupName));
}

grpc::Status makeInvalid(std::string message) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::move(message));
}

grpc::Status makePreconditionFailed(std::string message) {
  return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::move(message));
}

double clampAbs(double v, double maxAbs) {
  if (maxAbs <= 0.0) {
    return v;
  }
  if (v > maxAbs) {
    return maxAbs;
  }
  if (v < -maxAbs) {
    return -maxAbs;
  }
  return v;
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

grpc::Status GroupManager::validateGroupName(const std::string& groupName) const {
  if (groupName.empty()) {
    return makeInvalid("group_name is required");
  }
  return grpc::Status::OK;
}

grpc::Status GroupManager::validateGroupConfig(const AGCProto::GroupConfig& config) const {
  auto st = validateGroupName(config.group_name());
  if (!st.ok()) {
    return st;
  }
  if (!config.has_p_cmd() || !config.p_cmd().has_signal()) {
    return makeInvalid("p_cmd.signal is required");
  }
  if (config.p_cmd().signal().tag().empty()) {
    return makeInvalid("p_cmd.signal.tag is required");
  }
  if (config.members_size() <= 0) {
    return makeInvalid("members is required");
  }

  std::unordered_set<std::string> memberNames;
  memberNames.reserve(static_cast<size_t>(config.members_size()));
  for (const auto& m : config.members()) {
    if (m.member_name().empty()) {
      return makeInvalid("members.member_name is required");
    }
    if (!memberNames.emplace(m.member_name()).second) {
      return makeInvalid(std::format("duplicate member_name: {}", m.member_name()));
    }
    if (!m.has_p_meas() || m.p_meas().tag().empty()) {
      return makeInvalid(std::format("members[{}].p_meas.tag is required", m.member_name()));
    }
    if (m.controllable()) {
      if (!m.has_p_set() || !m.p_set().has_signal() || m.p_set().signal().tag().empty()) {
        return makeInvalid(std::format("members[{}].p_set.signal.tag is required for controllable member", m.member_name()));
      }
    }
  }
  return grpc::Status::OK;
}

grpc::Status GroupManager::fillGroupInfoLocked(const GroupRuntime& g, AGCProto::GroupInfo* out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
  }
  out->Clear();
  *out->mutable_config() = g.config;
  out->set_conn_id(g.connId);
  out->set_state(g.state);
  out->set_last_error(g.lastError);
  return grpc::Status::OK;
}

grpc::Status GroupManager::UpsertGroup(const AGCProto::UpsertGroupRequest& request, AGCProto::GroupInfo* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
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
        return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "group_name already exists");
      }
      if (it->second.state == AGCProto::GROUP_STATE_RUNNING) {
        return makePreconditionFailed("stop group before updating config");
      }
      if (it->second.state == AGCProto::GROUP_STATE_PENDING_DELETE) {
        return makePreconditionFailed("group is pending delete");
      }

      it->second.config = request.config();
      rebuildTagCache(&it->second);
      it->second.lastError.clear();
      fillGroupInfoLocked(it->second, out);
    } else {
      // create-only should fail if conn_name already exists in DataCenter.
      if (request.create_only()) {
        bool exists = false;
        status = dataCenter_.ConnectionExists(groupName, &exists);
        if (!status.ok()) {
          return status;
        }
        if (exists) {
          return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "group_name already exists");
        }
      }

      DataCenterProto::ConnectionInfo connInfo;
      status = dataCenter_.GetOrCreateConnection(groupName, &connInfo);
      if (!status.ok()) {
        return status;
      }
      if (connInfo.conn_id() == 0) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "DataCenter returned conn_id=0");
      }

      auto [pos, inserted] = groupsByName_.try_emplace(groupName);
      auto& g = pos->second;
      g.config = request.config();
      g.connId = connInfo.conn_id();
      g.state = AGCProto::GROUP_STATE_STOPPED;
      g.lastError.clear();
      rebuildTagCache(&g);
      fillGroupInfoLocked(g, out);
    }
  }

  // Best-effort: register tags into DataCenter point table (no rollback).
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
      for (const auto& t : tags) {
        tagList.emplace_back(t);
      }
      status = dataCenter_.UpsertPointTable(connId, tagList, true);
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

grpc::Status GroupManager::GetGroup(const std::string& groupName, AGCProto::GroupInfo* out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
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

grpc::Status GroupManager::ListGroups(AGCProto::ListGroupsResponse* out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
  }
  std::lock_guard<std::mutex> lock(mu_);
  out->Clear();
  for (const auto& [_, g] : groupsByName_) {
    auto* elem = out->add_groups();
    fillGroupInfoLocked(g, elem);
  }
  return grpc::Status::OK;
}

void GroupManager::stopThreadsLocked(GroupRuntime* g) {
  if (g == nullptr) {
    return;
  }
  if (g->dcSubscribeThread.joinable()) {
    g->dcSubscribeThread.request_stop();
    g->dcSubscribeThread.join();
  }
  g->dcSubscribeContext.reset();

  if (g->controlThread.joinable()) {
    g->controlThread.request_stop();
    g->controlThread.join();
  }
}

void GroupManager::startThreadsLocked(const std::string& groupName, GroupRuntime* g) {
  if (g == nullptr) {
    return;
  }
  stopThreadsLocked(g);
  rebuildTagCache(g);

  const auto connId = g->connId;
  auto tags = g->subscribeTags;
  if (connId == 0 || tags.empty()) {
    return;
  }

  g->dcSubscribeContext = std::make_shared<grpc::ClientContext>();
  auto ctx = g->dcSubscribeContext;

  g->dcSubscribeThread = std::jthread([this, groupName, ctx, connId, tags](std::stop_token st) {
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

  // Cache the control period while the group is running.
  uint32_t periodMs = g->config.has_loop() ? g->config.loop().period_ms() : 0;
  if (periodMs == 0) {
    periodMs = 200;
  }

  g->controlThread = std::jthread([this, groupName, periodMs](std::stop_token st) {
    while (!st.stop_requested()) {
      controlTick(groupName);
      std::this_thread::sleep_for(std::chrono::milliseconds(periodMs));
    }
  });
}

grpc::Status GroupManager::StartGroup(const std::string& groupName) {
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
    return makePreconditionFailed("group is pending delete");
  }
  if (it->second.state == AGCProto::GROUP_STATE_RUNNING) {
    return makePreconditionFailed("group already running");
  }
  startThreadsLocked(groupName, &it->second);
  it->second.state = AGCProto::GROUP_STATE_RUNNING;
  it->second.lastError.clear();
  return grpc::Status::OK;
}

grpc::Status GroupManager::StopGroup(const std::string& groupName) {
  auto status = validateGroupName(groupName);
  if (!status.ok()) {
    return status;
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto it = groupsByName_.find(groupName);
  if (it == groupsByName_.end()) {
    return makeNotFound(groupName);
  }
  stopThreadsLocked(&it->second);
  it->second.state = AGCProto::GROUP_STATE_STOPPED;
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

bool GroupManager::pointValueToDouble(const DataCenterProto::PointValue& v, double* out) {
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

double GroupManager::effectiveScale(const AGCProto::SignalSpec& s) {
  return (s.scale() == 0.0) ? 1.0 : s.scale();
}

double GroupManager::toPhysicalAbs(const AGCProto::SignalSpec& s, double raw) {
  const auto scale = effectiveScale(s);
  return raw * scale + s.offset();
}

double GroupManager::toPhysicalDelta(const AGCProto::SignalSpec& s, double rawDelta) {
  const auto scale = effectiveScale(s);
  return rawDelta * scale;
}

double GroupManager::toRawAbs(const AGCProto::SignalSpec& s, double physical) {
  const auto scale = effectiveScale(s);
  return (physical - s.offset()) / scale;
}

double GroupManager::toRawDelta(const AGCProto::SignalSpec& s, double physicalDelta) {
  const auto scale = effectiveScale(s);
  return physicalDelta / scale;
}

std::unordered_set<std::string> GroupManager::collectAllTags(const AGCProto::GroupConfig& config) {
  std::unordered_set<std::string> tags;
  if (config.has_p_cmd() && config.p_cmd().has_signal() && !config.p_cmd().signal().tag().empty()) {
    tags.emplace(config.p_cmd().signal().tag());
    if (config.p_cmd().mode() == AGCProto::VALUE_MODE_DELTA && config.p_cmd().delta_base() == AGCProto::DELTA_BASE_BASE_TAG &&
        !config.p_cmd().base_tag().empty()) {
      tags.emplace(config.p_cmd().base_tag());
    }
  }

  if (config.has_outputs()) {
    const auto& o = config.outputs();
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

  for (const auto& m : config.members()) {
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

void GroupManager::rebuildTagCache(GroupRuntime* g) {
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
    const auto& m = g->config.members(i);
    if (m.has_p_meas() && !m.p_meas().tag().empty()) {
      g->memberIndexByMeasTag.emplace(m.p_meas().tag(), static_cast<size_t>(i));
      g->subscribeTags.emplace_back(m.p_meas().tag());
    }
    if (m.has_p_set() && m.p_set().mode() == AGCProto::VALUE_MODE_DELTA && m.p_set().delta_base() == AGCProto::DELTA_BASE_BASE_TAG &&
        !m.p_set().base_tag().empty()) {
      g->baseTags.emplace(m.p_set().base_tag());
    }
  }

  for (const auto& t : g->baseTags) {
    g->subscribeTags.emplace_back(t);
  }
}

void GroupManager::handleUpdateLocked(GroupRuntime* g, const DataCenterProto::PointUpdate& update) {
  if (g == nullptr) {
    return;
  }
  const auto& tag = update.dst_tag();
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

void GroupManager::controlTick(const std::string& groupName) {
  AGCProto::GroupConfig config;
  uint32_t connId = 0;
  bool hasCmdRaw = false;
  double cmdRaw = 0.0;
  std::unordered_map<std::string, double> baseRawByTag;
  std::vector<bool> hasMemberMeasRaw;
  std::vector<double> memberMeasRaw;
  std::vector<bool> hasLastMemberTargetKw;
  std::vector<double> lastMemberTargetKw;
  bool hasLastDesiredTotalKw = false;
  double lastDesiredTotalKw = 0.0;
  bool hasLastTotalTargetKw = false;
  double lastTotalTargetKw = 0.0;

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
    hasCmdRaw = it->second.hasCmdRaw;
    cmdRaw = it->second.cmdRaw;
    baseRawByTag = it->second.baseRawByTag;
    hasMemberMeasRaw = it->second.hasMemberMeasRaw;
    memberMeasRaw = it->second.memberMeasRaw;
    hasLastMemberTargetKw = it->second.hasLastMemberTargetKw;
    lastMemberTargetKw = it->second.lastMemberTargetKw;
    hasLastDesiredTotalKw = it->second.hasLastDesiredTotalKw;
    lastDesiredTotalKw = it->second.lastDesiredTotalKw;
    hasLastTotalTargetKw = it->second.hasLastTotalTargetKw;
    lastTotalTargetKw = it->second.lastTotalTargetKw;
  }

  if (connId == 0 || !hasCmdRaw || !config.has_p_cmd() || !config.p_cmd().has_signal()) {
    return;
  }

  const auto memberCount = static_cast<size_t>(config.members_size());
  if (memberCount == 0) {
    return;
  }

  // Convert member measurements to kW.
  std::vector<double> measKw(memberCount, 0.0);
  for (size_t i = 0; i < memberCount && i < memberMeasRaw.size() && i < hasMemberMeasRaw.size(); ++i) {
    if (!hasMemberMeasRaw[i]) {
      continue;
    }
    measKw[i] = toPhysicalAbs(config.members(static_cast<int>(i)).p_meas(), memberMeasRaw[i]);
  }

  const auto totalMeasKw = std::accumulate(measKw.begin(), measKw.end(), 0.0);

  // Desired total setpoint (kW) from command.
  const auto& cmdSpec = config.p_cmd();
  double cmdKw = 0.0;
  if (cmdSpec.mode() == AGCProto::VALUE_MODE_DELTA) {
    cmdKw = toPhysicalDelta(cmdSpec.signal(), cmdRaw);
  } else {
    cmdKw = toPhysicalAbs(cmdSpec.signal(), cmdRaw);
  }

  double desiredTotalKw = cmdKw;
  if (cmdSpec.mode() == AGCProto::VALUE_MODE_DELTA) {
    double baseKw = totalMeasKw;
    switch (cmdSpec.delta_base()) {
    case AGCProto::DELTA_BASE_LAST_TARGET:
      if (hasLastDesiredTotalKw) {
        baseKw = lastDesiredTotalKw;
      }
      break;
    case AGCProto::DELTA_BASE_BASE_TAG: {
      auto it = baseRawByTag.find(cmdSpec.base_tag());
      if (it != baseRawByTag.end()) {
        baseKw = toPhysicalAbs(cmdSpec.signal(), it->second);
      }
      break;
    }
    case AGCProto::DELTA_BASE_CURRENT_MEAS:
    case AGCProto::DELTA_BASE_UNSPECIFIED:
    default:
      break;
    }
    desiredTotalKw = baseKw + cmdKw;
  }

  // Publish derived outputs (best-effort).
  const auto quality = DataCenterProto::QUALITY_GOOD;
  if (config.has_outputs()) {
    const auto& o = config.outputs();
    if (o.has_p_total_meas() && !o.p_total_meas().tag().empty()) {
      const auto raw = toRawAbs(o.p_total_meas(), totalMeasKw);
      (void)dataCenter_.PublishDouble(connId, o.p_total_meas().tag(), raw, quality, 0);
    }
  }

  // Compute total target with multi-step adjustment.
  const auto kp = (config.has_loop() && config.loop().kp() != 0.0) ? config.loop().kp() : 1.0;
  const auto maxStepKw = config.has_loop() ? config.loop().max_step_kw() : 0.0;
  const auto deadbandKw = config.has_loop() ? config.loop().deadband_kw() : 0.0;

  const auto errorKw = desiredTotalKw - totalMeasKw;
  double stepKw = 0.0;
  if (!(deadbandKw > 0.0 && std::fabs(errorKw) <= deadbandKw)) {
    stepKw = clampAbs(kp * errorKw, maxStepKw);
  }

  double currentTargetKw = hasLastTotalTargetKw ? lastTotalTargetKw : totalMeasKw;
  double nextTargetKw = currentTargetKw + stepKw;
  if (stepKw > 0.0) {
    nextTargetKw = std::min(nextTargetKw, desiredTotalKw);
  }
  if (stepKw < 0.0) {
    nextTargetKw = std::max(nextTargetKw, desiredTotalKw);
  }

  double passiveKw = 0.0;
  for (size_t i = 0; i < memberCount; ++i) {
    if (!config.members(static_cast<int>(i)).controllable()) {
      passiveKw += measKw[i];
    }
  }

  // Allocate to controllable members.
  std::vector<size_t> controllableIdx;
  controllableIdx.reserve(memberCount);
  std::vector<AGVC::AllocationMember> allocMembers;
  allocMembers.reserve(memberCount);
  for (size_t i = 0; i < memberCount; ++i) {
    const auto& m = config.members(static_cast<int>(i));
    if (!m.controllable()) {
      continue;
    }
    controllableIdx.emplace_back(i);

    AGVC::AllocationMember a;
    a.weight = m.weight() > 0.0 ? m.weight() : (m.capacity_kw() > 0.0 ? m.capacity_kw() : 1.0);
    a.min = m.min_kw();
    a.max = m.max_kw();
    if (a.max == 0.0 && m.capacity_kw() > 0.0) {
      a.max = m.capacity_kw();
    }
    allocMembers.emplace_back(a);
  }

  const auto targetControllableKw = nextTargetKw - passiveKw;
  const auto alloc = weightedStrategy_.Allocate(targetControllableKw, allocMembers);

  // Member targets in kW (for controllable members only).
  std::vector<double> memberTargetKw(memberCount, 0.0);
  for (size_t k = 0; k < controllableIdx.size() && k < alloc.values.size(); ++k) {
    memberTargetKw[controllableIdx[k]] = alloc.values[k];
  }

  double actualTargetKw = passiveKw;
  for (size_t i = 0; i < memberCount; ++i) {
    if (config.members(static_cast<int>(i)).controllable()) {
      actualTargetKw += memberTargetKw[i];
    }
  }

  if (config.has_outputs()) {
    const auto& o = config.outputs();
    if (o.has_p_total_target() && !o.p_total_target().tag().empty()) {
      const auto raw = toRawAbs(o.p_total_target(), actualTargetKw);
      (void)dataCenter_.PublishDouble(connId, o.p_total_target().tag(), raw, quality, 0);
    }
    if (o.has_p_total_error() && !o.p_total_error().tag().empty()) {
      const auto raw = toRawAbs(o.p_total_error(), desiredTotalKw - totalMeasKw);
      (void)dataCenter_.PublishDouble(connId, o.p_total_error().tag(), raw, quality, 0);
    }
  }

  // Publish member setpoints.
  for (size_t i = 0; i < memberCount; ++i) {
    const auto& m = config.members(static_cast<int>(i));
    if (!m.controllable()) {
      continue;
    }
    if (!m.has_p_set() || !m.p_set().has_signal() || m.p_set().signal().tag().empty()) {
      continue;
    }

    const auto& outSpec = m.p_set();
    double publishRaw = 0.0;
    if (outSpec.mode() == AGCProto::VALUE_MODE_DELTA) {
      double baseKw = 0.0;
      switch (outSpec.delta_base()) {
      case AGCProto::DELTA_BASE_LAST_TARGET:
        if (i < lastMemberTargetKw.size() && i < hasLastMemberTargetKw.size() && hasLastMemberTargetKw[i]) {
          baseKw = lastMemberTargetKw[i];
        }
        break;
      case AGCProto::DELTA_BASE_CURRENT_MEAS:
        baseKw = measKw[i];
        break;
      case AGCProto::DELTA_BASE_BASE_TAG: {
        auto it = baseRawByTag.find(outSpec.base_tag());
        if (it != baseRawByTag.end()) {
          baseKw = toPhysicalAbs(outSpec.signal(), it->second);
        } else {
          baseKw = measKw[i];
        }
        break;
      }
      case AGCProto::DELTA_BASE_UNSPECIFIED:
      default:
        baseKw = measKw[i];
        break;
      }
      publishRaw = toRawDelta(outSpec.signal(), memberTargetKw[i] - baseKw);
    } else {
      publishRaw = toRawAbs(outSpec.signal(), memberTargetKw[i]);
    }

    (void)dataCenter_.PublishDouble(connId, outSpec.signal().tag(), publishRaw, quality, 0);
  }

  // Update state (best-effort) after publishing.
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      return;
    }
    it->second.hasLastDesiredTotalKw = true;
    it->second.lastDesiredTotalKw = desiredTotalKw;
    it->second.hasLastTotalTargetKw = true;
    it->second.lastTotalTargetKw = actualTargetKw;

    const auto count = static_cast<size_t>(it->second.config.members_size());
    if (it->second.hasLastMemberTargetKw.size() != count) {
      it->second.hasLastMemberTargetKw.assign(count, false);
      it->second.lastMemberTargetKw.assign(count, 0.0);
    }
    for (size_t i = 0; i < count; ++i) {
      if (it->second.config.members(static_cast<int>(i)).controllable()) {
        it->second.hasLastMemberTargetKw[i] = true;
        it->second.lastMemberTargetKw[i] = memberTargetKw[i];
      }
    }
  }
}

}  // namespace AGC
