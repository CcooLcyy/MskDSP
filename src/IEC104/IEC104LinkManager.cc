#include "IEC104LinkManager.h"

#include <cstdint>
#include <format>
#include <string>
#include <utility>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

namespace IEC104 {
namespace {
grpc::Status makeNotFound(const std::string &connName) {
  return grpc::Status(grpc::StatusCode::NOT_FOUND, std::format("link not found: {}", connName));
}

constexpr uint32_t kMaxAsduBytes = 249;
constexpr uint32_t kMinMeasuredValueAsduBytes = 14;
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
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "conn_name is required");
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::validateLinkConfig(const IEC104Proto::LinkConfig &config) {
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
  if (config.telemetry_max_asdu_bytes() > 0) {
    if (config.telemetry_max_asdu_bytes() > kMaxAsduBytes) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "telemetry_max_asdu_bytes must be <= 249");
    }
    if (config.telemetry_max_asdu_bytes() < kMinMeasuredValueAsduBytes) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "telemetry_max_asdu_bytes must be >= 14");
    }
  }
  return grpc::Status::OK;
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
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
  }
  if (local.port() == 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "local.port is required");
  }
  if (local.port() > 65535) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "local.port must be <= 65535");
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
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::format("invalid local.ip: {}", ip));
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
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::format("invalid local.ip: {}", ep.ip));
    }
    endpoint = tcp::endpoint(addr, static_cast<uint16_t>(ep.port));
  }

  acceptor.open(endpoint.protocol(), ec);
  if (ec) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::format("acceptor open failed: {}", ec.message()));
  }
  acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
  if (ec) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::format("acceptor set_option failed: {}", ec.message()));
  }
  acceptor.bind(endpoint, ec);
  if (ec) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::format("acceptor bind failed: {}", ec.message()));
  }
  acceptor.listen(tcp::acceptor::max_listen_connections, ec);
  if (ec) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::format("acceptor listen failed: {}", ec.message()));
  }

  return grpc::Status::OK;
}

grpc::Status LinkManager::fillLinkInfoLocked(const LinkRuntime &link, IEC104Proto::LinkInfo *out) const {
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

grpc::Status LinkManager::UpsertLink(const IEC104Proto::UpsertLinkRequest &request, IEC104Proto::LinkInfo *out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
  }
  auto status = validateLinkConfig(request.config());
  if (!status.ok()) {
    return status;
  }
  const auto connName = request.config().conn_name();
  const bool isServer = (request.config().role() == IEC104Proto::ROLE_SERVER);
  ListenEndpoint desiredListen;
  if (isServer) {
    status = makeListenEndpoint(request.config().local(), &desiredListen);
    if (!status.ok()) {
      return status;
    }
  }

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
                  std::format("listen endpoint {} conflicts with {} ({})",
                              listenEndpointToString(desiredListen),
                              otherName,
                              listenEndpointToString(otherListen)));
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

      it->second.config = request.config();
      it->second.lastError.clear();
      return fillLinkInfoLocked(it->second, out);
    }

    if (pendingCreateByName_.contains(connName) || reservedServerListenByName_.contains(connName)) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name already exists");
    }

    if (isServer) {
      for (const auto &[otherName, otherListen] : reservedServerListenByName_) {
        if (listenEndpointsConflict(otherListen, desiredListen)) {
          return grpc::Status(
              grpc::StatusCode::ALREADY_EXISTS,
              std::format("listen endpoint {} conflicts with {} ({})",
                          listenEndpointToString(desiredListen),
                          otherName,
                          listenEndpointToString(otherListen)));
        }
      }
      status = checkSystemListenAvailable(desiredListen);
      if (!status.ok()) {
        return status;
      }
      // Reserve early to avoid races while we call DataCenter.
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
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name already exists");
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
    return grpc::Status(grpc::StatusCode::INTERNAL, "DataCenter returned conn_id=0");
  }

  std::lock_guard<std::mutex> lock(mu_);
  pendingCreateByName_.erase(connName);
  auto [it, inserted] = linksByName_.try_emplace(connName);
  if (!inserted) {
    // Another UpsertLink raced this one and already created the link.
    if (request.create_only()) {
      if (isServer && it->second.config.role() != IEC104Proto::ROLE_SERVER) {
        reservedServerListenByName_.erase(connName);
      }
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name already exists");
    }
    if (it->second.state == IEC104Proto::LINK_STATE_RUNNING) {
      if (isServer && it->second.config.role() != IEC104Proto::ROLE_SERVER) {
        reservedServerListenByName_.erase(connName);
      }
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "stop link before updating config");
    }
    if (it->second.state == IEC104Proto::LINK_STATE_PENDING_DELETE) {
      if (isServer && it->second.config.role() != IEC104Proto::ROLE_SERVER) {
        reservedServerListenByName_.erase(connName);
      }
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "link is pending delete");
    }

    it->second.config = request.config();
    it->second.lastError.clear();
    if (isServer) {
      reservedServerListenByName_[connName] = desiredListen;
    } else {
      reservedServerListenByName_.erase(connName);
    }
    return fillLinkInfoLocked(it->second, out);
  }

  it->second.config = request.config();
  it->second.connId = connInfo.conn_id();
  it->second.state = IEC104Proto::LINK_STATE_STOPPED;
  it->second.lastError.clear();
  return fillLinkInfoLocked(it->second, out);
}

grpc::Status LinkManager::GetLink(const std::string &connName, IEC104Proto::LinkInfo *out) const {
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

grpc::Status LinkManager::ListLinks(IEC104Proto::ListLinksResponse *out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out is null");
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
  if (link->config.role() == IEC104Proto::ROLE_CLIENT) {
    link->transport->SetMeasuredValueCallback([this, connName](const MeasuredValue &mv) {
      (void)handleClientMeasuredValue(connName, mv);
    });
  }
  if (link->config.role() == IEC104Proto::ROLE_SERVER) {
    link->transport->SetInterrogationSnapshotProvider([this, connName]() { return buildInterrogationSnapshot(connName); });
  }
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
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "link is pending delete");
  }
  if (link.state == IEC104Proto::LINK_STATE_RUNNING) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "link already running");
  }

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
  if (link.config.role() == IEC104Proto::ROLE_SERVER) {
    startDataCenterSubscribeLocked(connName, &link);
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

grpc::Status LinkManager::GetPointTable(const std::string &connName, IEC104Proto::PointTable *out) const {
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

}  // namespace IEC104
