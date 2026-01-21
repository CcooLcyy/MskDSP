#include "IEC104LinkManager.h"

#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

#include "Logger.h"

namespace IEC104 {
namespace {
grpc::Status makeNotFound(const std::string &connName) {
  return grpc::Status(grpc::StatusCode::NOT_FOUND, std::format("未找到链路: {}", connName));
}

constexpr uint32_t kMaxAsduBytes = 249;
constexpr uint32_t kMinMeasuredValueAsduBytes = 21;
constexpr const char *kDefaultTimeSyncTag = "__time_sync__";
}  // namespace

LinkManager::LinkManager(std::string moduleName) :
  dataCenter_(std::move(moduleName)) {}

void LinkManager::setDataCenterServerAddress(std::string address) {
  dataCenter_.setServerAddress(std::move(address));
}

void LinkManager::setDataCenterStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub) {
  dataCenter_.setStub(std::move(stub));
}

grpc::Status LinkManager::validateConnName(const std::string &connName) {
  if (connName.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_name 不能为空");
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::validateLinkConfig(const IEC104Proto::LinkConfig &config) {
  auto s = validateConnName(config.conn_name());
  if (!s.ok()) {
    return s;
  }
  if (config.role() != IEC104Proto::ROLE_SERVER && config.role() != IEC104Proto::ROLE_CLIENT) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "role 不能为空");
  }
  if (config.station_role() != IEC104Proto::STATION_ROLE_UNSPECIFIED &&
      config.station_role() != IEC104Proto::STATION_ROLE_MASTER &&
      config.station_role() != IEC104Proto::STATION_ROLE_SLAVE) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "station_role 非法");
  }
  if (config.role() == IEC104Proto::ROLE_SERVER) {
    if (config.local().port() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "role=ROLE_SERVER 时 local.port 不能为空");
    }
  }
  if (config.role() == IEC104Proto::ROLE_CLIENT) {
    if (config.remote().ip().empty() || config.remote().port() == 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "role=ROLE_CLIENT 时 remote.ip/port 不能为空");
    }
  }
  if (config.ca() > 65535) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "ca 必须 <= 65535");
  }
  if (config.oa() > 255) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "oa 必须 <= 255");
  }
  if (config.telemetry_max_asdu_bytes() > 0) {
    if (config.telemetry_max_asdu_bytes() > kMaxAsduBytes) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "telemetry_max_asdu_bytes 必须 <= 249");
    }
    if (config.telemetry_max_asdu_bytes() < kMinMeasuredValueAsduBytes) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "telemetry_max_asdu_bytes 必须 >= 21");
    }
  }
  return grpc::Status::OK;
}

IEC104Proto::StationRole LinkManager::normalizeStationRole(const IEC104Proto::LinkConfig &config) {
  if (config.station_role() != IEC104Proto::STATION_ROLE_UNSPECIFIED) {
    return config.station_role();
  }
  if (config.role() == IEC104Proto::ROLE_CLIENT) {
    return IEC104Proto::STATION_ROLE_MASTER;
  }
  if (config.role() == IEC104Proto::ROLE_SERVER) {
    return IEC104Proto::STATION_ROLE_SLAVE;
  }
  return IEC104Proto::STATION_ROLE_UNSPECIFIED;
}

bool LinkManager::isMasterStation(const IEC104Proto::LinkConfig &config) {
  return normalizeStationRole(config) == IEC104Proto::STATION_ROLE_MASTER;
}

bool LinkManager::isSlaveStation(const IEC104Proto::LinkConfig &config) {
  return normalizeStationRole(config) == IEC104Proto::STATION_ROLE_SLAVE;
}

const char *LinkManager::stationRoleToString(IEC104Proto::StationRole role) {
  switch (role) {
    case IEC104Proto::STATION_ROLE_MASTER:
      return "主站";
    case IEC104Proto::STATION_ROLE_SLAVE:
      return "从站";
    default:
      return "未指定";
  }
}

bool LinkManager::listenEndpointsConflict(const ListenEndpoint &a, const ListenEndpoint &b) {
  if (a.port != b.port) {
    return false;
  }
  if (a.any || b.any) {
    return true;
  }
  return a.ip == b.ip;
}

bool LinkManager::listenEndpointsEqual(const ListenEndpoint &a, const ListenEndpoint &b) {
  return a.any == b.any && a.port == b.port && a.ip == b.ip;
}

std::string LinkManager::listenEndpointToString(const ListenEndpoint &ep) {
  const auto ip = ep.any ? std::string("0.0.0.0") : (ep.ip.empty() ? std::string("0.0.0.0") : ep.ip);
  return std::format("{}:{}", ip, ep.port);
}

grpc::Status LinkManager::makeListenEndpoint(const IEC104Proto::Endpoint &local, ListenEndpoint *out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  if (local.port() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "local.port 不能为空");
  }
  if (local.port() > 65535) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "local.port 必须 <= 65535");
  }

  out->port = local.port();
  const auto &ip = local.ip();
  if (ip.empty() || ip == "0.0.0.0") {
    out->any = true;
    out->ip = "0.0.0.0";
    return grpc::Status::OK;
  }

  boost::system::error_code ec;
  auto addr = boost::asio::ip::make_address(ip, ec);
  if (ec) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::format("local.ip 非法: {}", ip));
  }

  out->any = false;
  out->ip = addr.to_string();
  return grpc::Status::OK;
}

grpc::Status LinkManager::checkSystemListenAvailable(const ListenEndpoint &ep) {
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;

  asio::io_context io;
  tcp::acceptor acceptor(io);

  boost::system::error_code ec;
  tcp::endpoint endpoint;
  if (ep.any) {
    endpoint = tcp::endpoint(asio::ip::address_v4::any(), static_cast<uint16_t>(ep.port));
  } else {
    auto addr = asio::ip::make_address(ep.ip, ec);
    if (ec) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::format("local.ip 非法: {}", ep.ip));
    }
    endpoint = tcp::endpoint(addr, static_cast<uint16_t>(ep.port));
  }

  acceptor.open(endpoint.protocol(), ec);
  if (ec) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::format("acceptor 打开失败: {}", ec.message()));
  }
  acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
  if (ec) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::format("acceptor 设置参数失败: {}", ec.message()));
  }
  acceptor.bind(endpoint, ec);
  if (ec) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::format("acceptor 绑定失败: {}", ec.message()));
  }
  acceptor.listen(tcp::acceptor::max_listen_connections, ec);
  if (ec) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::format("acceptor 监听失败: {}", ec.message()));
  }

  return grpc::Status::OK;
}

grpc::Status LinkManager::fillLinkInfoLocked(const LinkRuntime &link, IEC104Proto::LinkInfo *out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  out->Clear();
  *out->mutable_config() = link.config;
  out->set_conn_id(link.connId);
  out->set_state(link.state);
  out->set_last_error(link.lastError);
  return grpc::Status::OK;
}

grpc::Status LinkManager::UpsertLink(const IEC104Proto::UpsertLinkRequest &request, IEC104Proto::LinkInfo *out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  IEC104Proto::LinkConfig config = request.config();
  if (config.time_sync_tag().empty()) {
    config.set_time_sync_tag(kDefaultTimeSyncTag);
  }
  auto status = validateLinkConfig(config);
  if (!status.ok()) {
    return status;
  }
  if (config.station_role() == IEC104Proto::STATION_ROLE_UNSPECIFIED) {
    const auto stationRole = normalizeStationRole(config);
    if (stationRole != IEC104Proto::STATION_ROLE_UNSPECIFIED) {
      config.set_station_role(stationRole);
      LOG_INFO("IEC104 未指定站点角色，已按传输角色默认: conn_name={}, station_role={}",
               config.conn_name(),
               stationRoleToString(stationRole));
    }
  }
  const auto connName = config.conn_name();
  const bool isServer = (config.role() == IEC104Proto::ROLE_SERVER);
  ListenEndpoint desiredListen;
  if (isServer) {
    status = makeListenEndpoint(config.local(), &desiredListen);
    if (!status.ok()) {
      return status;
    }
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it != linksByName_.end()) {
      if (request.create_only()) {
        return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name 已存在");
      }
      if (it->second.state == IEC104Proto::LINK_STATE_RUNNING) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "更新配置前请先停止链路");
      }
      if (it->second.state == IEC104Proto::LINK_STATE_PENDING_DELETE) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路处于待删除状态");
      }

      const bool wasServer = (it->second.config.role() == IEC104Proto::ROLE_SERVER);
      if (isServer) {
        ListenEndpoint currentListen;
        bool needCheck = true;
        if (wasServer) {
          auto rIt = reservedServerListenByName_.find(connName);
          if (rIt != reservedServerListenByName_.end()) {
            currentListen = rIt->second;
            needCheck = !listenEndpointsEqual(currentListen, desiredListen);
          }
        }

        if (needCheck) {
          for (const auto &[otherName, otherListen] : reservedServerListenByName_) {
            if (otherName == connName) {
              continue;
            }
            if (listenEndpointsConflict(otherListen, desiredListen)) {
              return grpc::Status(
                  grpc::StatusCode::ALREADY_EXISTS,
                  std::format("监听端点 {} 与 {} 冲突 ({})", listenEndpointToString(desiredListen), otherName, listenEndpointToString(otherListen)));
            }
          }
          status = checkSystemListenAvailable(desiredListen);
          if (!status.ok()) {
            return status;
          }
        }

        reservedServerListenByName_[connName] = desiredListen;
      } else if (wasServer) {
        reservedServerListenByName_.erase(connName);
      }

      it->second.config = config;
      it->second.lastError.clear();
      return fillLinkInfoLocked(it->second, out);
    }

    if (pendingCreateByName_.contains(connName) || reservedServerListenByName_.contains(connName)) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name 已存在");
    }

    if (isServer) {
      for (const auto &[otherName, otherListen] : reservedServerListenByName_) {
        if (listenEndpointsConflict(otherListen, desiredListen)) {
          return grpc::Status(
              grpc::StatusCode::ALREADY_EXISTS,
              std::format("监听端点 {} 与 {} 冲突 ({})", listenEndpointToString(desiredListen), otherName, listenEndpointToString(otherListen)));
        }
      }
      status = checkSystemListenAvailable(desiredListen);
      if (!status.ok()) {
        return status;
      }
      // 提前预留，避免调用 DataCenter 时发生竞态。
      reservedServerListenByName_[connName] = desiredListen;
    }
    pendingCreateByName_.emplace(connName);
  }

  auto rollbackPendingCreate = [this, &connName, isServer]() {
    std::lock_guard<std::mutex> lock(mu_);
    pendingCreateByName_.erase(connName);
    if (isServer) {
      reservedServerListenByName_.erase(connName);
    }
  };

  if (request.create_only()) {
    bool exists = false;
    status = dataCenter_.ConnectionExists(connName, &exists);
    if (!status.ok()) {
      rollbackPendingCreate();
      return status;
    }
    if (exists) {
      rollbackPendingCreate();
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name 已存在");
    }
  }

  DataCenterProto::ConnectionInfo connInfo;
  status = dataCenter_.GetOrCreateConnection(connName, &connInfo);
  if (!status.ok()) {
    rollbackPendingCreate();
    return status;
  }
  if (connInfo.conn_id() == 0) {
    rollbackPendingCreate();
    return grpc::Status(grpc::StatusCode::INTERNAL, "DataCenter 返回 conn_id=0");
  }

  std::lock_guard<std::mutex> lock(mu_);
  pendingCreateByName_.erase(connName);
  auto [it, inserted] = linksByName_.try_emplace(connName);
  if (!inserted) {
    // 另一个 UpsertLink 与此请求并发并已创建链路。
    if (request.create_only()) {
      if (isServer && it->second.config.role() != IEC104Proto::ROLE_SERVER) {
        reservedServerListenByName_.erase(connName);
      }
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name 已存在");
    }
    if (it->second.state == IEC104Proto::LINK_STATE_RUNNING) {
      if (isServer && it->second.config.role() != IEC104Proto::ROLE_SERVER) {
        reservedServerListenByName_.erase(connName);
      }
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "更新配置前请先停止链路");
    }
    if (it->second.state == IEC104Proto::LINK_STATE_PENDING_DELETE) {
      if (isServer && it->second.config.role() != IEC104Proto::ROLE_SERVER) {
        reservedServerListenByName_.erase(connName);
      }
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路处于待删除状态");
    }

    it->second.config = config;
    it->second.lastError.clear();
    if (isServer) {
      reservedServerListenByName_[connName] = desiredListen;
    } else {
      reservedServerListenByName_.erase(connName);
    }
    return fillLinkInfoLocked(it->second, out);
  }

  it->second.config = config;
  it->second.connId = connInfo.conn_id();
  it->second.state = IEC104Proto::LINK_STATE_STOPPED;
  it->second.lastError.clear();
  return fillLinkInfoLocked(it->second, out);
}

grpc::Status LinkManager::GetLink(const std::string &connName, IEC104Proto::LinkInfo *out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
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

grpc::Status LinkManager::ListLinks(IEC104Proto::ListLinksResponse *out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  std::lock_guard<std::mutex> lock(mu_);
  out->Clear();
  for (const auto &[_, link] : linksByName_) {
    auto *elem = out->add_links();
    fillLinkInfoLocked(link, elem);
  }
  return grpc::Status::OK;
}

void LinkManager::configureTransportCallbacksLocked(const std::string &connName, LinkRuntime *link) {
  if (link == nullptr || !link->transport) {
    return;
  }
  if (isMasterStation(link->config)) {
    link->transport->SetPointValueCallback([this, connName](const PointValue &pv) {
      (void)handleClientPointValue(connName, pv);
    });
  }
  if (isSlaveStation(link->config)) {
    link->transport->SetInterrogationSnapshotProvider([this, connName]() { return buildInterrogationSnapshot(connName); });
  }
  link->transport->SetTimeSyncCallback([this, connName](int64_t tsMs) {
    (void)handleTimeSyncCommand(connName, tsMs);
  });
}

grpc::Status LinkManager::StartLink(const std::string &connName) {
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto it = linksByName_.find(connName);
  if (it == linksByName_.end()) {
    return makeNotFound(connName);
  }
  auto &link = it->second;
  if (link.state == IEC104Proto::LINK_STATE_PENDING_DELETE) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路处于待删除状态");
  }
  if (link.state == IEC104Proto::LINK_STATE_RUNNING) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路已在运行");
  }

  link.lastReportedByTag.clear();
  link.transport = std::make_unique<TcpLink>(link.config);
  configureTransportCallbacksLocked(connName, &link);

  status = link.transport->Start();
  if (!status.ok()) {
    link.lastError = status.error_message();
    link.transport.reset();
    return status;
  }

  link.state = IEC104Proto::LINK_STATE_RUNNING;
  link.lastError.clear();
  if (isSlaveStation(link.config)) {
    startDataCenterSubscribeLocked(connName, &link);
  } else if (isMasterStation(link.config)) {
    startTimeSyncSubscribeLocked(connName, &link);
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::StopLink(const std::string &connName) {
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
    stopTimeSyncSubscribeLocked(&it->second);
    transport = std::move(it->second.transport);
    it->second.state = pendingDelete ? IEC104Proto::LINK_STATE_PENDING_DELETE : IEC104Proto::LINK_STATE_STOPPED;
  }

  if (transport) {
    transport->Stop();
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::DeleteLink(const std::string &connName) {
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
  reservedServerListenByName_.erase(connName);
  return grpc::Status::OK;
}

grpc::Status LinkManager::UpsertPointTable(const IEC104Proto::UpsertPointTableRequest &request) {
  auto status = validateConnName(request.conn_name());
  if (!status.ok()) {
    return status;
  }

  uint32_t connId = 0;
  IEC104Proto::LinkConfig config;
  PointTable current;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(request.conn_name());
    if (it == linksByName_.end()) {
      return makeNotFound(request.conn_name());
    }
    if (it->second.state == IEC104Proto::LINK_STATE_RUNNING) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "更新点表前请先停止链路");
    }
    if (it->second.state == IEC104Proto::LINK_STATE_PENDING_DELETE) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路处于待删除状态");
    }
    connId = it->second.connId;
    config = it->second.config;
    current = it->second.pointTable;
  }

  PointTable next = current;
  status = next.Upsert(request.points(), request.replace());
  if (!status.ok()) {
    return status;
  }

  auto tags = next.Tags();
  const auto timeSyncTag = normalizeTimeSyncTag(config);
  if (!timeSyncTag.empty() && std::find(tags.begin(), tags.end(), timeSyncTag) == tags.end()) {
    tags.emplace_back(timeSyncTag);
    std::sort(tags.begin(), tags.end());
  }
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
  it->second.lastReportedByTag.clear();
  return grpc::Status::OK;
}

grpc::Status LinkManager::GetPointTable(const std::string &connName, IEC104Proto::PointTable *out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
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

grpc::Status LinkManager::SendTimeSync(const std::string &connName, int64_t tsMs) {
  auto status = validateConnName(connName);
  if (!status.ok()) {
    return status;
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto it = linksByName_.find(connName);
  if (it == linksByName_.end()) {
    return makeNotFound(connName);
  }
  if (it->second.state != IEC104Proto::LINK_STATE_RUNNING || !it->second.transport) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路未运行");
  }
  if (!isMasterStation(it->second.config)) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "仅主站允许主动对时");
  }

  LOG_INFO("IEC104 触发主动对时: conn_name={}, ts_ms={}", connName, tsMs);
  it->second.transport->SendTimeSync(tsMs);
  return grpc::Status::OK;
}

std::string LinkManager::normalizeTimeSyncTag(const IEC104Proto::LinkConfig &config) {
  if (!config.time_sync_tag().empty()) {
    return config.time_sync_tag();
  }
  return kDefaultTimeSyncTag;
}

}  // namespace IEC104
