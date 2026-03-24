#pragma once

#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>

#include <atomic>
#include <boost/json.hpp>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "DLT645.pb.h"
#include "DLT645DataCenterClient.h"
#include "DLT645LinkStore.h"
#include "DLT645MqttClient.h"
#include "DLT645MqttStore.h"
#include "DLT645PointTable.h"
#include "DLT645PointTableStore.h"

namespace DLT645 {

class LinkManager {
public:
  explicit LinkManager(std::string moduleName);

  grpc::Status UpdateConfig(const DLT645Proto::UpdateConfigRequest &request, DLT645Proto::UpdateConfigResponse *response);
  grpc::Status UpsertLink(const DLT645Proto::UpsertLinkRequest &request, DLT645Proto::LinkInfo *out);
  grpc::Status GetLink(const std::string &connName, DLT645Proto::LinkInfo *out) const;
  grpc::Status ListLinks(DLT645Proto::ListLinksResponse *out) const;
  grpc::Status StartLink(const std::string &connName);
  grpc::Status StopLink(const std::string &connName);
  grpc::Status DeleteLink(const std::string &connName);
  grpc::Status UpsertPointTable(const DLT645Proto::UpsertPointTableRequest &request);
  grpc::Status GetPointTable(const std::string &connName, DLT645Proto::PointTable *out) const;
  void LoadPersistedConfig();
  void TryAutoStartReadyLinks(std::string_view trigger);

  void setDataCenterServerAddress(std::string address);
  void setDataCenterStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub);
  void setMqttStub(std::shared_ptr<MQTTManagerProto::MQTTManagerService::StubInterface> stub);

private:
  friend class DLT645LinkManagerTestPeer;
  struct PendingResponse {
    std::mutex mutex;
    std::condition_variable cv;
    bool done{false};
    bool ok{false};
    int32_t status{0};
    std::string message;
    std::string payloadBase64;
  };

  struct LinkRuntime {
    DLT645Proto::LinkConfig config;
    uint32_t connId = 0;
    DLT645Proto::LinkState state = DLT645Proto::LINK_STATE_STOPPED;
    std::string lastError;
    PointTable pointTable;
    bool pointTableConfigured = false;
    bool archiveRetrying = false;

    std::shared_ptr<grpc::ClientContext> mqttSubscribeContext;
    std::jthread mqttSubscribeThread;

    std::shared_ptr<grpc::ClientContext> dcSubscribeContext;
    std::jthread dcSubscribeThread;

    std::jthread pollThread;
    std::jthread archiveRetryThread;
    std::mutex requestMutex;
    std::mutex pendingMutex;
    std::unordered_map<std::string, std::shared_ptr<PendingResponse>> pending;
    std::unordered_map<std::string, double> lastReportedByTag;
  };

  struct Frame {
    std::vector<uint8_t> address;
    uint8_t control = 0;
    std::vector<uint8_t> data;
  };

  grpc::Status validateConnName(const std::string &connName) const;
  grpc::Status normalizeLinkConfig(const DLT645Proto::LinkConfig &config, DLT645Proto::LinkConfig *out) const;
  grpc::Status fillLinkInfoLocked(const LinkRuntime &link, DLT645Proto::LinkInfo *out) const;
  DLT645Proto::LinksConfig dumpLinksConfigLocked() const;
  DLT645Proto::PointTablesConfig dumpPointTablesConfigLocked() const;
  grpc::Status saveLinksConfig(const DLT645Proto::LinksConfig &config);
  grpc::Status savePointTablesConfig(const DLT645Proto::PointTablesConfig &config);
  bool isLinkAutoStartReadyLocked(const LinkRuntime &link, std::string *reason) const;
  grpc::Status maybeAutoStartLink(const std::string &connName, std::string_view trigger);
  void autoStartEligibleLinks(std::string_view trigger);
  void launchArchiveRetryLocked(const std::string &connName,
                                const std::shared_ptr<LinkRuntime> &link,
                                const std::string &archiveKey);
  void runArchiveRetryLoop(std::string connName,
                           std::shared_ptr<LinkRuntime> link,
                           std::string archiveKey,
                           std::stop_token st);
  void stopArchiveRetryLocked(LinkRuntime *link, std::jthread *outThread);
  void releaseArchiveRefOnStartAbort(const std::string &connName,
                                     const std::shared_ptr<LinkRuntime> &link,
                                     const std::string &archiveKey);

  void startPollingLocked(const std::string &connName, const std::shared_ptr<LinkRuntime> &link);
  void stopPollingLocked(LinkRuntime *link);
  void startMqttSubscribeLocked(const std::string &connName, const std::shared_ptr<LinkRuntime> &link);
  void stopMqttSubscribeLocked(LinkRuntime *link);
  void startDataCenterSubscribeLocked(const std::string &connName, const std::shared_ptr<LinkRuntime> &link);
  void stopDataCenterSubscribeLocked(LinkRuntime *link);

  grpc::Status sendAddSlaveNode(LinkRuntime *link, bool *outArchiveExists);
  grpc::Status sendAddSlaveNodes(const std::vector<LinkRuntime *> &links, bool *outArchiveExists);
  grpc::Status sendDelSlaveNode(LinkRuntime *link);
  grpc::Status sendMonitorRequest(LinkRuntime *link, const std::vector<uint8_t> &frame, std::string *outPayloadBase64, int32_t *outStatus);
  grpc::Status sendWriteRequest(LinkRuntime *link, const std::vector<uint8_t> &frame);
  grpc::Status sendMonitorRequest(LinkRuntime *link, const std::string &requestTopic, const std::string &responseTopic, const boost::json::object &obj, uint32_t timeoutMs, int32_t *outStatus, std::string *outPayloadBase64);
  grpc::Status runLoraSerialized(LinkRuntime *link, const char *operation, const std::string &topic, const std::function<grpc::Status()> &action);

  grpc::Status decodeAndPublish(LinkRuntime *link, const PointTable::Point &point, const std::vector<uint8_t> &payload, int64_t tsMs, bool trimRightSpace);

  static std::string makeMonitorRequestTopic(const DLT645Proto::LinkConfig &config);
  static std::string makeMonitorResponseTopic(const DLT645Proto::LinkConfig &config);
  static std::string makeAddSlaveRequestTopic(DLT645Proto::CommMode mode);
  static std::string makeAddSlaveResponseTopic(DLT645Proto::CommMode mode);
  static std::string makeDelSlaveRequestTopic(DLT645Proto::CommMode mode);
  static std::string makeDelSlaveResponseTopic(DLT645Proto::CommMode mode);

  static std::string formatHex(const std::vector<uint8_t> &data);
  static std::string formatTimestamp();
  static uint64_t nowMs();
  static bool parseHexByte(const std::string &text, uint8_t *out);
  static bool decodeHexString(const std::string &text, std::vector<uint8_t> *out);
  static std::vector<uint8_t> encodeBcd(const std::string &digits);
  static std::vector<uint8_t> encodeAddress(const std::string &addr);
  static std::vector<uint8_t> encodeDiBytes(const std::array<uint8_t, 4> &diBytes, const DLT645Proto::LinkConfig &config);
  static std::vector<uint8_t> encodeDi(const PointTable::Point &point, const DLT645Proto::LinkConfig &config);
  static std::vector<uint8_t> encodeData(const PointTable::Point &point, const DataCenterProto::PointValue &value, std::string *error);
  static bool decodeFrame(const std::vector<uint8_t> &data, Frame *out, std::string *error);
  static std::vector<uint8_t> buildFrame(const std::vector<uint8_t> &addr, uint8_t control, const std::vector<uint8_t> &data);
  static uint8_t checksum(const std::vector<uint8_t> &data);
  static void addOffset33(std::vector<uint8_t> *data);
  static void subOffset33(std::vector<uint8_t> *data);

  grpc::Status handleMonitorResponse(LinkRuntime *link, const std::string &payloadBase64, const PointTable::Point &point, std::string *error);
  grpc::Status handleMonitorResponse(LinkRuntime *link, const std::string &payloadBase64, const PointTable::Block &block, std::string *error);

  grpc::Status parseResponsePayload(const std::string &payloadBase64, Frame *outFrame, std::string *error);

  static bool pointValueToDouble(const DataCenterProto::PointValue &value, double *out);
  static bool pointValueToBool(const DataCenterProto::PointValue &value, bool *out);
  static bool pointValueToString(const DataCenterProto::PointValue &value, std::string *out);
  static bool reverseScale(double value, double scale, double offset, double *out);

  std::string nextToken();

  mutable std::mutex mu_;
  std::unordered_map<std::string, std::shared_ptr<LinkRuntime>> linksByName_;
  std::unordered_map<std::string, uint32_t> archiveRefCountByKey_;
  std::unordered_set<std::string> archiveAddInFlightByKey_;
  std::unordered_set<std::string> archiveDelInFlightByKey_;
  std::condition_variable archiveStateCv_;
  std::unordered_set<std::string> pendingCreateByName_;
  DataCenterClient dataCenter_;
  MqttClient mqttClient_;
  DLT645MqttStore mqttStore_;
  DLT645LinkStore linkStore_;
  DLT645PointTableStore pointTableStore_;
  std::string moduleName_;
  std::atomic<uint64_t> tokenCounter_{0};
  std::mutex loraRequestMutex_;
};

}  // namespace DLT645
