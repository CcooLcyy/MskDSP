#pragma once

#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "IEC104.pb.h"
#include "IEC104DataCenterClient.h"
#include "IEC104PointTable.h"
#include "IEC104TcpLink.h"

namespace IEC104 {

class IEC104LinkStore;
class IEC104PointTableStore;

class LinkManager {
public:
  explicit LinkManager(std::string moduleName, std::filesystem::path linksPath = {}, std::filesystem::path pointTablesPath = {});
  ~LinkManager();

  void setDataCenterServerAddress(std::string address);
  void setDataCenterStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub);

  void LoadPersistedConfig();
  grpc::Status UpsertLink(const IEC104Proto::UpsertLinkRequest &request, IEC104Proto::LinkInfo *out);
  grpc::Status GetLink(const std::string &connName, IEC104Proto::LinkInfo *out) const;
  grpc::Status ListLinks(IEC104Proto::ListLinksResponse *out) const;
  grpc::Status StartLink(const std::string &connName);
  grpc::Status StopLink(const std::string &connName);
  grpc::Status DeleteLink(const std::string &connName);
  grpc::Status SendTimeSync(const std::string &connName, int64_t tsMs);
  void TryAutoStartReadyLinks(std::string_view trigger);

  grpc::Status UpsertPointTable(const IEC104Proto::UpsertPointTableRequest &request);
  grpc::Status GetPointTable(const std::string &connName, IEC104Proto::PointTable *out) const;

private:
  friend class IEC104LinkManagerTestPeer;
  friend class IEC104LinkStore;
  struct ListenEndpoint {
    // ROLE_SERVER 的规范化监听地址。
    // - any=true 表示绑定到 0.0.0.0:<port>（local.ip 为空或为 "0.0.0.0"）。
    // - any=false 表示绑定到 <ip>:<port>（ip 为规范字符串）。
    bool any = false;
    std::string ip;
    uint32_t port = 0;
  };

  struct LinkRuntime {
    IEC104Proto::LinkConfig config;
    uint32_t connId = 0;
    IEC104Proto::LinkState state = IEC104Proto::LINK_STATE_STOPPED;
    std::string lastError;
    PointTable pointTable;
    bool pointTableConfigured = false;
    std::unordered_map<std::string, double> lastReportedByTag;

    std::unique_ptr<TcpLink> transport;

    std::shared_ptr<grpc::ClientContext> dcSubscribeContext;
    std::jthread dcSubscribeThread;

    std::shared_ptr<grpc::ClientContext> dcTimeSyncContext;
    std::jthread dcTimeSyncThread;

    std::shared_ptr<grpc::ClientContext> dcCommandContext;
    std::jthread dcCommandThread;
  };

  static grpc::Status validateConnName(const std::string &connName);
  static grpc::Status validateLinkConfig(const IEC104Proto::LinkConfig &config);
  static IEC104Proto::StationRole normalizeStationRole(const IEC104Proto::LinkConfig &config);
  static bool isMasterStation(const IEC104Proto::LinkConfig &config);
  static bool isSlaveStation(const IEC104Proto::LinkConfig &config);
  static const char *stationRoleToString(IEC104Proto::StationRole role);

  static bool listenEndpointsConflict(const ListenEndpoint &a, const ListenEndpoint &b);
  static bool listenEndpointsEqual(const ListenEndpoint &a, const ListenEndpoint &b);
  static std::string listenEndpointToString(const ListenEndpoint &ep);
  static grpc::Status makeListenEndpoint(const IEC104Proto::Endpoint &local, ListenEndpoint *out);
  static grpc::Status checkSystemListenAvailable(const ListenEndpoint &ep);

  grpc::Status fillLinkInfoLocked(const LinkRuntime &link, IEC104Proto::LinkInfo *out) const;
  grpc::Status checkStartPreconditionsLocked(const LinkRuntime &link) const;
  grpc::Status tryAutoStartLink(const std::string &connName, std::string_view trigger);
  void loadPersistedConfig(std::string_view trigger);
  grpc::Status saveLinksLocked();
  grpc::Status savePointTablesLocked();
  IEC104Proto::LinksConfig dumpLinksConfigLocked() const;
  IEC104Proto::PointTablesConfig dumpPointTablesConfigLocked() const;
  bool persistenceEnabled() const;

  void configureTransportCallbacksLocked(const std::string &connName, LinkRuntime *link);
  void startDataCenterSubscribeLocked(const std::string &connName, LinkRuntime *link);
  void stopDataCenterSubscribeLocked(LinkRuntime *link);
  void startTimeSyncSubscribeLocked(const std::string &connName, LinkRuntime *link);
  void stopTimeSyncSubscribeLocked(LinkRuntime *link);
  void startCommandSubscribeLocked(const std::string &connName, LinkRuntime *link);
  void stopCommandSubscribeLocked(LinkRuntime *link);

  grpc::Status handleClientPointValue(const std::string &connName, const PointValue &pv);
  grpc::Status handleCommandValue(const std::string &connName, const CommandValue &cv);
  grpc::Status handleTimeSyncCommand(const std::string &connName, int64_t tsMs);
  std::vector<PointValue> buildInterrogationSnapshot(const std::string &connName);

  static std::string normalizeTimeSyncTag(const IEC104Proto::LinkConfig &config);

  mutable std::mutex mu_;
  std::unordered_map<std::string, LinkRuntime> linksByName_;
  // 记录 ROLE_SERVER 链路配置预留的端口（含进行中的 UpsertLink 创建）。
  std::unordered_map<std::string, ListenEndpoint> reservedServerListenByName_;
  // 在调用 DataCenter 期间阻止同一 conn_name 的并发创建。
  std::unordered_set<std::string> pendingCreateByName_;
  std::unique_ptr<IEC104LinkStore> linkStore_;
  std::unique_ptr<IEC104PointTableStore> pointTableStore_;
  DataCenterClient dataCenter_;
};

}  // namespace IEC104
