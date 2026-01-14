#include "IEC104LinkManager.h"

#include <chrono>
#include <format>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace IEC104 {
namespace {
constexpr uint8_t kIec104QualityGood = 0x00;
constexpr uint8_t kIec104QualityInvalid = 0x80;

constexpr uint8_t kCotSpontaneous = 3;

DataCenterProto::Quality toDataCenterQuality(uint8_t qds) {
  if ((qds & kIec104QualityInvalid) != 0) {
    return DataCenterProto::QUALITY_BAD;
  }
  if (qds == 0) {
    return DataCenterProto::QUALITY_GOOD;
  }
  return DataCenterProto::QUALITY_UNCERTAIN;
}

uint8_t toIec104Quality(DataCenterProto::Quality q) {
  switch (q) {
  case DataCenterProto::QUALITY_GOOD:
    return kIec104QualityGood;
  case DataCenterProto::QUALITY_BAD:
  case DataCenterProto::QUALITY_UNCERTAIN:
  case DataCenterProto::QUALITY_UNSPECIFIED:
  default:
    return kIec104QualityInvalid;
  }
}

bool pointValueToDouble(const DataCenterProto::PointValue& v, double* out) {
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

grpc::Status makeNotFound(const std::string& connName) {
  return grpc::Status(grpc::StatusCode::NOT_FOUND, std::format("link not found: {}", connName));
}
}  // namespace

LinkManager::LinkManager(std::string moduleName) : dataCenter_(std::move(moduleName)) {}

void LinkManager::setDataCenterServerAddress(std::string address) {
  dataCenter_.setServerAddress(std::move(address));
}

void LinkManager::setDataCenterStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub) {
  dataCenter_.setStub(std::move(stub));
}

grpc::Status LinkManager::validateConnName(const std::string& connName) {
  if (connName.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_name is required");
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::validateLinkConfig(const IEC104Proto::LinkConfig& config) {
  auto s = validateConnName(config.conn_name());
  if (!s.ok()) {
    return s;
  }
  if (config.role() != IEC104Proto::ROLE_SERVER && config.role() != IEC104Proto::ROLE_CLIENT) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "role is required");
  }
  if (config.role() == IEC104Proto::ROLE_SERVER) {
    if (config.local().port() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "local.port is required for server role");
    }
  }
  if (config.role() == IEC104Proto::ROLE_CLIENT) {
    if (config.remote().ip().empty() || config.remote().port() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "remote.ip/port is required for client role");
    }
  }
  if (config.ca() > 65535) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "ca must be <= 65535");
  }
  if (config.oa() > 255) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "oa must be <= 255");
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::fillLinkInfoLocked(const LinkRuntime& link, IEC104Proto::LinkInfo* out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
  }
  out->Clear();
  *out->mutable_config() = link.config;
  out->set_conn_id(link.connId);
  out->set_state(link.state);
  out->set_last_error(link.lastError);
  return grpc::Status::OK;
}

grpc::Status LinkManager::UpsertLink(const IEC104Proto::UpsertLinkRequest& request, IEC104Proto::LinkInfo* out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
  }
  auto status = validateLinkConfig(request.config());
  if (!status.ok()) {
    return status;
  }
  const auto connName = request.config().conn_name();

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it != linksByName_.end()) {
      if (request.create_only()) {
        return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name already exists");
      }
      if (it->second.state == IEC104Proto::LINK_STATE_RUNNING) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "stop link before updating config");
      }
      if (it->second.state == IEC104Proto::LINK_STATE_PENDING_DELETE) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "link is pending delete");
      }
      it->second.config = request.config();
      it->second.lastError.clear();
      return fillLinkInfoLocked(it->second, out);
    }
  }

  if (request.create_only()) {
    bool exists = false;
    status = dataCenter_.ConnectionExists(connName, &exists);
    if (!status.ok()) {
      return status;
    }
    if (exists) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name already exists");
    }
  }

  DataCenterProto::ConnectionInfo connInfo;
  status = dataCenter_.GetOrCreateConnection(connName, &connInfo);
  if (!status.ok()) {
    return status;
  }
  if (connInfo.conn_id() == 0) {
    return grpc::Status(grpc::StatusCode::INTERNAL, "DataCenter returned conn_id=0");
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto [it, inserted] = linksByName_.try_emplace(connName);
  if (!inserted) {
    if (request.create_only()) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name already exists");
    }
    return fillLinkInfoLocked(it->second, out);
  }

  it->second.config = request.config();
  it->second.connId = connInfo.conn_id();
  it->second.state = IEC104Proto::LINK_STATE_STOPPED;
  it->second.lastError.clear();
  return fillLinkInfoLocked(it->second, out);
}

grpc::Status LinkManager::GetLink(const std::string& connName, IEC104Proto::LinkInfo* out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
  }
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto it = linksByName_.find(connName);
  if (it == linksByName_.end()) {
    return makeNotFound(connName);
  }
  return fillLinkInfoLocked(it->second, out);
}

grpc::Status LinkManager::ListLinks(IEC104Proto::ListLinksResponse* out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
  }
  std::lock_guard<std::mutex> lock(mu_);
  out->Clear();
  for (const auto& [_, link] : linksByName_) {
    auto* elem = out->add_links();
    fillLinkInfoLocked(link, elem);
  }
  return grpc::Status::OK;
}

void LinkManager::configureTransportCallbacksLocked(const std::string& connName, LinkRuntime* link) {
  if (link == nullptr || !link->transport) {
    return;
  }
  if (link->config.role() == IEC104Proto::ROLE_CLIENT) {
    link->transport->SetMeasuredValueCallback([this, connName](const MeasuredValue& mv) {
      (void)handleClientMeasuredValue(connName, mv);
    });
  }
  if (link->config.role() == IEC104Proto::ROLE_SERVER) {
    link->transport->SetInterrogationSnapshotProvider([this, connName]() { return buildInterrogationSnapshot(connName); });
  }
}

grpc::Status LinkManager::StartLink(const std::string& connName) {
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }

  LinkRuntime* link = nullptr;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      return makeNotFound(connName);
    }
    if (it->second.state == IEC104Proto::LINK_STATE_PENDING_DELETE) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "link is pending delete");
    }
    if (it->second.state == IEC104Proto::LINK_STATE_RUNNING) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "link already running");
    }
    it->second.transport = std::make_unique<TcpLink>(it->second.config);
    configureTransportCallbacksLocked(connName, &it->second);
    link = &it->second;
  }

  status = link->transport->Start();
  if (!status.ok()) {
    std::lock_guard<std::mutex> lock(mu_);
    link->lastError = status.error_message();
    link->transport.reset();
    return status;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      return makeNotFound(connName);
    }
    it->second.state = IEC104Proto::LINK_STATE_RUNNING;
    it->second.lastError.clear();
    if (it->second.config.role() == IEC104Proto::ROLE_SERVER) {
      startDataCenterSubscribeLocked(connName, &it->second);
    }
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::StopLink(const std::string& connName) {
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }

  std::unique_ptr<TcpLink> transport;
  bool pendingDelete = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      return makeNotFound(connName);
    }
    pendingDelete = (it->second.state == IEC104Proto::LINK_STATE_PENDING_DELETE);
    stopDataCenterSubscribeLocked(&it->second);
    transport = std::move(it->second.transport);
    it->second.state = pendingDelete ? IEC104Proto::LINK_STATE_PENDING_DELETE : IEC104Proto::LINK_STATE_STOPPED;
  }

  if (transport) {
    transport->Stop();
  }
  return grpc::Status::OK;
}

void LinkManager::stopDataCenterSubscribeLocked(LinkRuntime* link) {
  if (link == nullptr) {
    return;
  }
  if (link->dcSubscribeThread.joinable()) {
    link->dcSubscribeThread.request_stop();
    link->dcSubscribeThread.join();
  }
  link->dcSubscribeContext.reset();
}

void LinkManager::startDataCenterSubscribeLocked(const std::string& connName, LinkRuntime* link) {
  if (link == nullptr || link->config.role() != IEC104Proto::ROLE_SERVER || !link->transport) {
    return;
  }
  stopDataCenterSubscribeLocked(link);

  auto tags = link->pointTable.Tags();
  std::unordered_map<std::string, uint32_t> ioaByTag;
  ioaByTag.reserve(tags.size());
  for (const auto& tag : tags) {
    auto p = link->pointTable.FindByTag(tag);
    if (p) {
      ioaByTag.emplace(tag, p->ioa);
    }
  }

  auto* transport = link->transport.get();
  auto connId = link->connId;

  link->dcSubscribeContext = std::make_shared<grpc::ClientContext>();
  auto ctx = link->dcSubscribeContext;

  link->dcSubscribeThread = std::jthread([this, connName, ctx, connId, tags, ioaByTag, transport](std::stop_token st) {
    std::stop_callback cb(st, [&ctx]() { ctx->TryCancel(); });

    auto reader = dataCenter_.Subscribe(ctx.get(), connId, tags, false);
    if (!reader) {
      return;
    }

    DataCenterProto::PointUpdate update;
    while (reader->Read(&update)) {
      auto it = ioaByTag.find(update.dst_tag());
      if (it == ioaByTag.end()) {
        continue;
      }
      double value = 0;
      if (!pointValueToDouble(update.value(), &value)) {
        continue;
      }
      uint8_t qds = toIec104Quality(update.quality());
      transport->SendMeasuredValue(it->second, value, qds, kCotSpontaneous);
    }

    auto finishStatus = reader->Finish();
    if (!finishStatus.ok() && !st.stop_requested()) {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = linksByName_.find(connName);
      if (it != linksByName_.end()) {
        it->second.lastError = finishStatus.error_message();
      }
    }
  });
}

grpc::Status LinkManager::DeleteLink(const std::string& connName) {
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }

  status = StopLink(connName);
  if (!status.ok() && status.error_code() != grpc::StatusCode::NOT_FOUND) {
    return status;
  }

  grpc::Status dc = dataCenter_.DeleteConnection(connName);
  if (!dc.ok() && dc.error_code() != grpc::StatusCode::NOT_FOUND) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it != linksByName_.end()) {
      it->second.state = IEC104Proto::LINK_STATE_PENDING_DELETE;
      it->second.lastError = dc.error_message();
    }
    return dc;
  }

  std::lock_guard<std::mutex> lock(mu_);
  linksByName_.erase(connName);
  return grpc::Status::OK;
}

grpc::Status LinkManager::UpsertPointTable(const IEC104Proto::UpsertPointTableRequest& request) {
  auto status = validateConnName(request.conn_name());
  if (!status.ok()) {
    return status;
  }

  uint32_t connId = 0;
  PointTable current;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(request.conn_name());
    if (it == linksByName_.end()) {
      return makeNotFound(request.conn_name());
    }
    if (it->second.state == IEC104Proto::LINK_STATE_RUNNING) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "stop link before updating point table");
    }
    if (it->second.state == IEC104Proto::LINK_STATE_PENDING_DELETE) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "link is pending delete");
    }
    connId = it->second.connId;
    current = it->second.pointTable;
  }

  PointTable next = current;
  status = next.Upsert(request.points(), request.replace());
  if (!status.ok()) {
    return status;
  }

  auto tags = next.Tags();
  status = dataCenter_.UpsertPointTable(connId, tags, true);
  if (!status.ok()) {
    return status;
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto it = linksByName_.find(request.conn_name());
  if (it == linksByName_.end()) {
    return makeNotFound(request.conn_name());
  }
  it->second.pointTable = std::move(next);
  return grpc::Status::OK;
}

grpc::Status LinkManager::GetPointTable(const std::string& connName, IEC104Proto::PointTable* out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
  }
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto it = linksByName_.find(connName);
  if (it == linksByName_.end()) {
    return makeNotFound(connName);
  }
  it->second.pointTable.ToProto(connName, out);
  return grpc::Status::OK;
}

grpc::Status LinkManager::handleClientMeasuredValue(const std::string& connName, const MeasuredValue& mv) {
  uint32_t connId = 0;
  std::string tag;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      return makeNotFound(connName);
    }
    connId = it->second.connId;
    auto p = it->second.pointTable.FindByIoa(mv.ioa);
    if (!p) {
      return grpc::Status::OK;
    }
    tag = p->tag;
  }

  auto quality = toDataCenterQuality(mv.quality);
  auto st = dataCenter_.PublishDouble(connId, tag, mv.value, quality, 0);
  if (!st.ok()) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it != linksByName_.end()) {
      it->second.lastError = st.error_message();
    }
  }
  return st;
}

std::vector<MeasuredValue> LinkManager::buildInterrogationSnapshot(const std::string& connName) {
  uint32_t connId = 0;
  std::unordered_map<std::string, uint32_t> ioaByTag;
  std::vector<std::string> tags;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      return {};
    }
    connId = it->second.connId;
    tags = it->second.pointTable.Tags();
    ioaByTag.reserve(tags.size());
    for (const auto& tag : tags) {
      auto p = it->second.pointTable.FindByTag(tag);
      if (p) {
        ioaByTag.emplace(tag, p->ioa);
      }
    }
  }

  DataCenterProto::GetLatestResponse resp;
  auto status = dataCenter_.GetLatest(connId, tags, &resp);
  if (!status.ok()) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it != linksByName_.end()) {
      it->second.lastError = status.error_message();
    }
    return {};
  }

  std::vector<MeasuredValue> out;
  out.reserve(static_cast<size_t>(resp.updates_size()));
  for (const auto& update : resp.updates()) {
    auto it = ioaByTag.find(update.dst_tag());
    if (it == ioaByTag.end()) {
      continue;
    }
    double value = 0;
    if (!pointValueToDouble(update.value(), &value)) {
      continue;
    }
    MeasuredValue mv;
    mv.ioa = it->second;
    mv.value = value;
    mv.quality = toIec104Quality(update.quality());
    out.emplace_back(std::move(mv));
  }
  return out;
}

}  // namespace IEC104
