#pragma once

#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>

#include <cstdint>
#include <filesystem>
#include <functional>
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
#include "IEC104ReportPolicy.hpp"
#include "IEC104TcpLink.h"

namespace IEC104 {

class IEC104LinkStore;
class IEC104PointTableStore;
class IEC104SoeStore;

class LinkManager {
public:
  explicit LinkManager(std::string moduleName, std::filesystem::path configDbPath = {});
  ~LinkManager();

  void setDataCenterServerAddress(std::string address);
  void setDataCenterStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub);

  void LoadPersistedConfig();
  grpc::Status UpsertLink(const IEC104Proto::UpsertLinkRequest &request, IEC104Proto::LinkInfo *out);
  grpc::Status RenameLink(const std::string &oldConnName, const std::string &newConnName, IEC104Proto::LinkInfo *out);
  grpc::Status GetLink(const std::string &connName, IEC104Proto::LinkInfo *out) const;
  grpc::Status ListLinks(IEC104Proto::ListLinksResponse *out) const;
  grpc::Status StartLink(const std::string &connName);
  grpc::Status StopLink(const std::string &connName);
  grpc::Status DeleteLink(const std::string &connName);
  grpc::Status SendTimeSync(const std::string &connName, int64_t tsMs);
  void TryAutoStartReadyLinks(std::string_view trigger);

  grpc::Status UpsertPointTable(const IEC104Proto::UpsertPointTableRequest &request);
  grpc::Status GetPointTable(const std::string &connName, IEC104Proto::PointTable *out) const;
  grpc::Status QuerySoe(const IEC104Proto::QuerySoeRequest &request,
                        IEC104Proto::QuerySoeResponse *out) const;
  grpc::Status GenerateSimulationValues(const IEC104Proto::SimulationRequest &request,
                                        IEC104Proto::SimulationSnapshot *out);
  grpc::Status GetSimulationSnapshot(const std::string &connName, IEC104Proto::SimulationSnapshot *out) const;
  grpc::Status ApplySimulationValues(const std::string &connName);
  grpc::Status ClearSimulationValues(const std::string &connName);

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
    LinkRuntime() = default;
    // 析构前必须先摘除传输层回调：连接状态回调会反向获取 LinkManager::mu_，
    // 若析构发生在持锁期间（例如模块启动阶段重载配置时替换链路表），
    // 会因同一线程重入非递归互斥量而死锁。
    ~LinkRuntime();
    LinkRuntime(const LinkRuntime &) = delete;
    LinkRuntime &operator=(const LinkRuntime &) = delete;
    LinkRuntime(LinkRuntime &&) = default;
    LinkRuntime &operator=(LinkRuntime &&) = default;

    IEC104Proto::LinkConfig config;
    uint32_t connId = 0;
    IEC104Proto::LinkState state = IEC104Proto::LINK_STATE_STOPPED;
    IEC104Proto::ConnectionState connectionState = IEC104Proto::CONNECTION_STATE_DISCONNECTED;
    std::string lastError;
    PointTable pointTable;
    bool pointTableConfigured = false;
    std::unordered_map<std::string, mskdsp::numeric::Decimal20> lastReportedByTag;
    // 遥信 SOE 变位判定基线：按 tag 记录上一次已成功上送的 (状态, 品质)。
    // 链路启动、点表更新与配置重载时清空，清空后每个点会重新上送一次（首次规则）。
    std::unordered_map<std::string, detail::LastReportedSinglePoint> lastReportedSingleByTag;
    std::unordered_map<std::string, IEC104Proto::SimulationPoint> simulationValues;

    std::unique_ptr<TcpLink> transport;

    std::shared_ptr<grpc::ClientContext> dcSubscribeContext;
    std::jthread dcSubscribeThread;

    std::shared_ptr<grpc::ClientContext> dcTimeSyncContext;
    std::jthread dcTimeSyncThread;

    std::shared_ptr<grpc::ClientContext> dcCommandContext;
    std::jthread dcCommandThread;
  };

  // 订阅线程（DataCenter 订阅 / 对时订阅 / 命令订阅）停止所需的临时承载物。
  // 这些线程的运行期回调会反向获取 mu_，因此 request_stop() + join() 必须在锁外执行：
  // 若在持锁期间 join，一旦被 join 的线程正卡在等待 mu_ 上，就会永久死锁。
  // 使用方式：在获取 mu_ 之前声明本对象，锁内只把线程「摘出」，析构（此时锁已释放）完成停止与回收。
  struct SubscribeShutdown {
    SubscribeShutdown() = default;
    SubscribeShutdown(const SubscribeShutdown &) = delete;
    SubscribeShutdown &operator=(const SubscribeShutdown &) = delete;
    SubscribeShutdown(SubscribeShutdown &&) = delete;
    SubscribeShutdown &operator=(SubscribeShutdown &&) = delete;
    ~SubscribeShutdown();

    std::jthread thread;
    std::shared_ptr<grpc::ClientContext> context;
    // 仅用于停止日志，便于定位是哪条链路的哪类订阅被回收。
    std::string connName;
    std::string kind;
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
  void handleTransportConnectionState(const std::string &connName,
                                      IEC104Proto::ConnectionState state);
  void startDataCenterSubscribeLocked(const std::string &connName, LinkRuntime *link);
  // 仅把订阅线程与上下文摘出到 out，实际停止（request_stop + join）由 SubscribeShutdown 在锁外完成。
  void detachDataCenterSubscribeLocked(LinkRuntime *link, SubscribeShutdown *out);
  void startTimeSyncSubscribeLocked(const std::string &connName, LinkRuntime *link);
  void detachTimeSyncSubscribeLocked(LinkRuntime *link, SubscribeShutdown *out);
  void startCommandSubscribeLocked(const std::string &connName, LinkRuntime *link);
  void detachCommandSubscribeLocked(LinkRuntime *link, SubscribeShutdown *out);

  grpc::Status handleClientPointValue(const std::string &connName, const PointValue &pv);
  bool storeAndSendSoe(const std::string &connName, const PointValue &pv, TcpLink *transport);
  CommandResult handleCommandValue(const std::string &connName, const CommandValue &cv);
  static grpc::Status setSystemClock(int64_t tsMs);
  grpc::Status handleTimeSyncCommand(const std::string &connName, int64_t tsMs);
  std::vector<PointValue> buildInterrogationSnapshot(const std::string &connName);
  bool isSimulationValueActive(const std::string &connName, const std::string &tag) const;
  // 读取某条链路某 tag 的遥信变位基线；未记录时返回空，表示该点尚无历史基线（首次上送）。
  std::optional<detail::LastReportedSinglePoint> lastReportedSinglePoint(const std::string &connName,
                                                                        const std::string &tag) const;
  // 记录某条链路某 tag 本次已成功上送的遥信状态，作为下一次变位判定的基线。
  void rememberReportedSinglePoint(const std::string &connName, const std::string &tag, bool value,
                                   DataCenterProto::Quality quality);
  grpc::Status fillSimulationSnapshotLocked(const LinkRuntime &link, IEC104Proto::SimulationSnapshot *out) const;

  static std::string normalizeTimeSyncTag(const IEC104Proto::LinkConfig &config);

  mutable std::mutex mu_;
  // 必须晚于运行中链路析构，避免会话关闭回调访问已释放的 SOE 存储。
  std::unique_ptr<IEC104SoeStore> soeStore_;
  std::unordered_map<std::string, LinkRuntime> linksByName_;
  // 记录 ROLE_SERVER 链路配置的监听端点（含进行中的 UpsertLink 创建）；
  // 仅用于配置索引与恢复，不代表端点当前已成功监听。
  std::unordered_map<std::string, ListenEndpoint> reservedServerListenByName_;
  // 在调用 DataCenter 期间阻止同一 conn_name 的并发创建。
  std::unordered_set<std::string> pendingCreateByName_;
  std::unique_ptr<IEC104LinkStore> linkStore_;
  std::unique_ptr<IEC104PointTableStore> pointTableStore_;
  DataCenterClient dataCenter_;
  std::function<grpc::Status(int64_t)> systemTimeSetter_{setSystemClock};
};

}  // namespace IEC104
