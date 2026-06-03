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
#include <vector>

#include "IEC104LinkStore.h"
#include "IEC104PointTableStore.h"
#include "Logger.h"

namespace IEC104 {
namespace {
grpc::Status makeNotFound(const std::string &connName) {
  return grpc::Status(grpc::StatusCode::NOT_FOUND, std::format("未找到链路: {}", connName));
}

constexpr uint32_t kMaxAsduBytes = 249;
constexpr uint32_t kMinMeasuredValueAsduBytes = 21;
constexpr const char *kDefaultTimeSyncTag = "__time_sync__";

std::vector<std::string> buildDataCenterTags(const IEC104::PointTable &pointTable, const IEC104Proto::LinkConfig &config) {
  auto tags = pointTable.Tags();
  const auto timeSyncTag = config.time_sync_tag().empty() ? std::string(kDefaultTimeSyncTag) : config.time_sync_tag();
  if (!timeSyncTag.empty() && std::find(tags.begin(), tags.end(), timeSyncTag) == tags.end()) {
    tags.emplace_back(timeSyncTag);
    std::sort(tags.begin(), tags.end());
  }
  return tags;
}

bool renamePersistedLinkConfig(IEC104Proto::LinksConfig *config,
                               const std::string &oldConnName,
                               const std::string &newConnName) {
  if (config == nullptr) {
    return false;
  }
  for (auto &link : *config->mutable_links()) {
    if (link.config().conn_name() != oldConnName) {
      continue;
    }
    link.mutable_config()->set_conn_name(newConnName);
    return true;
  }
  return false;
}

void renamePersistedPointTableConfig(IEC104Proto::PointTablesConfig *config,
                                     const std::string &oldConnName,
                                     const std::string &newConnName) {
  if (config == nullptr) {
    return;
  }
  for (auto &table : *config->mutable_point_tables()) {
    if (table.conn_name() == oldConnName) {
      table.set_conn_name(newConnName);
      return;
    }
  }
}
}  // namespace

LinkManager::LinkManager(std::string moduleName, std::filesystem::path configDbPath) :
  dataCenter_(std::move(moduleName)) {
  if (configDbPath.empty()) {
    return;
  }

  linkStore_ = std::make_unique<IEC104LinkStore>(configDbPath);
  pointTableStore_ = std::make_unique<IEC104PointTableStore>(std::move(configDbPath));
  loadPersistedConfig("构造阶段");
}

LinkManager::~LinkManager() = default;

void LinkManager::setDataCenterServerAddress(std::string address) {
  dataCenter_.setServerAddress(std::move(address));
  bool needReload = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    needReload = persistenceEnabled() && linksByName_.empty() && reservedServerListenByName_.empty() && pendingCreateByName_.empty();
  }
  if (needReload) {
    loadPersistedConfig("设置 DataCenter 地址后重试");
  }
}

void LinkManager::setDataCenterStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub) {
  dataCenter_.setStub(std::move(stub));
  bool needReload = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    needReload = persistenceEnabled() && linksByName_.empty() && reservedServerListenByName_.empty() && pendingCreateByName_.empty();
  }
  if (needReload) {
    loadPersistedConfig("设置 DataCenter Stub 后重试");
  }
}

void LinkManager::LoadPersistedConfig() {
  bool canReload = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    canReload = persistenceEnabled();
  }
  if (canReload) {
    LOG_INFO("IEC104 模块启动阶段准备重新加载本地持久化配置，以最终恢复结果为准");
    loadPersistedConfig("模块启动阶段重试");
  } else {
    LOG_INFO("IEC104 模块启动阶段未启用本地持久化恢复，跳过重载");
  }
  TryAutoStartReadyLinks("模块启动后恢复检查");
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
  if (config.point_max_asdu_bytes() > 0) {
    if (config.point_max_asdu_bytes() > kMaxAsduBytes) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "point_max_asdu_bytes 必须 <= 249");
    }
    if (config.point_max_asdu_bytes() < kMinMeasuredValueAsduBytes) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "point_max_asdu_bytes 必须 >= 21");
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

grpc::Status LinkManager::checkStartPreconditionsLocked(const LinkRuntime &link) const {
  if (link.state == IEC104Proto::LINK_STATE_PENDING_DELETE) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路处于待删除状态");
  }
  const auto pointCount = link.pointTable.Tags().size();
  if (!link.pointTableConfigured || pointCount == 0) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路点表未就绪，当前规则要求链路非待删除且至少存在 1 条通过校验的点表记录后才启动连接功能");
  }
  return grpc::Status::OK;
}

grpc::Status LinkManager::tryAutoStartLink(const std::string &connName, std::string_view trigger) {
  size_t pointCount = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(connName);
    if (it == linksByName_.end()) {
      return makeNotFound(connName);
    }
    pointCount = it->second.pointTable.Tags().size();
    if (it->second.state == IEC104Proto::LINK_STATE_RUNNING) {
      LOG_INFO("IEC104 自动启动链路跳过: conn_name={}, 触发来源={}, 原因=链路已在运行", connName, trigger);
      return grpc::Status::OK;
    }
    auto status = checkStartPreconditionsLocked(it->second);
    if (!status.ok()) {
      it->second.lastError = status.error_message();
      LOG_INFO("IEC104 自动启动链路跳过: conn_name={}, 触发来源={}, 点数={}, 原因={}", connName, trigger, pointCount, status.error_message());
      return status;
    }
  }

  LOG_INFO("IEC104 自动启动链路: conn_name={}, 触发来源={}, 规则=链路非待删除且至少存在 1 条有效点表记录, 点数={}", connName, trigger, pointCount);
  auto status = StartLink(connName);
  if (!status.ok()) {
    LOG_WARNING("IEC104 自动启动链路失败: conn_name={}, 触发来源={}, 原因={}", connName, trigger, status.error_message());
  } else {
    LOG_INFO("IEC104 自动启动链路成功: conn_name={}, 触发来源={}", connName, trigger);
  }
  return status;
}

void LinkManager::TryAutoStartReadyLinks(std::string_view trigger) {
  std::vector<std::string> connNames;
  {
    std::lock_guard<std::mutex> lock(mu_);
    connNames.reserve(linksByName_.size());
    for (const auto &[connName, link] : linksByName_) {
      if (link.state == IEC104Proto::LINK_STATE_STOPPED || link.state == IEC104Proto::LINK_STATE_RUNNING) {
        connNames.push_back(connName);
      }
    }
  }

  if (connNames.empty()) {
    LOG_INFO("IEC104 自动启动检查完成: 触发来源={}, 当前无可评估链路", trigger);
    return;
  }

  for (const auto &connName : connNames) {
    (void)tryAutoStartLink(connName, trigger);
  }
}

void LinkManager::loadPersistedConfig(std::string_view trigger) {
  if (!persistenceEnabled()) {
    return;
  }

  LOG_INFO("IEC104 开始加载本地持久化配置: 触发来源={}", trigger);

  IEC104Proto::LinksConfig linksConfig;
  auto status = linkStore_->Load(&linksConfig);
  if (!status.ok()) {
    LOG_ERROR("IEC104 加载本地链路配置失败: {}", status.error_message());
    return;
  }

  IEC104Proto::PointTablesConfig pointTablesConfig;
  status = pointTableStore_->Load(&pointTablesConfig);
  if (!status.ok()) {
    LOG_ERROR("IEC104 加载本地点表配置失败: {}", status.error_message());
    return;
  }
  LOG_INFO("IEC104 持久化配置载入摘要: 触发来源={}, 链路记录数={}, 点表记录数={}", trigger, linksConfig.links_size(), pointTablesConfig.point_tables_size());

  std::unordered_map<std::string, IEC104Proto::PointTable> pointTablesByConn;
  std::unordered_map<std::string, int> pointTableIndexByConn;
  pointTablesByConn.reserve(static_cast<size_t>(pointTablesConfig.point_tables_size()));
  pointTableIndexByConn.reserve(static_cast<size_t>(pointTablesConfig.point_tables_size()));
  std::vector<bool> keepPointTables(static_cast<size_t>(pointTablesConfig.point_tables_size()), true);
  for (int i = 0; i < pointTablesConfig.point_tables_size(); ++i) {
    const auto &table = pointTablesConfig.point_tables(i);
    pointTablesByConn.emplace(table.conn_name(), table);
    pointTableIndexByConn.emplace(table.conn_name(), i);
  }

  if (linksConfig.links_size() == 0) {
    if (!pointTablesByConn.empty()) {
      LOG_WARNING("IEC104 未找到链路持久化配置，但存在 {} 条点表持久化配置，准备清理孤立点表", pointTablesByConn.size());
      auto saveStatus = pointTableStore_->Save(IEC104Proto::PointTablesConfig());
      if (!saveStatus.ok()) {
        LOG_ERROR("IEC104 清理孤立点表持久化配置失败: {}", saveStatus.error_message());
      }
    } else {
      LOG_INFO("IEC104 未找到链路持久化配置");
    }
    return;
  }

  std::unordered_map<std::string, LinkRuntime> restoredLinks;
  std::unordered_map<std::string, ListenEndpoint> restoredServerListenByName;
  restoredLinks.reserve(static_cast<size_t>(linksConfig.links_size()));
  restoredServerListenByName.reserve(static_cast<size_t>(linksConfig.links_size()));
  std::unordered_map<std::string, uint32_t> updatedConnIds;
  updatedConnIds.reserve(static_cast<size_t>(linksConfig.links_size()));
  std::vector<bool> keepLinks(static_cast<size_t>(linksConfig.links_size()), true);
  bool needResaveLinks = false;
  bool needResavePointTables = false;
  size_t failedCount = 0;
  size_t dataCenterFailureCount = 0;

  auto removePointTableForConn = [&](const std::string &connName) {
    auto indexIt = pointTableIndexByConn.find(connName);
    if (indexIt != pointTableIndexByConn.end()) {
      const auto keepIndex = static_cast<size_t>(indexIt->second);
      if (keepPointTables[keepIndex]) {
        keepPointTables[keepIndex] = false;
        needResavePointTables = true;
      }
    }
    pointTablesByConn.erase(connName);
  };

  auto removeLinkRecord = [&](size_t index, const std::string &connName) {
    if (keepLinks[index]) {
      keepLinks[index] = false;
      needResaveLinks = true;
    }
    if (!connName.empty()) {
      removePointTableForConn(connName);
    }
  };

  for (int i = 0; i < linksConfig.links_size(); ++i) {
    const auto &persisted = linksConfig.links(i);
    if (!persisted.has_config()) {
      LOG_WARNING("IEC104 跳过空链路持久化记录");
      removeLinkRecord(static_cast<size_t>(i), "");
      continue;
    }

    auto normalized = persisted.config();
    if (normalized.time_sync_tag().empty()) {
      normalized.set_time_sync_tag(kDefaultTimeSyncTag);
    }
    if (normalized.station_role() == IEC104Proto::STATION_ROLE_UNSPECIFIED) {
      const auto stationRole = normalizeStationRole(normalized);
      if (stationRole != IEC104Proto::STATION_ROLE_UNSPECIFIED) {
        normalized.set_station_role(stationRole);
      }
    }
    const auto connName = normalized.conn_name();
    size_t persistedPointCount = 0;
    if (auto persistedTableIt = pointTablesByConn.find(connName); persistedTableIt != pointTablesByConn.end()) {
      persistedPointCount = static_cast<size_t>(persistedTableIt->second.points_size());
    }
    LOG_INFO("IEC104 开始恢复链路持久化记录: 触发来源={}, conn_name={}, 持久化conn_id={}, 待删除={}, 持久化点数={}", trigger, connName, persisted.conn_id(), persisted.pending_delete(), persistedPointCount);

    status = validateLinkConfig(normalized);
    if (!status.ok()) {
      ++failedCount;
      LOG_ERROR("IEC104 链路持久化配置非法，已跳过: conn_name={}, 原因={}", connName, status.error_message());
      removeLinkRecord(static_cast<size_t>(i), connName);
      continue;
    }

    ListenEndpoint listen;
    if (normalized.role() == IEC104Proto::ROLE_SERVER) {
      status = makeListenEndpoint(normalized.local(), &listen);
      if (!status.ok()) {
        ++failedCount;
        LOG_ERROR("IEC104 恢复链路时监听端点非法，已跳过: conn_name={}, 原因={}", connName, status.error_message());
        removeLinkRecord(static_cast<size_t>(i), connName);
        continue;
      }

      bool conflicted = false;
      std::string conflictName;
      ListenEndpoint conflictListen;
      for (const auto &[otherName, otherListen] : restoredServerListenByName) {
        if (listenEndpointsConflict(otherListen, listen)) {
          conflicted = true;
          conflictName = otherName;
          conflictListen = otherListen;
          break;
        }
      }
      if (conflicted) {
        ++failedCount;
        LOG_ERROR("IEC104 恢复链路时监听端点冲突，已跳过: conn_name={}, 对端链路={}, 当前监听={}, 对端监听={}", connName, conflictName, listenEndpointToString(listen), listenEndpointToString(conflictListen));
        removeLinkRecord(static_cast<size_t>(i), connName);
        continue;
      }
    }

    DataCenterProto::ConnectionInfo connInfo;
    status = dataCenter_.GetOrCreateConnection(connName, &connInfo);
    if (!status.ok()) {
      ++failedCount;
      ++dataCenterFailureCount;
      LOG_ERROR("IEC104 恢复链路时获取 DataCenter 连接失败: 触发来源={}, conn_name={}, 原因={}, 本地点表点数={}", trigger, connName, status.error_message(), persistedPointCount);
      pointTablesByConn.erase(connName);
      continue;
    }
    if (connInfo.conn_id() == 0) {
      ++failedCount;
      LOG_ERROR("IEC104 恢复链路时 DataCenter 返回无效 conn_id，已跳过: 触发来源={}, conn_name={}, 本地点表点数={}", trigger, connName, persistedPointCount);
      pointTablesByConn.erase(connName);
      continue;
    }
    if (persisted.conn_id() != connInfo.conn_id()) {
      LOG_WARNING("IEC104 恢复链路时发现 conn_id 已变化: conn_name={}, 持久化conn_id={}, 当前conn_id={}", connName, persisted.conn_id(), connInfo.conn_id());
      updatedConnIds[connName] = connInfo.conn_id();
      needResaveLinks = true;
    }

    LinkRuntime runtime;
    runtime.config = normalized;
    runtime.connId = connInfo.conn_id();
    runtime.state = persisted.pending_delete() ? IEC104Proto::LINK_STATE_PENDING_DELETE : IEC104Proto::LINK_STATE_STOPPED;
    runtime.lastError.clear();
    runtime.lastReportedByTag.clear();

    size_t pointCount = 0;
    auto tableIt = pointTablesByConn.find(connName);
    if (tableIt != pointTablesByConn.end()) {
      PointTable pointTable;
      auto pointStatus = pointTable.Upsert(tableIt->second.points(), true);
      if (!pointStatus.ok()) {
        LOG_ERROR("IEC104 恢复点表失败，已忽略该点表: conn_name={}, 原因={}", connName, pointStatus.error_message());
        removePointTableForConn(connName);
      } else {
        runtime.pointTable = std::move(pointTable);
        runtime.pointTableConfigured = true;
        pointCount = static_cast<size_t>(tableIt->second.points_size());
        pointTablesByConn.erase(tableIt);
      }
    } else {
      LOG_WARNING("IEC104 恢复链路时未找到对应点表，链路将保持已停止等待后续补全: conn_name={}", connName);
    }

    auto tags = buildDataCenterTags(runtime.pointTable, runtime.config);
    auto syncStatus = dataCenter_.UpsertConnTags(runtime.connId, tags, true);
    if (!syncStatus.ok()) {
      LOG_ERROR("IEC104 恢复链路时同步 DataCenter 连接标签注册表失败: conn_name={}, conn_id={}, 原因={}", connName, runtime.connId, syncStatus.error_message());
    }

    if (normalized.role() == IEC104Proto::ROLE_SERVER) {
      restoredServerListenByName[connName] = listen;
    }
    restoredLinks[connName] = std::move(runtime);
    LOG_INFO("IEC104 已恢复链路配置: conn_name={}, conn_id={}, 点数={}, 状态={}", connName, connInfo.conn_id(), pointCount, persisted.pending_delete() ? "待删除" : "已停止");
  }

  for (const auto &[connName, _] : pointTablesByConn) {
    auto tableIt = pointTablesByConn.find(connName);
    const size_t pointCount = tableIt == pointTablesByConn.end() ? 0u : static_cast<size_t>(tableIt->second.points_size());
    LOG_WARNING("IEC104 点表持久化配置未找到对应链路，已忽略: conn_name={}", connName);
    LOG_WARNING("IEC104 点表持久化配置未进入本次恢复快照: 触发来源={}, conn_name={}, 点数={}", trigger, connName, pointCount);
    auto indexIt = pointTableIndexByConn.find(connName);
    if (indexIt != pointTableIndexByConn.end()) {
      const auto keepIndex = static_cast<size_t>(indexIt->second);
      if (keepPointTables[keepIndex]) {
        keepPointTables[keepIndex] = false;
        needResavePointTables = true;
      }
    }
  }

  const auto restoredCount = restoredLinks.size();
  {
    std::lock_guard<std::mutex> lock(mu_);
    linksByName_ = std::move(restoredLinks);
    reservedServerListenByName_ = std::move(restoredServerListenByName);
    pendingCreateByName_.clear();
  }

  // 仅回写已确认需要修正/清理的记录；DataCenter 瞬时失败的链路保留原持久化内容，留待下次恢复重试。
  if (needResaveLinks) {
    IEC104Proto::LinksConfig linksToPersist;
    for (int i = 0; i < linksConfig.links_size(); ++i) {
      if (!keepLinks[static_cast<size_t>(i)]) {
        continue;
      }
      auto *item = linksToPersist.add_links();
      *item = linksConfig.links(i);
      if (item->has_config()) {
        auto connIt = updatedConnIds.find(item->config().conn_name());
        if (connIt != updatedConnIds.end()) {
          item->set_conn_id(connIt->second);
        }
      }
    }
    LOG_WARNING("IEC104 本次恢复将回写链路持久化配置: 触发来源={}, 回写链路记录数={}", trigger, linksToPersist.links_size());
    auto saveStatus = linkStore_->Save(linksToPersist);
    if (!saveStatus.ok()) {
      LOG_ERROR("IEC104 回写链路持久化配置失败: {}", saveStatus.error_message());
    }
  }

  if (needResavePointTables) {
    IEC104Proto::PointTablesConfig pointTablesToPersist;
    for (int i = 0; i < pointTablesConfig.point_tables_size(); ++i) {
      if (!keepPointTables[static_cast<size_t>(i)]) {
        continue;
      }
      *pointTablesToPersist.add_point_tables() = pointTablesConfig.point_tables(i);
    }
    LOG_WARNING("IEC104 本次恢复将回写点表持久化配置: 触发来源={}, 回写点表记录数={}", trigger, pointTablesToPersist.point_tables_size());
    auto saveStatus = pointTableStore_->Save(pointTablesToPersist);
    if (!saveStatus.ok()) {
      LOG_ERROR("IEC104 回写点表持久化配置失败: {}", saveStatus.error_message());
    }
  }

  LOG_INFO("IEC104 本地持久化配置加载完成: 触发来源={}, 恢复成功={}, 恢复失败={}, DataCenter失败={}, 当前内存链路数={}", trigger, restoredCount, failedCount, dataCenterFailureCount, restoredCount);
  TryAutoStartReadyLinks(trigger);
}

grpc::Status LinkManager::saveLinksLocked() {
  if (!persistenceEnabled()) {
    return grpc::Status::OK;
  }
  auto config = dumpLinksConfigLocked();
  auto status = linkStore_->Save(config);
  if (!status.ok()) {
    LOG_ERROR("IEC104 本地链路配置落盘失败: {}", status.error_message());
  }
  return status;
}

grpc::Status LinkManager::savePointTablesLocked() {
  if (!persistenceEnabled()) {
    return grpc::Status::OK;
  }
  auto config = dumpPointTablesConfigLocked();
  auto status = pointTableStore_->Save(config);
  if (!status.ok()) {
    LOG_ERROR("IEC104 本地点表配置落盘失败: {}", status.error_message());
  }
  return status;
}

IEC104Proto::LinksConfig LinkManager::dumpLinksConfigLocked() const {
  IEC104Proto::LinksConfig config;
  for (const auto &[_, link] : linksByName_) {
    auto *item = config.add_links();
    *item->mutable_config() = link.config;
    item->set_conn_id(link.connId);
    item->set_pending_delete(link.state == IEC104Proto::LINK_STATE_PENDING_DELETE);
  }
  return config;
}

IEC104Proto::PointTablesConfig LinkManager::dumpPointTablesConfigLocked() const {
  IEC104Proto::PointTablesConfig config;
  for (const auto &[connName, link] : linksByName_) {
    if (!link.pointTableConfigured) {
      continue;
    }
    auto *table = config.add_point_tables();
    link.pointTable.ToProto(connName, table);
  }
  return config;
}

bool LinkManager::persistenceEnabled() const {
  return linkStore_ != nullptr && pointTableStore_ != nullptr;
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
      LOG_INFO("IEC104 未指定站点角色，已按传输角色默认: conn_name={}, station_role={}", config.conn_name(), stationRoleToString(stationRole));
    }
  }
  const auto connName = config.conn_name();
  const bool isServer = (config.role() == IEC104Proto::ROLE_SERVER);
  ListenEndpoint desiredListen;
  bool updated = false;
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
      status = saveLinksLocked();
      if (!status.ok()) {
        LOG_ERROR("IEC104 更新链路配置落盘失败: conn_name={}, 原因={}", connName, status.error_message());
        return status;
      }
      status = fillLinkInfoLocked(it->second, out);
      if (!status.ok()) {
        return status;
      }
      updated = true;
    } else {
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
  }

  if (updated) {
    LOG_INFO("IEC104 链路配置更新成功，当前不会自动启动链路连接功能，等待显式调用 StartLink: conn_name={}", connName);
    return grpc::Status::OK;
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

  bool createdNow = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    pendingCreateByName_.erase(connName);
    auto [it, inserted] = linksByName_.try_emplace(connName);
    createdNow = inserted;
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
      status = saveLinksLocked();
      if (!status.ok()) {
        LOG_ERROR("IEC104 更新链路配置落盘失败: conn_name={}, 原因={}", connName, status.error_message());
        return status;
      }
      status = fillLinkInfoLocked(it->second, out);
      if (!status.ok()) {
        return status;
      }
    } else {
      it->second.config = config;
      it->second.connId = connInfo.conn_id();
      it->second.state = IEC104Proto::LINK_STATE_STOPPED;
      it->second.lastError.clear();
      status = saveLinksLocked();
      if (!status.ok()) {
        LOG_ERROR("IEC104 创建链路配置落盘失败: conn_name={}, 原因={}", connName, status.error_message());
        return status;
      }
      status = fillLinkInfoLocked(it->second, out);
      if (!status.ok()) {
        return status;
      }
    }
  }
  LOG_INFO("IEC104 链路配置{}成功，当前不会自动启动链路连接功能，等待显式调用 StartLink: conn_name={}",
           createdNow ? "创建" : "更新",
           connName);
  return grpc::Status::OK;
}

grpc::Status LinkManager::RenameLink(const std::string &oldConnName,
                                     const std::string &newConnName,
                                     IEC104Proto::LinkInfo *out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  auto status = validateConnName(oldConnName);
  if (!status.ok()) {
    return status;
  }
  status = validateConnName(newConnName);
  if (!status.ok()) {
    return status;
  }

  uint32_t expectedConnId = 0;
  bool reservedRenameName = false;
  IEC104Proto::LinksConfig linksConfig;
  IEC104Proto::PointTablesConfig pointTablesConfig;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(oldConnName);
    if (it == linksByName_.end()) {
      return makeNotFound(oldConnName);
    }
    if (oldConnName == newConnName) {
      return fillLinkInfoLocked(it->second, out);
    }
    if (linksByName_.contains(newConnName) ||
        pendingCreateByName_.contains(newConnName) ||
        reservedServerListenByName_.contains(newConnName)) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name 已存在");
    }
    if (it->second.state == IEC104Proto::LINK_STATE_RUNNING) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "更新配置前请先停止链路");
    }
    if (it->second.state == IEC104Proto::LINK_STATE_PENDING_DELETE) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "链路处于待删除状态");
    }

    pendingCreateByName_.insert(newConnName);
    reservedRenameName = true;
    expectedConnId = it->second.connId;
    linksConfig = dumpLinksConfigLocked();
    pointTablesConfig = dumpPointTablesConfigLocked();
  }

  auto releaseRenameReservation = [this, &newConnName, &reservedRenameName]() {
    if (!reservedRenameName) {
      return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    pendingCreateByName_.erase(newConnName);
    reservedRenameName = false;
  };

  if (!renamePersistedLinkConfig(&linksConfig, oldConnName, newConnName)) {
    releaseRenameReservation();
    return grpc::Status(grpc::StatusCode::INTERNAL, "本地链路配置快照缺少待改名连接");
  }
  renamePersistedPointTableConfig(&pointTablesConfig, oldConnName, newConnName);

  DataCenterProto::ConnectionInfo connInfo;
  status = dataCenter_.RenameConnection(oldConnName, newConnName, &connInfo);
  if (!status.ok() && status.error_code() == grpc::StatusCode::NOT_FOUND) {
    bool oldExists = false;
    bool newExists = false;
    auto existsStatus = dataCenter_.ConnectionExists(oldConnName, &oldExists);
    if (!existsStatus.ok()) {
      releaseRenameReservation();
      return existsStatus;
    }
    existsStatus = dataCenter_.ConnectionExists(newConnName, &newExists);
    if (!existsStatus.ok()) {
      releaseRenameReservation();
      return existsStatus;
    }
    if (!oldExists && newExists) {
      auto getStatus = dataCenter_.GetOrCreateConnection(newConnName, &connInfo);
      if (!getStatus.ok()) {
        releaseRenameReservation();
        return getStatus;
      }
      if (connInfo.conn_id() != expectedConnId) {
        releaseRenameReservation();
        return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name 已存在");
      }
      LOG_WARNING("IEC104 检测到 DataCenter 连接已提前完成改名，继续收敛本地配置: old_conn_name={}, new_conn_name={}, conn_id={}",
                  oldConnName,
                  newConnName,
                  connInfo.conn_id());
      status = grpc::Status::OK;
    }
  }
  if (!status.ok()) {
    releaseRenameReservation();
    return status;
  }
  if (connInfo.conn_id() == 0) {
    releaseRenameReservation();
    return grpc::Status(grpc::StatusCode::INTERNAL, "DataCenter 返回 conn_id=0");
  }
  if (connInfo.conn_id() != expectedConnId) {
    releaseRenameReservation();
    return grpc::Status(grpc::StatusCode::INTERNAL, "DataCenter 返回的 conn_id 与本地链路不一致");
  }

  if (persistenceEnabled()) {
    status = linkStore_->Save(linksConfig);
    if (!status.ok()) {
      LOG_ERROR("IEC104 改名链路配置落盘失败: old_conn_name={}, new_conn_name={}, 原因={}",
                oldConnName,
                newConnName,
                status.error_message());
      releaseRenameReservation();
      return status;
    }
    status = pointTableStore_->Save(pointTablesConfig);
    if (!status.ok()) {
      LOG_ERROR("IEC104 改名点表配置落盘失败: old_conn_name={}, new_conn_name={}, 原因={}",
                oldConnName,
                newConnName,
                status.error_message());
      releaseRenameReservation();
      return status;
    }
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    pendingCreateByName_.erase(newConnName);
    reservedRenameName = false;

    auto oldIt = linksByName_.find(oldConnName);
    if (oldIt == linksByName_.end()) {
      auto newIt = linksByName_.find(newConnName);
      if (newIt == linksByName_.end()) {
        return makeNotFound(oldConnName);
      }
      return fillLinkInfoLocked(newIt->second, out);
    }
    if (linksByName_.contains(newConnName)) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name 已存在");
    }

    auto linkNode = linksByName_.extract(oldConnName);
    linkNode.key() = newConnName;
    linkNode.mapped().config.set_conn_name(newConnName);
    linkNode.mapped().connId = connInfo.conn_id();
    auto insertResult = linksByName_.insert(std::move(linkNode));
    if (!insertResult.inserted) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "conn_name 已存在");
    }

    auto listenNode = reservedServerListenByName_.extract(oldConnName);
    if (!listenNode.empty()) {
      listenNode.key() = newConnName;
      reservedServerListenByName_.insert(std::move(listenNode));
    }
    status = fillLinkInfoLocked(insertResult.position->second, out);
    if (!status.ok()) {
      return status;
    }
  }

  LOG_INFO("IEC104 链路改名成功: old_conn_name={}, new_conn_name={}, conn_id={}",
           oldConnName,
           newConnName,
           connInfo.conn_id());
  return grpc::Status::OK;
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
  link->transport->SetCommandCallback([this, connName](const CommandValue &cv) {
    (void)handleCommandValue(connName, cv);
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
  if (link.state == IEC104Proto::LINK_STATE_RUNNING) {
    LOG_INFO("IEC104 启动连接请求幂等成功: conn_name={}, 原因=链路已在运行", connName);
    return grpc::Status::OK;
  }

  status = checkStartPreconditionsLocked(link);
  if (!status.ok()) {
    link.lastError = status.error_message();
    return status;
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
    startCommandSubscribeLocked(connName, &link);
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
    stopCommandSubscribeLocked(&it->second);
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
      auto saveStatus = saveLinksLocked();
      if (!saveStatus.ok()) {
        LOG_ERROR("IEC104 待删除链路配置落盘失败: conn_name={}, 原因={}", connName, saveStatus.error_message());
      }
    }
    return dc;
  }

  std::lock_guard<std::mutex> lock(mu_);
  linksByName_.erase(connName);
  reservedServerListenByName_.erase(connName);
  status = saveLinksLocked();
  if (!status.ok()) {
    LOG_ERROR("IEC104 删除链路配置落盘失败: conn_name={}, 原因={}", connName, status.error_message());
    return status;
  }
  status = savePointTablesLocked();
  if (!status.ok()) {
    LOG_ERROR("IEC104 删除点表配置落盘失败: conn_name={}, 原因={}", connName, status.error_message());
    return status;
  }
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

  auto tags = buildDataCenterTags(next, config);
  status = dataCenter_.UpsertConnTags(connId, tags, true);
  if (!status.ok()) {
    return status;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = linksByName_.find(request.conn_name());
    if (it == linksByName_.end()) {
      return makeNotFound(request.conn_name());
    }
    it->second.pointTable = std::move(next);
    it->second.pointTableConfigured = true;
    it->second.lastReportedByTag.clear();
    status = savePointTablesLocked();
    if (!status.ok()) {
      LOG_ERROR("IEC104 点表配置落盘失败: conn_name={}, 原因={}", request.conn_name(), status.error_message());
      return status;
    }
  }
  LOG_INFO("IEC104 点表配置更新成功，当前不会自动启动链路连接功能，等待显式调用 StartLink: conn_name={}", request.conn_name());
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
