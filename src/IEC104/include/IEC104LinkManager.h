#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

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

  grpc::Status fillLinkInfoLocked(const LinkRuntime& link, IEC104Proto::LinkInfo* out) const;

  void configureTransportCallbacksLocked(const std::string& connName, LinkRuntime* link);
  void startDataCenterSubscribeLocked(const std::string& connName, LinkRuntime* link);
  void stopDataCenterSubscribeLocked(LinkRuntime* link);

  grpc::Status handleClientMeasuredValue(const std::string& connName, const MeasuredValue& mv);
  std::vector<MeasuredValue> buildInterrogationSnapshot(const std::string& connName);

  mutable std::mutex mu_;
  std::unordered_map<std::string, LinkRuntime> linksByName_;
  DataCenterClient dataCenter_;
};

}  // namespace IEC104
