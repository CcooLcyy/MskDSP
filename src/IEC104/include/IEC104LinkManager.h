#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>

#include "IEC104.pb.h"
#include "IEC104DataCenterClient.h"
#include "IEC104PointTable.h"
#include "IEC104TcpLink.h"

namespace IEC104 {

class LinkManager {
public:
  explicit LinkManager(std::string moduleName);

  void setDataCenterServerAddress(std::string address);
  void setDataCenterStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub);

  grpc::Status UpsertLink(const IEC104Proto::UpsertLinkRequest& request, IEC104Proto::LinkInfo* out);
  grpc::Status GetLink(const std::string& connName, IEC104Proto::LinkInfo* out) const;
  grpc::Status ListLinks(IEC104Proto::ListLinksResponse* out) const;
  grpc::Status StartLink(const std::string& connName);
  grpc::Status StopLink(const std::string& connName);
  grpc::Status DeleteLink(const std::string& connName);

  grpc::Status UpsertPointTable(const IEC104Proto::UpsertPointTableRequest& request);
  grpc::Status GetPointTable(const std::string& connName, IEC104Proto::PointTable* out) const;

private:
  struct ListenEndpoint {
    // Normalized listen address for ROLE_SERVER.
    // - any=true means bind to 0.0.0.0:<port> (local.ip empty or "0.0.0.0").
    // - any=false means bind to <ip>:<port> (ip is canonical string form).
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

    std::unique_ptr<TcpLink> transport;

    std::shared_ptr<grpc::ClientContext> dcSubscribeContext;
    std::jthread dcSubscribeThread;
  };

  static grpc::Status validateConnName(const std::string& connName);
  static grpc::Status validateLinkConfig(const IEC104Proto::LinkConfig& config);

  static bool listenEndpointsConflict(const ListenEndpoint& a, const ListenEndpoint& b);
  static bool listenEndpointsEqual(const ListenEndpoint& a, const ListenEndpoint& b);
  static std::string listenEndpointToString(const ListenEndpoint& ep);
  static grpc::Status makeListenEndpoint(const IEC104Proto::Endpoint& local, ListenEndpoint* out);
  static grpc::Status checkSystemListenAvailable(const ListenEndpoint& ep);

  grpc::Status fillLinkInfoLocked(const LinkRuntime& link, IEC104Proto::LinkInfo* out) const;

  void configureTransportCallbacksLocked(const std::string& connName, LinkRuntime* link);
  void startDataCenterSubscribeLocked(const std::string& connName, LinkRuntime* link);
  void stopDataCenterSubscribeLocked(LinkRuntime* link);

  grpc::Status handleClientMeasuredValue(const std::string& connName, const MeasuredValue& mv);
  std::vector<MeasuredValue> buildInterrogationSnapshot(const std::string& connName);

  mutable std::mutex mu_;
  std::unordered_map<std::string, LinkRuntime> linksByName_;
  // Tracks ports reserved by ROLE_SERVER link configs (including in-flight UpsertLink creates).
  std::unordered_map<std::string, ListenEndpoint> reservedServerListenByName_;
  // Blocks concurrent creation of the same conn_name while we call DataCenter.
  std::unordered_set<std::string> pendingCreateByName_;
  DataCenterClient dataCenter_;
};

}  // namespace IEC104
