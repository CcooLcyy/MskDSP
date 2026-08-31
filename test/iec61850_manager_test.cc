#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "IEC61850Manager.h"
#include "IEC61850ProtocolStack.h"
#include "mskdsp/IEC61850Limits.hpp"
#include "support/FakeDataCenter.hpp"

namespace {

constexpr auto kMinimalScl = R"xml(
<SCL xmlns="http://www.iec.ch/61850/2003/SCL">
  <Communication>
    <SubNetwork name="station" type="8-MMS">
      <ConnectedAP iedName="IED1" apName="AP1">
        <Address><P type="IP">192.0.2.10</P></Address>
        <GSE ldInst="LD0" cbName="gcb1"><Address>
          <P type="MAC-Address">01-0C-CD-01-00-01</P>
          <P type="APPID">1001</P>
          <P type="VLAN-ID">001</P>
          <P type="VLAN-PRIORITY">4</P>
        </Address></GSE>
        <SMV ldInst="LD0" cbName="smv1"><Address>
          <P type="MAC-Address">01-0C-CD-04-00-01</P>
          <P type="APPID">4001</P>
          <P type="VLAN-ID">004</P>
          <P type="VLAN-PRIORITY">4</P>
        </Address></SMV>
      </ConnectedAP>
      <ConnectedAP iedName="IED1" apName="AP2">
        <Address><P type="IP">192.0.2.20</P></Address>
        <GSE ldInst="LD1" cbName="gcb2"><Address>
          <P type="MAC-Address">01-0C-CD-01-00-03</P>
          <P type="APPID">2001</P>
          <P type="VLAN-ID">003</P>
          <P type="VLAN-PRIORITY">4</P>
        </Address></GSE>
      </ConnectedAP>
    </SubNetwork>
    <SubNetwork name="station-b" type="8-MMS">
      <ConnectedAP iedName="IED1" apName="AP1">
        <Address><P type="IP">192.0.2.11</P></Address>
        <GSE ldInst="LD0" cbName="gcb1"><Address>
          <P type="MAC-Address">01-0C-CD-01-00-02</P>
          <P type="APPID">1002</P>
          <P type="VLAN-ID">002</P>
          <P type="VLAN-PRIORITY">4</P>
        </Address></GSE>
      </ConnectedAP>
    </SubNetwork>
  </Communication>
  <IED name="IED1">
    <AccessPoint name="AP1"><Server><LDevice inst="LD0">
      <LN0 lnClass="LLN0" inst="" lnType="ln0">
        <DataSet name="events">
          <FCDA ldInst="LD0" lnClass="PTRC" lnInst="1" doName="Tr" daName="general" fc="ST"/>
        </DataSet>
        <DataSet name="samples">
          <FCDA ldInst="LD0" lnClass="PTRC" lnInst="1" doName="Tr" daName="measurement" fc="MX"/>
        </DataSet>
        <ReportControl name="brcb1" rptID="IED1/Events" datSet="events" buffered="true" confRev="3" intgPd="5000" bufTime="20">
          <TrgOps dchg="true" qchg="true" dupd="false" period="true" gi="true"/>
          <OptFields seqNum="true" timeStamp="true" reasonCode="true" dataSet="true" dataRef="true" bufOvfl="true" entryID="true" configRef="true" segmentation="true"/>
          <RptEnabled max="2"/>
        </ReportControl>
        <GSEControl name="gcb1" appID="Trip" datSet="events" confRev="4"/>
        <SampledValueControl name="smv1" smvID="MU01" datSet="samples" confRev="5" smpRate="4"/>
      </LN0>
      <LN lnClass="PTRC" inst="1" lnType="ptrc">
        <Inputs>
          <ExtRef intAddr="trip-in" iedName="IED1" ldInst="LD0" lnClass="PTRC" lnInst="1" doName="Tr" daName="general" fc="ST" serviceType="GOOSE" srcLDInst="LD0" srcLNClass="LLN0" srcCBName="gcb1"/>
          <ExtRef intAddr="sample-in" iedName="IED1" ldInst="LD0" lnClass="PTRC" lnInst="1" doName="Tr" daName="measurement" fc="MX" serviceType="SMV" srcLDInst="LD0" srcLNClass="LLN0" srcCBName="smv1"/>
        </Inputs>
      </LN>
    </LDevice></Server></AccessPoint>
    <AccessPoint name="AP2"><Server><LDevice inst="LD1">
      <LN0 lnClass="LLN0" inst="" lnType="ln0">
        <DataSet name="other-events">
          <FCDA ldInst="LD1" lnClass="PTRC" lnInst="1" doName="Tr" daName="general" fc="ST"/>
        </DataSet>
        <ReportControl name="brcb2" rptID="IED1/OtherEvents" datSet="other-events" buffered="true" confRev="9">
          <TrgOps dchg="true" gi="false"/>
          <OptFields seqNum="true" segmentation="false"/>
        </ReportControl>
        <GSEControl name="gcb2" appID="OtherTrip" datSet="other-events" confRev="10"/>
        <SampledValueControl name="smv2" smvID="MU02" datSet="other-events" confRev="11" smpRate="256"/>
      </LN0>
      <LN lnClass="PTRC" inst="1" lnType="ptrc">
        <Inputs><ExtRef intAddr="other-trip-in" iedName="IED1" ldInst="LD1" lnClass="PTRC" lnInst="1" doName="Tr" daName="general" fc="ST" serviceType="GOOSE" srcLDInst="LD1" srcLNClass="LLN0" srcCBName="gcb2"/></Inputs>
      </LN>
    </LDevice></Server></AccessPoint>
    <AccessPoint name="AP3"><Server/></AccessPoint>
  </IED>
  <DataTypeTemplates>
    <LNodeType id="ln0" lnClass="LLN0"/>
    <LNodeType id="ptrc" lnClass="PTRC"><DO name="Tr" type="act"/><DO name="Int" type="intctrl"/><DO name="Analog" type="floatctrl"/></LNodeType>
    <DOType id="act" cdc="ACT"><DA name="general" bType="BOOLEAN" fc="ST"/><DA name="measurement" bType="FLOAT64" fc="MX"/><DA name="ctlVal" bType="BOOLEAN" fc="CO"/></DOType>
    <DOType id="intctrl" cdc="INC"><DA name="ctlVal" bType="INT32" fc="CO"/></DOType>
    <DOType id="floatctrl" cdc="APC"><DA name="ctlVal" bType="FLOAT64" fc="CO"/></DOType>
  </DataTypeTemplates>
</SCL>)xml";

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("mskdsp-iec61850-manager-test-" + std::to_string(suffix));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  std::filesystem::path database() const { return path_ / "config.db"; }

private:
  std::filesystem::path path_;
};

class FakeProtocolStack final : public IEC61850::ProtocolStackAdapter {
public:
  grpc::Status StartIed(IEC61850::ProtocolIedPlan plan,
                        IEC61850::ProtocolEventCallbacks callbacksIn) override {
    ++startCount;
    lastStarted = plan.config.conn_name();
    lastPlan = plan;
    callbacks = std::move(callbacksIn);
    if (throwOnStart) {
      throw std::runtime_error("协议栈启动异常");
    }
    if (startStatus.ok() && emitConnectedOnStart &&
        plan.config.enable_mms() &&
        callbacks.onMmsConnection) {
      IEC61850::MmsConnectionEvent event;
      event.type = IEC61850::MmsConnectionEventType::STATE_SNAPSHOT;
      event.state = IEC61850::ProtocolSessionState::READY;
      bool activeSelected = false;
      for (const auto& channel : plan.config.channels()) {
        if (!channel.enabled()) {
          continue;
        }
        auto& status = event.channels.emplace_back();
        status.channel = channel.channel();
        status.state = activeSelected
                           ? IEC61850Proto::CHANNEL_STATE_DISCONNECTED
                           : IEC61850Proto::CHANNEL_STATE_CONNECTED;
        if (!activeSelected) {
          event.activeChannel = channel.channel();
          activeSelected = true;
        }
      }
      callbacks.onMmsConnection(std::move(event));
    }
    return startStatus;
  }

  grpc::Status StopIed(std::string_view connName) override {
    ++stopCount;
    lastStopped = connName;
    callbacks = {};
    if (throwOnStop) {
      throw std::runtime_error("协议栈停止异常");
    }
    return stopStatus;
  }

  grpc::Status ReadMms(std::string_view connName,
                       const IEC61850::MmsReadRequest& request,
                       IEC61850::MmsReadResponse* response) override {
    ++readCount;
    lastReadConnName = connName;
    lastReadRequest = request;
    if (throwOnRead) {
      throw std::runtime_error("MMS Read协议栈异常");
    }
    if (readStatus.ok() && response != nullptr) {
      *response = readResponse;
    }
    return readStatus;
  }

  grpc::Status WriteMms(std::string_view connName,
                        const IEC61850::MmsWriteRequest& request,
                        IEC61850::MmsWriteResponse* response) override {
    ++writeCount;
    lastWriteConnName = connName;
    lastWriteRequest = request;
    if (throwOnWrite) {
      throw std::runtime_error("MMS Write协议栈异常");
    }
    if (writeStatus.ok() && response != nullptr) {
      *response = writeResponse;
    }
    return writeStatus;
  }

  grpc::Status SelectMmsControl(
      std::string_view connName, const IEC61850::MmsObjectName& controlObject,
      IEC61850::MmsReadResponse* response) override {
    ++selectControlCount;
    lastSelectControlConnName = connName;
    lastSelectControlObject = controlObject;
    if (throwOnSelectControl) {
      throw std::runtime_error("MMS SBO选择协议栈异常");
    }
    if (selectControlStatus.ok() && response != nullptr) {
      *response = selectControlResponse;
    }
    return selectControlStatus;
  }

  grpc::Status WriteMmsControl(
      std::string_view connName,
      const IEC61850::MmsControlCommand& command,
      IEC61850::MmsWriteResponse* response) override {
    ++writeControlCount;
    lastWriteControlConnName = connName;
    lastWriteControlCommand = command;
    if (throwOnWriteControl) {
      throw std::runtime_error("MMS控制Write协议栈异常");
    }
    if (writeControlStatus.ok() && response != nullptr) {
      *response = writeControlResponse;
    }
    return writeControlStatus;
  }

  grpc::Status ExecuteMmsPointControl(
      std::string_view connName,
      const IEC61850::MmsPointControlCommand& command,
      IEC61850::MmsWriteResponse* response) override {
    std::unique_lock pointControlLock(pointControlMutex);
    ++pointControlCount;
    lastPointControlConnName = connName;
    lastPointControlCommand = command;
    if (blockPointControl) {
      pointControlEntered = true;
      pointControlCondition.notify_all();
      pointControlCondition.wait(pointControlLock,
                                 [this] { return releasePointControl; });
    }
    if (throwOnPointControl) {
      throw std::runtime_error("MMS同步控制协议栈异常");
    }
    if (pointControlStatus.ok() && response != nullptr) {
      *response = pointControlResponse;
    }
    return pointControlStatus;
  }

  void BlockPointControl() {
    std::lock_guard lock(pointControlMutex);
    blockPointControl = true;
    pointControlEntered = false;
    releasePointControl = false;
  }

  bool WaitUntilPointControlEntered() {
    std::unique_lock lock(pointControlMutex);
    return pointControlCondition.wait_for(
        lock, std::chrono::seconds(2), [this] { return pointControlEntered; });
  }

  void ReleasePointControl() {
    std::lock_guard lock(pointControlMutex);
    releasePointControl = true;
    pointControlCondition.notify_all();
  }

  grpc::Status PublishGoose(
      std::string_view connName,
      const IEC61850::ProtocolGoosePublishCommand& command) override {
    std::lock_guard lock(gooseMutex);
    ++goosePublishCount;
    goosePublishThread = std::this_thread::get_id();
    lastGoosePublishConnName = connName;
    lastGoosePublishPublisherId = command.publisherId;
    lastGoosePublishSubscriptionId = command.subscriptionId;
    lastGoosePublishValues.assign(command.values.begin(), command.values.end());
    return goosePublishStatus;
  }

  void EmitMmsReport(IEC61850::MmsReportEvent report) const {
    if (callbacks.onMmsReport) {
      callbacks.onMmsReport(std::move(report));
    }
  }

  void EmitMmsConnectionEvent(IEC61850::MmsConnectionEvent event) const {
    if (callbacks.onMmsConnection) {
      callbacks.onMmsConnection(std::move(event));
    }
  }

  void EmitGooseFrame(IEC61850::ProtocolGooseFrameView frame) const {
    if (callbacks.onGooseFrame) {
      callbacks.onGooseFrame(frame);
    }
  }

  void EmitSvFrame(IEC61850::ProtocolSvFrameView frame) const {
    if (callbacks.onSvFrame) {
      callbacks.onSvFrame(frame);
    }
  }

  IEC61850::ProtocolEventCallbacks CopyCallbacks() const { return callbacks; }

  int GoosePublishCount() const {
    std::lock_guard lock(gooseMutex);
    return goosePublishCount;
  }

  std::thread::id GoosePublishThread() const {
    std::lock_guard lock(gooseMutex);
    return goosePublishThread;
  }

  static void EmitMmsReportWith(
      const IEC61850::ProtocolEventCallbacks& target,
      IEC61850::MmsReportEvent report) {
    if (target.onMmsReport) {
      target.onMmsReport(std::move(report));
    }
  }

  static void EmitMmsConnectionEventWith(
      const IEC61850::ProtocolEventCallbacks& target,
      IEC61850::MmsConnectionEvent event) {
    if (target.onMmsConnection) {
      target.onMmsConnection(std::move(event));
    }
  }

  static void EmitGooseFrameWith(
      const IEC61850::ProtocolEventCallbacks& target,
      IEC61850::ProtocolGooseFrameView frame) {
    if (target.onGooseFrame) {
      target.onGooseFrame(frame);
    }
  }

  int startCount = 0;
  int stopCount = 0;
  std::string lastStarted;
  std::string lastStopped;
  grpc::Status startStatus = grpc::Status::OK;
  grpc::Status stopStatus = grpc::Status::OK;
  bool emitConnectedOnStart = true;
  bool throwOnStart = false;
  bool throwOnStop = false;
  bool throwOnRead = false;
  bool throwOnWrite = false;
  bool throwOnSelectControl = false;
  bool throwOnWriteControl = false;
  bool throwOnPointControl = false;
  int readCount = 0;
  int writeCount = 0;
  int selectControlCount = 0;
  int writeControlCount = 0;
  int pointControlCount = 0;
  std::string lastReadConnName;
  std::string lastWriteConnName;
  std::string lastSelectControlConnName;
  std::string lastWriteControlConnName;
  IEC61850::MmsReadRequest lastReadRequest;
  IEC61850::MmsWriteRequest lastWriteRequest;
  IEC61850::MmsObjectName lastSelectControlObject;
  IEC61850::MmsControlCommand lastWriteControlCommand;
  IEC61850::MmsPointControlCommand lastPointControlCommand;
  std::string lastPointControlConnName;
  grpc::Status readStatus = grpc::Status::OK;
  grpc::Status writeStatus = grpc::Status::OK;
  grpc::Status selectControlStatus = grpc::Status::OK;
  grpc::Status writeControlStatus = grpc::Status::OK;
  IEC61850::MmsReadResponse readResponse;
  IEC61850::MmsWriteResponse writeResponse;
  IEC61850::MmsReadResponse selectControlResponse;
  IEC61850::MmsWriteResponse writeControlResponse;
  grpc::Status pointControlStatus = grpc::Status::OK;
  IEC61850::MmsWriteResponse pointControlResponse;
  mutable std::mutex pointControlMutex;
  std::condition_variable pointControlCondition;
  bool blockPointControl = false;
  bool pointControlEntered = false;
  bool releasePointControl = true;
  mutable std::mutex gooseMutex;
  int goosePublishCount = 0;
  std::thread::id goosePublishThread;
  std::string lastGoosePublishConnName;
  std::uint32_t lastGoosePublishPublisherId = 0;
  std::uint32_t lastGoosePublishSubscriptionId = 0;
  std::vector<IEC61850::ProtocolRealtimeValue> lastGoosePublishValues;
  grpc::Status goosePublishStatus = grpc::Status::OK;
  IEC61850::ProtocolIedPlan lastPlan;
  IEC61850::ProtocolEventCallbacks callbacks;
};

class BlockingProtocolStack final : public IEC61850::ProtocolStackAdapter {
public:
  grpc::Status StartIed(IEC61850::ProtocolIedPlan plan,
                        IEC61850::ProtocolEventCallbacks) override {
    std::unique_lock lock(mutex_);
    ++startCount_;
    lastStarted_ = plan.config.conn_name();
    startEntered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [this]() { return releaseStart_; });
    return grpc::Status::OK;
  }

  grpc::Status StopIed(std::string_view connName) override {
    std::unique_lock lock(mutex_);
    ++stopCount_;
    lastStopped_ = connName;
    stopEntered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [this]() { return releaseStop_; });
    return grpc::Status::OK;
  }

  bool WaitUntilStartEntered() {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(2),
                               [this]() { return startEntered_; });
  }

  void ReleaseStart() {
    std::lock_guard lock(mutex_);
    releaseStart_ = true;
    condition_.notify_all();
  }

  void BlockStop() {
    std::lock_guard lock(mutex_);
    stopEntered_ = false;
    releaseStop_ = false;
  }

  bool WaitUntilStopEntered() {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(2),
                               [this]() { return stopEntered_; });
  }

  void ReleaseStop() {
    std::lock_guard lock(mutex_);
    releaseStop_ = true;
    condition_.notify_all();
  }

  int startCount() const {
    std::lock_guard lock(mutex_);
    return startCount_;
  }

  int stopCount() const {
    std::lock_guard lock(mutex_);
    return stopCount_;
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool startEntered_ = false;
  bool releaseStart_ = false;
  bool stopEntered_ = false;
  bool releaseStop_ = true;
  int startCount_ = 0;
  int stopCount_ = 0;
  std::string lastStarted_;
  std::string lastStopped_;
};

IEC61850Proto::ImportSclRequest MakeImportRequest(bool validateOnly = false) {
  IEC61850Proto::ImportSclRequest request;
  request.set_model_name("station-model");
  request.set_source_name("station.scd");
  request.set_content(kMinimalScl);
  request.set_validate_only(validateOnly);
  return request;
}

IEC61850Proto::UpsertIedRequest MakeIedRequest() {
  IEC61850Proto::UpsertIedRequest request;
  auto* config = request.mutable_config();
  config->set_conn_name("line-1");
  config->set_model_name("station-model");
  config->set_ied_name("IED1");
  config->set_access_point("AP1");
  config->set_enable_goose(true);
  auto* channel = config->add_channels();
  channel->set_channel(IEC61850Proto::NETWORK_CHANNEL_A);
  channel->set_enabled(true);
  channel->set_interface_name("eth0");
  channel->set_subnetwork_name("station");
  return request;
}

IEC61850Proto::UpsertIedRequest MakeMmsIedRequest() {
  auto request = MakeIedRequest();
  auto* config = request.mutable_config();
  config->set_enable_goose(false);
  config->set_enable_mms(true);
  config->set_mms_event_queue_capacity(8);
  config->set_publish_batch_size(4);
  config->set_publish_batch_window_ms(1);
  auto* channel = config->mutable_channels(0);
  channel->set_remote_ip("192.0.2.10");
  channel->set_remote_port(102);
  return request;
}

IEC61850Proto::ApplyTargetConfigRequest MakeTargetRequest(
    bool desiredRunning) {
  IEC61850Proto::ApplyTargetConfigRequest request;
  auto* model = request.add_models();
  model->set_model_name("station-model");
  model->set_source_name("station.scd");
  model->set_content(kMinimalScl);
  auto* target = request.add_ieds();
  *target->mutable_config() = MakeIedRequest().config();
  target->set_desired_running(desiredRunning);
  return request;
}

IEC61850Proto::UpsertPointMappingsRequest MakeTripMappingRequest() {
  IEC61850Proto::UpsertPointMappingsRequest request;
  request.set_conn_name("line-1");
  request.set_replace(true);
  auto* point = request.add_points();
  point->set_tag("TRIP");
  point->set_data_ref("IED1LD0/PTRC1.Tr.general");
  point->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST);
  point->set_source(IEC61850Proto::POINT_SOURCE_GOOSE);
  point->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_BOOL);
  return request;
}

IEC61850Proto::UpsertPointMappingsRequest MakeMmsControlMappingRequest() {
  IEC61850Proto::UpsertPointMappingsRequest request;
  request.set_conn_name("line-1");
  request.set_replace(true);

  auto* boolPoint = request.add_points();
  boolPoint->set_tag("BOOL_CONTROL");
  boolPoint->set_data_ref("IED1LD0/PTRC1.Tr.ctlVal");
  boolPoint->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_CO);
  boolPoint->set_source(IEC61850Proto::POINT_SOURCE_MMS);
  boolPoint->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_BOOL);

  auto* integerPoint = request.add_points();
  integerPoint->set_tag("INTEGER_CONTROL");
  integerPoint->set_data_ref("IED1LD0/PTRC1.Int.ctlVal");
  integerPoint->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_CO);
  integerPoint->set_source(IEC61850Proto::POINT_SOURCE_MMS);
  integerPoint->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_INT64);
  integerPoint->set_scale(2.0);
  integerPoint->set_offset(3.0);

  auto* floatingPoint = request.add_points();
  floatingPoint->set_tag("FLOAT_CONTROL");
  floatingPoint->set_data_ref("IED1LD0/PTRC1.Analog.ctlVal");
  floatingPoint->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_CO);
  floatingPoint->set_source(IEC61850Proto::POINT_SOURCE_MMS);
  floatingPoint->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_DOUBLE);
  floatingPoint->set_scale(4.0);
  floatingPoint->set_offset(-1.0);

  return request;
}

IEC61850::MmsReportEvent MakeTripReport(bool value, int64_t timestampMs) {
  IEC61850::MmsReportEvent report;
  report.reportRef = "IED1LD0/LLN0$BR$events";
  report.dataSetRef = "IED1LD0/LLN0$events";
  report.confRev = 1;
  report.sequenceNumber = 1;
  report.receiveTimestampMs = timestampMs;
  auto& member = report.values.emplace_back();
  member.dataRef = "IED1LD0/PTRC1.Tr.general";
  member.fc = IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST;
  member.value = value;
  member.timestampMs = timestampMs;
  member.timestampValid = true;
  return report;
}

IEC61850::MmsConnectionEvent MakeMmsConnectionSnapshot(
    IEC61850::ProtocolSessionState state,
    IEC61850Proto::NetworkChannel activeChannel =
        IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED) {
  IEC61850::MmsConnectionEvent event;
  event.type = IEC61850::MmsConnectionEventType::STATE_SNAPSHOT;
  event.state = state;
  event.activeChannel = activeChannel;
  return event;
}

IEC61850::ProtocolGooseFrameView MakeTripGooseFrame(
    std::span<const IEC61850::ProtocolRealtimeValue> values,
    std::uint32_t stateNumber = 1, std::uint32_t sequenceNumber = 0) {
  IEC61850::ProtocolGooseFrameView frame;
  frame.subscriptionId = 1;
  frame.gocbRef = "IED1LD0/LLN0$GO$gcb1";
  frame.dataSetRef = "IED1LD0/LLN0$events";
  frame.goId = "Trip";
  frame.channel = IEC61850Proto::NETWORK_CHANNEL_A;
  frame.appId = 0x1001;
  frame.configRevision = 4;
  frame.timeAllowedToLiveMs = 100;
  frame.stateNumber = stateNumber;
  frame.sequenceNumber = sequenceNumber;
  frame.receiveTimestampNs = 1'000'000;
  frame.values = values;
  return frame;
}

IEC61850::ProtocolSvFrameView MakeSampleSvFrame(
    std::span<const IEC61850::ProtocolRealtimeValue> values,
    std::uint16_t sampleCount = 1) {
  IEC61850::ProtocolSvFrameView frame;
  frame.streamId = 1;
  frame.svId = "MU01";
  frame.channel = IEC61850Proto::NETWORK_CHANNEL_A;
  frame.appId = 0x4001;
  frame.configRevision = 5;
  frame.sampleCount = sampleCount;
  frame.asduCount = 1;
  frame.receiveTimestampNs = 1'000'000;
  frame.values = values;
  return frame;
}

static_assert(std::is_trivially_copyable_v<IEC61850::ProtocolRealtimeValue>);
static_assert(std::is_trivially_copyable_v<IEC61850::ProtocolGooseFrameView>);
static_assert(std::is_trivially_copyable_v<IEC61850::ProtocolSvFrameView>);

void AddMmsChannelStatus(IEC61850::MmsConnectionEvent* event,
                         IEC61850Proto::NetworkChannel channel,
                         IEC61850Proto::ChannelState state,
                         std::string error = {}) {
  auto& status = event->channels.emplace_back();
  status.channel = channel;
  status.state = state;
  status.error = std::move(error);
}

void ImportModel(IEC61850::Manager* manager) {
  IEC61850Proto::ImportSclResponse response;
  const auto status = manager->ImportScl(MakeImportRequest(), &response);
  ASSERT_TRUE(status.ok()) << status.error_message();
}

// 验证：validate_only只返回解析摘要，不修改内存或SQLite配置。
TEST(IEC61850ManagerTest, ValidateOnlyDoesNotPersistModel) {
  TemporaryDirectory directory;
  IEC61850::Manager manager(directory.database());
  IEC61850Proto::ImportSclResponse response;

  ASSERT_TRUE(manager.ImportScl(MakeImportRequest(true), &response).ok());
  EXPECT_EQ(response.summary().ied_count(), 1u);
  IEC61850Proto::ListModelsResponse models;
  ASSERT_TRUE(manager.ListModels(&models).ok());
  EXPECT_TRUE(models.models().empty());

  IEC61850::Manager reloaded(directory.database());
  ASSERT_TRUE(reloaded.LoadPersistedConfig().ok());
  models.Clear();
  ASSERT_TRUE(reloaded.ListModels(&models).ok());
  EXPECT_TRUE(models.models().empty());
}

// 验证：导入模型后能够查询摘要并从SQLite恢复。
TEST(IEC61850ManagerTest, ImportsQueriesAndReloadsModel) {
  TemporaryDirectory directory;
  IEC61850::Manager manager(directory.database());
  ImportModel(&manager);

  IEC61850Proto::SclModelSummary summary;
  ASSERT_TRUE(manager.GetModelSummary("station-model", &summary).ok());
  EXPECT_EQ(summary.source_name(), "station.scd");
  EXPECT_EQ(summary.ied_count(), 1u);
  ASSERT_EQ(summary.ieds_size(), 1);
  EXPECT_EQ(summary.ieds(0).name(), "IED1");
  ASSERT_EQ(summary.ieds(0).access_points_size(), 3);
  EXPECT_EQ(summary.ieds(0).access_points(0).name(), "AP1");
  EXPECT_TRUE(summary.ieds(0).access_points(0).has_server());
  EXPECT_EQ(summary.ieds(0).access_points(2).name(), "AP3");
  EXPECT_TRUE(summary.ieds(0).access_points(2).has_server());

  IEC61850Proto::ListModelsResponse models;
  ASSERT_TRUE(manager.ListModels(&models).ok());
  ASSERT_EQ(models.models_size(), 1);
  ASSERT_EQ(models.models(0).ieds_size(), 1);
  EXPECT_EQ(models.models(0).ieds(0).name(), "IED1");

  IEC61850::Manager reloaded(directory.database());
  ASSERT_TRUE(reloaded.LoadPersistedConfig().ok());
  summary.Clear();
  ASSERT_TRUE(reloaded.GetModelSummary("station-model", &summary).ok());
  EXPECT_EQ(summary.ied_count(), 1u);
  ASSERT_EQ(summary.ieds_size(), 1);
  EXPECT_EQ(summary.ieds(0).name(), "IED1");
}

// 验证：相同内容重复导入幂等，不需要replace=true。
TEST(IEC61850ManagerTest, ReimportingIdenticalModelIsIdempotent) {
  TemporaryDirectory directory;
  IEC61850::Manager manager(directory.database());
  ImportModel(&manager);
  IEC61850Proto::ImportSclResponse response;

  const auto status = manager.ImportScl(MakeImportRequest(), &response);

  EXPECT_TRUE(status.ok()) << status.error_message();
  IEC61850Proto::ListModelsResponse models;
  ASSERT_TRUE(manager.ListModels(&models).ok());
  EXPECT_EQ(models.models_size(), 1);
}

// 验证：聚合目标态任一SCL无效时不覆盖已经持久化的当前模型。
TEST(IEC61850ManagerTest, ApplyTargetConfigIsAtomicWhenAnySclIsInvalid) {
  TemporaryDirectory directory;
  IEC61850::Manager manager(directory.database());
  ImportModel(&manager);
  IEC61850Proto::ApplyTargetConfigRequest request;
  auto* valid = request.add_models();
  valid->set_model_name("new-valid");
  valid->set_source_name("new-valid.scd");
  valid->set_content(kMinimalScl);
  auto* invalid = request.add_models();
  invalid->set_model_name("new-invalid");
  invalid->set_source_name("new-invalid.scd");
  invalid->set_content("<SCL>");
  IEC61850Proto::ApplyTargetConfigResponse response;

  const auto status = manager.ApplyTargetConfig(request, &response);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  IEC61850Proto::ListModelsResponse models;
  ASSERT_TRUE(manager.ListModels(&models).ok());
  ASSERT_EQ(models.models_size(), 1);
  EXPECT_EQ(models.models(0).model_name(), "station-model");
}

// 验证：聚合目标态一次替换模型、IED和点映射并注册DataCenter稳定连接。
TEST(IEC61850ManagerTest, AppliesCompleteTargetConfigInOneSnapshot) {
  TemporaryDirectory directory;
  IEC61850::Manager manager(directory.database());
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  manager.SetDataCenterStub(stub);
  IEC61850Proto::ApplyTargetConfigRequest request;
  auto* model = request.add_models();
  model->set_model_name("station-model");
  model->set_source_name("station.scd");
  model->set_content(kMinimalScl);
  auto* target = request.add_ieds();
  *target->mutable_config() = MakeIedRequest().config();
  target->set_desired_running(false);
  auto* point = target->add_points();
  point->set_tag("TRIP");
  point->set_data_ref("IED1LD0/PTRC1.Tr.general");
  point->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST);
  point->set_source(IEC61850Proto::POINT_SOURCE_GOOSE);
  point->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_BOOL);
  IEC61850Proto::ApplyTargetConfigResponse response;

  const auto status = manager.ApplyTargetConfig(request, &response);

  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(response.models_size(), 1);
  ASSERT_EQ(response.ieds_size(), 1);
  EXPECT_NE(response.ieds(0).conn_id(), 0u);
  EXPECT_TRUE(state.HasConnection("IEC61850", "line-1"));
  IEC61850Proto::PointMappings mappings;
  ASSERT_TRUE(manager.GetPointMappings("line-1", &mappings).ok());
  ASSERT_EQ(mappings.points_size(), 1);
  EXPECT_EQ(mappings.points(0).tag(), "TRIP");
}

// 验证：配置IED时按稳定键注册DataCenter连接并持久化conn_id。
TEST(IEC61850ManagerTest, UpsertIedRegistersStableDataCenterConnection) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  manager.SetDataCenterStub(stub);
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;

  const auto status = manager.UpsertIed(MakeIedRequest(), &info);

  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_NE(info.conn_id(), 0u);
  EXPECT_TRUE(info.data_center_available());
  EXPECT_TRUE(state.HasConnection("IEC61850", "line-1"));

  IEC61850::Manager reloaded(directory.database(), stack);
  ASSERT_TRUE(reloaded.LoadPersistedConfig().ok());
  IEC61850Proto::IedInfo loaded;
  ASSERT_TRUE(reloaded.GetIed("line-1", &loaded).ok());
  EXPECT_EQ(loaded.conn_id(), info.conn_id());
}

// 验证：DataCenter不可用时仍保存GOOSE/SV可用的IED目标配置并报告降级。
TEST(IEC61850ManagerTest, DataCenterFailureDoesNotDiscardIedTargetConfig) {
  TemporaryDirectory directory;
  IEC61850::Manager manager(directory.database());
  auto stub = std::make_shared<DataCenterProto::MockDataCenterServiceStub>();
  ON_CALL(*stub, GetOrCreateConnection(testing::_, testing::_, testing::_))
      .WillByDefault(testing::Return(
          grpc::Status(grpc::StatusCode::UNAVAILABLE, "DataCenter不可用")));
  manager.SetDataCenterStub(stub);
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;

  const auto status = manager.UpsertIed(MakeIedRequest(), &info);

  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(info.conn_id(), 0u);
  EXPECT_FALSE(info.data_center_available());
  IEC61850Proto::IedInfo queried;
  ASSERT_TRUE(manager.GetIed("line-1", &queried).ok());
  EXPECT_EQ(queried.config().ied_name(), "IED1");
}

// 验证：更新点映射后以全量语义同步DataCenter连接标签。
TEST(IEC61850ManagerTest, UpsertPointMappingsSynchronizesDataCenterTags) {
  TemporaryDirectory directory;
  IEC61850::Manager manager(directory.database());
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  manager.SetDataCenterStub(stub);
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(MakeIedRequest(), &info).ok());
  EXPECT_CALL(*stub, UpsertConnTags(testing::_, testing::_, testing::_))
      .WillOnce(testing::Invoke([](grpc::ClientContext*,
                                   const DataCenterProto::UpsertConnTagsRequest& request,
                                   DataCenterProto::Empty*) {
        EXPECT_TRUE(request.replace());
        EXPECT_EQ(request.tags_size(), 1);
        EXPECT_EQ(request.tags(0), "TRIP");
        return grpc::Status::OK;
      }));
  IEC61850Proto::UpsertPointMappingsRequest request;
  request.set_conn_name("line-1");
  request.set_replace(true);
  auto* point = request.add_points();
  point->set_tag("TRIP");
  point->set_data_ref("IED1LD0/PTRC1.Tr.general");
  point->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST);
  point->set_source(IEC61850Proto::POINT_SOURCE_GOOSE);
  point->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_BOOL);

  const auto status = manager.UpsertPointMappings(request);

  EXPECT_TRUE(status.ok()) << status.error_message();
  IEC61850Proto::PointMappings mappings;
  ASSERT_TRUE(manager.GetPointMappings("line-1", &mappings).ok());
  ASSERT_EQ(mappings.points_size(), 1);
  EXPECT_EQ(mappings.points(0).tag(), "TRIP");
}

// 验证：启动和停止IED通信功能委派给协议栈并持久化目标运行状态。
TEST(IEC61850ManagerTest, StartsAndStopsIedThroughProtocolStack) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  manager.SetDataCenterStub(MakeStub(&state));
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(MakeIedRequest(), &info).ok());

  ASSERT_TRUE(manager.StartIed("line-1").ok());
  EXPECT_EQ(stack->startCount, 1);
  EXPECT_EQ(stack->lastStarted, "line-1");
  EXPECT_EQ(stack->lastPlan.config.conn_name(), "line-1");
  EXPECT_EQ(stack->lastPlan.ied.name(), "IED1");
  ASSERT_EQ(stack->lastPlan.ied.access_points_size(), 1);
  EXPECT_EQ(stack->lastPlan.ied.access_points(0).name(), "AP1");
  ASSERT_EQ(stack->lastPlan.connectedAccessPoints.size(), 1u);
  const auto& connectedAp = stack->lastPlan.connectedAccessPoints.front();
  EXPECT_EQ(connectedAp.ied_name(), "IED1");
  EXPECT_EQ(connectedAp.ap_name(), "AP1");
  ASSERT_EQ(connectedAp.address_size(), 1);
  EXPECT_EQ(connectedAp.address(0).type(), "IP");
  EXPECT_EQ(connectedAp.address(0).value(), "192.0.2.10");
  ASSERT_EQ(stack->lastPlan.networkBindings.size(), 1u);
  EXPECT_EQ(stack->lastPlan.networkBindings[0].channel.channel(),
            IEC61850Proto::NETWORK_CHANNEL_A);
  EXPECT_EQ(stack->lastPlan.networkBindings[0]
                .connectedAccessPoint.subnetwork_name(),
            "station");
  ASSERT_EQ(stack->lastPlan.gooseSubscriptions.size(), 1u);
  const auto& goose = stack->lastPlan.gooseSubscriptions.front();
  EXPECT_EQ(goose.controlRef, "IED1LD0/LLN0$GO$gcb1");
  EXPECT_EQ(goose.goId, "Trip");
  ASSERT_EQ(goose.members.size(), 1u);
  EXPECT_EQ(goose.members.front().signalId, 0u);
  ASSERT_EQ(goose.endpoints.size(), 1u);
  EXPECT_EQ(goose.endpoints.front().appId, 0x1001u);
  EXPECT_EQ(goose.endpoints.front().destinationMac,
            "01-0C-CD-01-00-01");
  ASSERT_EQ(stack->lastPlan.ied.data_sets_size(), 2);
  ASSERT_EQ(stack->lastPlan.ied.data_sets(0).name(), "events");
  ASSERT_EQ(stack->lastPlan.ied.data_sets(0).members_size(), 1);
  EXPECT_EQ(stack->lastPlan.ied.data_sets(0).members(0).data_ref(),
            "IED1LD0/PTRC1.Tr.general");
  ASSERT_EQ(stack->lastPlan.ied.data_sets(1).name(), "samples");
  ASSERT_EQ(stack->lastPlan.ied.data_sets(1).members_size(), 1);
  EXPECT_EQ(stack->lastPlan.ied.data_sets(1).members(0).data_ref(),
            "IED1LD0/PTRC1.Tr.measurement");
  ASSERT_EQ(stack->lastPlan.ied.report_controls_size(), 1);
  const auto& reportControl = stack->lastPlan.ied.report_controls(0);
  EXPECT_TRUE(reportControl.buffered());
  EXPECT_EQ(reportControl.config_revision(), 3u);
  EXPECT_TRUE(reportControl.trigger_options().general_interrogation());
  EXPECT_TRUE(reportControl.optional_fields().segmentation());
  for (const auto& node : stack->lastPlan.ied.logical_nodes()) {
    EXPECT_EQ(node.access_point(), "AP1");
  }
  for (const auto& object : stack->lastPlan.ied.data_objects()) {
    EXPECT_EQ(object.access_point(), "AP1");
  }
  for (const auto& attribute : stack->lastPlan.ied.data_attributes()) {
    EXPECT_EQ(attribute.access_point(), "AP1");
  }
  for (const auto& dataSet : stack->lastPlan.ied.data_sets()) {
    EXPECT_EQ(dataSet.access_point(), "AP1");
  }
  for (const auto& control : stack->lastPlan.ied.report_controls()) {
    EXPECT_EQ(control.access_point(), "AP1");
  }
  ASSERT_EQ(stack->lastPlan.ied.gse_controls_size(), 1);
  EXPECT_EQ(stack->lastPlan.ied.gse_controls(0).access_point(), "AP1");
  EXPECT_EQ(stack->lastPlan.ied.gse_controls(0).name(), "gcb1");
  ASSERT_EQ(stack->lastPlan.ied.sampled_value_controls_size(), 1);
  EXPECT_EQ(stack->lastPlan.ied.sampled_value_controls(0).access_point(),
            "AP1");
  EXPECT_EQ(stack->lastPlan.ied.sampled_value_controls(0).name(), "smv1");
  ASSERT_EQ(stack->lastPlan.ied.ext_refs_size(), 2);
  EXPECT_EQ(stack->lastPlan.ied.ext_refs(0).access_point(), "AP1");
  EXPECT_EQ(stack->lastPlan.ied.ext_refs(0).int_addr(), "trip-in");
  ASSERT_TRUE(manager.GetIed("line-1", &info).ok());
  EXPECT_EQ(info.state(), IEC61850Proto::IED_STATE_RUNNING);

  ASSERT_TRUE(manager.StopIed("line-1").ok());
  EXPECT_EQ(stack->stopCount, 1);
  EXPECT_EQ(stack->lastStopped, "line-1");
  ASSERT_TRUE(manager.GetIed("line-1", &info).ok());
  EXPECT_EQ(info.state(), IEC61850Proto::IED_STATE_STOPPED);
}

// 验证：Manager只允许已启动且READY的MMS会话转发Read/Write，并在停止后拒绝请求。
TEST(IEC61850ManagerTest, ForwardsMmsReadWriteOnlyWhenIedIsReady) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  manager.SetDataCenterStub(MakeStub(&state));
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(MakeMmsIedRequest(), &info).ok());

  IEC61850::MmsReadRequest readRequest;
  auto& readVariable = readRequest.variables.emplace_back();
  readVariable.type = IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC;
  readVariable.domain = "IED1LD0";
  readVariable.identifier = "LLN0$ST$Tr$general";
  IEC61850::MmsReadResponse expectedRead;
  auto& readItem = expectedRead.items.emplace_back();
  readItem.success = true;
  readItem.encodedData = {0x83, 0x01, 0x01};
  stack->readResponse = expectedRead;

  IEC61850::MmsReadResponse readResponse;
  EXPECT_EQ(manager.ReadMms("line-1", readRequest, &readResponse).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(stack->readCount, 0);

  ASSERT_TRUE(manager.StartIed("line-1").ok());
  ASSERT_TRUE(manager.ReadMms("line-1", readRequest, &readResponse).ok());
  EXPECT_EQ(stack->readCount, 1);
  EXPECT_EQ(stack->lastReadConnName, "line-1");
  EXPECT_EQ(stack->lastReadRequest.variables.size(), 1u);
  ASSERT_EQ(readResponse.items.size(), expectedRead.items.size());
  EXPECT_EQ(readResponse.items.front().success,
            expectedRead.items.front().success);
  EXPECT_EQ(readResponse.items.front().encodedData,
            expectedRead.items.front().encodedData);

  IEC61850::MmsWriteRequest writeRequest;
  auto& writeItem = writeRequest.items.emplace_back();
  writeItem.variable = readVariable;
  writeItem.encodedData = {0x83, 0x01, 0x01};
  IEC61850::MmsWriteResponse expectedWrite;
  expectedWrite.items.emplace_back().success = true;
  stack->writeResponse = expectedWrite;

  IEC61850::MmsWriteResponse writeResponse;
  ASSERT_TRUE(manager.WriteMms("line-1", writeRequest, &writeResponse).ok());
  EXPECT_EQ(stack->writeCount, 1);
  EXPECT_EQ(stack->lastWriteConnName, "line-1");
  EXPECT_EQ(stack->lastWriteRequest.items.size(), 1u);
  ASSERT_EQ(writeResponse.items.size(), expectedWrite.items.size());
  EXPECT_EQ(writeResponse.items.front().success,
            expectedWrite.items.front().success);

  ASSERT_TRUE(manager.StopIed("line-1").ok());
  EXPECT_EQ(manager.ReadMms("line-1", readRequest, &readResponse).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(manager.WriteMms("line-1", writeRequest, &writeResponse)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(stack->readCount, 1);
  EXPECT_EQ(stack->writeCount, 1);
}

// 验证：Manager对MMS控制参数、协议栈状态和协议栈异常返回明确中文错误。
TEST(IEC61850ManagerTest, ValidatesMmsControlRequestsAndConvertsExceptions) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  manager.SetDataCenterStub(MakeStub(&state));
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(MakeMmsIedRequest(), &info).ok());

  IEC61850::MmsReadRequest emptyRead;
  IEC61850::MmsReadResponse readResponse;
  EXPECT_EQ(manager.ReadMms("", emptyRead, &readResponse).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  IEC61850::MmsReadRequest validRead;
  validRead.variables.emplace_back().identifier = "LLN0$ST$Tr$general";
  EXPECT_EQ(manager.ReadMms("missing", validRead, &readResponse).error_code(),
            grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(manager.ReadMms("line-1", emptyRead, &readResponse).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(manager.ReadMms("line-1", IEC61850::MmsReadRequest{}, nullptr)
                .error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  IEC61850::MmsReadRequest readRequest = validRead;
  ASSERT_TRUE(manager.StartIed("line-1").ok());
  stack->readStatus = grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                   "IED通信中断");
  EXPECT_EQ(manager.ReadMms("line-1", readRequest, &readResponse).error_code(),
            grpc::StatusCode::UNAVAILABLE);
  EXPECT_TRUE(readResponse.items.empty());

  stack->readStatus = grpc::Status::OK;
  stack->throwOnRead = true;
  const auto exceptionStatus =
      manager.ReadMms("line-1", readRequest, &readResponse);
  EXPECT_EQ(exceptionStatus.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_NE(exceptionStatus.error_message().find("异常"), std::string::npos);
}

// 验证：MMS控制在IED未就绪时被拒绝，合法命令转发到协议栈，非法命令和异常均返回明确状态。
TEST(IEC61850ManagerTest,
     ForwardsMmsControlOnlyWhenIedIsReadyAndConvertsExceptions) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  manager.SetDataCenterStub(MakeStub(&state));
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(MakeMmsIedRequest(), &info).ok());

  IEC61850::MmsObjectName controlObject;
  controlObject.type = IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC;
  controlObject.domain = "IED1LD0";
  controlObject.identifier = "CSWI1$Pos";

  IEC61850::MmsControlCommand command;
  command.operation = IEC61850::MmsControlOperation::OPERATE;
  command.controlObject = controlObject;
  command.controlValue = {0x83, 0x01, 0xff};
  command.controlNumber = 7;
  command.originCategory = 2;
  command.originIdentifier = {192, 0, 2, 10};
  command.timestampMs = 1123;
  command.check = 1;

  IEC61850::MmsReadResponse selectResponse;
  IEC61850::MmsWriteResponse writeResponse;
  EXPECT_EQ(manager.SelectMmsControl("line-1", controlObject, &selectResponse)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(manager.WriteMmsControl("line-1", command, &writeResponse)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(stack->selectControlCount, 0);
  EXPECT_EQ(stack->writeControlCount, 0);

  auto invalidCommand = command;
  invalidCommand.operation = IEC61850::MmsControlOperation::SELECT;
  EXPECT_EQ(manager.WriteMmsControl("line-1", invalidCommand, &writeResponse)
                .error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(stack->writeControlCount, 0);

  ASSERT_TRUE(manager.StartIed("line-1").ok());

  stack->selectControlResponse.items.emplace_back().success = true;
  stack->selectControlResponse.items.front().encodedData = {0x83, 0x01, 0x01};
  stack->writeControlResponse.items.emplace_back().success = true;

  ASSERT_TRUE(
      manager.SelectMmsControl("line-1", controlObject, &selectResponse).ok());
  EXPECT_EQ(stack->selectControlCount, 1);
  EXPECT_EQ(stack->lastSelectControlConnName, "line-1");
  EXPECT_EQ(stack->lastSelectControlObject.type,
            IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC);
  EXPECT_EQ(stack->lastSelectControlObject.domain, "IED1LD0");
  EXPECT_EQ(stack->lastSelectControlObject.identifier, "CSWI1$Pos");
  ASSERT_EQ(selectResponse.items.size(), 1u);
  EXPECT_TRUE(selectResponse.items.front().success);
  EXPECT_EQ(selectResponse.items.front().encodedData,
            std::vector<std::uint8_t>({0x83, 0x01, 0x01}));

  ASSERT_TRUE(manager.WriteMmsControl("line-1", command, &writeResponse).ok());
  EXPECT_EQ(stack->writeControlCount, 1);
  EXPECT_EQ(stack->lastWriteControlConnName, "line-1");
  EXPECT_EQ(stack->lastWriteControlCommand.operation,
            IEC61850::MmsControlOperation::OPERATE);
  EXPECT_EQ(stack->lastWriteControlCommand.controlObject.identifier,
            "CSWI1$Pos");
  EXPECT_EQ(stack->lastWriteControlCommand.controlValue,
            std::vector<std::uint8_t>({0x83, 0x01, 0xff}));
  ASSERT_EQ(writeResponse.items.size(), 1u);
  EXPECT_TRUE(writeResponse.items.front().success);

  stack->throwOnSelectControl = true;
  selectResponse.items.emplace_back().success = true;
  const auto selectException =
      manager.SelectMmsControl("line-1", controlObject, &selectResponse);
  EXPECT_EQ(selectException.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_TRUE(selectResponse.items.empty());
  stack->throwOnSelectControl = false;

  stack->throwOnWriteControl = true;
  writeResponse.items.emplace_back().success = true;
  const auto writeException =
      manager.WriteMmsControl("line-1", command, &writeResponse);
  EXPECT_EQ(writeException.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_TRUE(writeResponse.items.empty());
}

// 验证：DataCenter同步命令按目标点映射校验状态和值类型，并把BOOL、整数和浮点控制转发到MMS协议栈。
TEST(IEC61850ManagerTest,
     ExecutesDataCenterMmsControlAndValidatesResults) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  stack->pointControlResponse.items.emplace_back().success = true;
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  manager.SetDataCenterStub(MakeStub(&state));
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(MakeMmsIedRequest(), &info).ok());
  const auto mappingStatus =
      manager.UpsertPointMappings(MakeMmsControlMappingRequest());
  ASSERT_TRUE(mappingStatus.ok()) << mappingStatus.error_message();

  auto makeRequest = [](std::string_view tag) {
    DataCenterProto::ExecuteCommandRequest request;
    request.mutable_dst()->set_module_name("IEC61850");
    request.mutable_dst()->set_conn_name("line-1");
    request.mutable_dst()->set_tag(std::string(tag));
    request.set_quality(DataCenterProto::QUALITY_GOOD);
    return request;
  };

  auto stoppedRequest = makeRequest("BOOL_CONTROL");
  stoppedRequest.mutable_value()->set_bool_value(true);
  DataCenterProto::ExecuteCommandResponse response;
  ASSERT_TRUE(manager.ExecuteDataCenterCommand(stoppedRequest, &response).ok());
  EXPECT_EQ(response.status(), DataCenterProto::COMMAND_TARGET_UNAVAILABLE);
  EXPECT_EQ(stack->pointControlCount, 0);

  ASSERT_TRUE(manager.StartIed("line-1").ok());

  auto unknownRequest = makeRequest("UNKNOWN_CONTROL");
  unknownRequest.mutable_value()->set_bool_value(true);
  response.Clear();
  ASSERT_TRUE(manager.ExecuteDataCenterCommand(unknownRequest, &response).ok());
  EXPECT_EQ(response.status(), DataCenterProto::COMMAND_REJECTED);
  EXPECT_EQ(response.reject_code(),
            DataCenterProto::COMMAND_REJECT_UNSUPPORTED_POINT);
  EXPECT_EQ(stack->pointControlCount, 0);

  auto boolRequest = makeRequest("BOOL_CONTROL");
  boolRequest.mutable_value()->set_bool_value(true);
  response.Clear();
  ASSERT_TRUE(manager.ExecuteDataCenterCommand(boolRequest, &response).ok());
  EXPECT_EQ(response.status(), DataCenterProto::COMMAND_ACCEPTED);
  EXPECT_DOUBLE_EQ(response.requested_value(), 1.0);
  EXPECT_DOUBLE_EQ(response.accepted_value(), 1.0);
  EXPECT_EQ(stack->lastPointControlConnName, "line-1");
  EXPECT_EQ(stack->lastPointControlCommand.valueType,
            IEC61850Proto::POINT_VALUE_TYPE_BOOL);
  EXPECT_TRUE(stack->lastPointControlCommand.boolValue);

  auto integerRequest = makeRequest("INTEGER_CONTROL");
  integerRequest.mutable_value()->set_int_value(7);
  response.Clear();
  ASSERT_TRUE(manager.ExecuteDataCenterCommand(integerRequest, &response).ok());
  EXPECT_EQ(response.status(), DataCenterProto::COMMAND_ACCEPTED);
  EXPECT_EQ(stack->lastPointControlCommand.valueType,
            IEC61850Proto::POINT_VALUE_TYPE_INT64);
  EXPECT_EQ(stack->lastPointControlCommand.intValue, 7);
  EXPECT_DOUBLE_EQ(stack->lastPointControlCommand.scale, 2.0);
  EXPECT_DOUBLE_EQ(stack->lastPointControlCommand.offset, 3.0);

  auto floatingRequest = makeRequest("FLOAT_CONTROL");
  floatingRequest.mutable_value()->set_double_value(2.5);
  response.Clear();
  ASSERT_TRUE(manager.ExecuteDataCenterCommand(floatingRequest, &response).ok());
  EXPECT_EQ(response.status(), DataCenterProto::COMMAND_ACCEPTED);
  EXPECT_EQ(stack->lastPointControlCommand.valueType,
            IEC61850Proto::POINT_VALUE_TYPE_DOUBLE);
  EXPECT_DOUBLE_EQ(stack->lastPointControlCommand.doubleValue, 2.5);
  EXPECT_DOUBLE_EQ(stack->lastPointControlCommand.scale, 4.0);
  EXPECT_DOUBLE_EQ(stack->lastPointControlCommand.offset, -1.0);

  auto boundedRequest = makeRequest("BOOL_CONTROL");
  boundedRequest.mutable_value()->set_bool_value(true);
  boundedRequest.set_timeout_ms(37);
  response.Clear();
  ASSERT_TRUE(
      manager.ExecuteDataCenterCommand(boundedRequest, &response).ok());
  EXPECT_EQ(response.status(), DataCenterProto::COMMAND_ACCEPTED);
  ASSERT_TRUE(stack->lastPointControlCommand.requestTimeout.has_value());
  EXPECT_EQ(stack->lastPointControlCommand.requestTimeout->count(), 37);

  auto overflowRequest = makeRequest("INTEGER_CONTROL");
  overflowRequest.mutable_value()->set_double_value(0x1p63);
  response.Clear();
  ASSERT_TRUE(
      manager.ExecuteDataCenterCommand(overflowRequest, &response).ok());
  EXPECT_EQ(response.status(), DataCenterProto::COMMAND_REJECTED);
  EXPECT_EQ(response.reject_code(),
            DataCenterProto::COMMAND_REJECT_UNSUPPORTED_POINT);
  EXPECT_EQ(stack->pointControlCount, 4);

  auto badQualityRequest = makeRequest("BOOL_CONTROL");
  badQualityRequest.mutable_value()->set_bool_value(true);
  badQualityRequest.set_quality(DataCenterProto::QUALITY_BAD);
  response.Clear();
  ASSERT_TRUE(
      manager.ExecuteDataCenterCommand(badQualityRequest, &response).ok());
  EXPECT_EQ(response.status(), DataCenterProto::COMMAND_REJECTED);
  EXPECT_EQ(stack->pointControlCount, 4);

  auto badTypeRequest = makeRequest("BOOL_CONTROL");
  badTypeRequest.mutable_value()->set_string_value("true");
  response.Clear();
  ASSERT_TRUE(manager.ExecuteDataCenterCommand(badTypeRequest, &response).ok());
  EXPECT_EQ(response.status(), DataCenterProto::COMMAND_REJECTED);
  EXPECT_EQ(stack->pointControlCount, 4);

  auto mismatchedIdRequest = makeRequest("BOOL_CONTROL");
  mismatchedIdRequest.mutable_value()->set_bool_value(true);
  mismatchedIdRequest.mutable_dst()->set_conn_id(info.conn_id() + 1);
  response.Clear();
  ASSERT_TRUE(
      manager.ExecuteDataCenterCommand(mismatchedIdRequest, &response).ok());
  EXPECT_EQ(response.status(), DataCenterProto::COMMAND_REJECTED);
  EXPECT_EQ(stack->pointControlCount, 4);

  stack->pointControlStatus =
      grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "MMS控制超时");
  response.Clear();
  ASSERT_TRUE(manager.ExecuteDataCenterCommand(boolRequest, &response).ok());
  EXPECT_EQ(response.status(), DataCenterProto::COMMAND_TIMEOUT);
  EXPECT_EQ(stack->pointControlCount, 5);
  stack->pointControlStatus = grpc::Status::OK;
  stack->throwOnPointControl = true;
  response.Clear();
  ASSERT_TRUE(manager.ExecuteDataCenterCommand(boolRequest, &response).ok());
  EXPECT_EQ(response.status(), DataCenterProto::COMMAND_INTERNAL_ERROR);
  EXPECT_EQ(stack->pointControlCount, 6);

  ASSERT_TRUE(manager.StopIed("line-1").ok());
  manager.Shutdown();
  response.Clear();
  EXPECT_EQ(manager.ExecuteDataCenterCommand(boolRequest, &response)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证协议栈返回前收到取消时，Manager不会把命令伪报为已接受。
TEST(IEC61850ManagerTest, RejectsDataCenterCommandCancelledBeforeReturn) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  stack->pointControlResponse.items.emplace_back().success = true;
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  manager.SetDataCenterStub(MakeStub(&state));
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(MakeMmsIedRequest(), &info).ok());
  ASSERT_TRUE(manager.UpsertPointMappings(MakeMmsControlMappingRequest()).ok());
  ASSERT_TRUE(manager.StartIed("line-1").ok());

  DataCenterProto::ExecuteCommandRequest request;
  request.mutable_dst()->set_module_name("IEC61850");
  request.mutable_dst()->set_conn_name("line-1");
  request.mutable_dst()->set_tag("BOOL_CONTROL");
  request.mutable_value()->set_bool_value(true);
  request.set_quality(DataCenterProto::QUALITY_GOOD);
  auto cancellation = std::make_shared<std::atomic_bool>(false);
  DataCenterProto::ExecuteCommandResponse response;

  stack->BlockPointControl();
  grpc::Status commandStatus;
  std::thread commandThread([&] {
    commandStatus = manager.ExecuteDataCenterCommand(
        request, &response, cancellation);
  });
  if (!stack->WaitUntilPointControlEntered()) {
    stack->ReleasePointControl();
    commandThread.join();
    ADD_FAILURE() << "同步控制未进入协议栈调用";
    return;
  }
  cancellation->store(true, std::memory_order_release);
  stack->ReleasePointControl();
  commandThread.join();

  EXPECT_EQ(commandStatus.error_code(), grpc::StatusCode::CANCELLED);
  EXPECT_EQ(response.status(), DataCenterProto::COMMAND_STATUS_UNSPECIFIED);
  EXPECT_TRUE(response.reason().empty());
  ASSERT_TRUE(manager.StopIed("line-1").ok());
  manager.Shutdown();
}

// 验证在途DataCenter控制完成前Stop不会并发关闭同一IED的协议会话。
TEST(IEC61850ManagerTest, SerializesStopBehindInFlightDataCenterCommand) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  stack->pointControlResponse.items.emplace_back().success = true;
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  manager.SetDataCenterStub(MakeStub(&state));
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(MakeMmsIedRequest(), &info).ok());
  ASSERT_TRUE(manager.UpsertPointMappings(MakeMmsControlMappingRequest()).ok());
  ASSERT_TRUE(manager.StartIed("line-1").ok());

  DataCenterProto::ExecuteCommandRequest request;
  request.mutable_dst()->set_module_name("IEC61850");
  request.mutable_dst()->set_conn_name("line-1");
  request.mutable_dst()->set_tag("BOOL_CONTROL");
  request.mutable_value()->set_bool_value(true);
  request.set_quality(DataCenterProto::QUALITY_GOOD);
  DataCenterProto::ExecuteCommandResponse response;

  stack->BlockPointControl();
  grpc::Status commandStatus;
  std::jthread commandThread([&] {
    commandStatus = manager.ExecuteDataCenterCommand(request, &response);
  });
  ASSERT_TRUE(stack->WaitUntilPointControlEntered());

  grpc::Status stopStatus;
  std::jthread stopThread([&] { stopStatus = manager.StopIed("line-1"); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(stack->stopCount, 0);

  stack->ReleasePointControl();
  commandThread.join();
  stopThread.join();

  ASSERT_TRUE(commandStatus.ok()) << commandStatus.error_message();
  ASSERT_TRUE(stopStatus.ok()) << stopStatus.error_message();
  EXPECT_EQ(response.status(), DataCenterProto::COMMAND_ACCEPTED);
  EXPECT_EQ(stack->stopCount, 1);
}

// 验证：GOOSE帧直接进入当前会话的实时入口，停止和重启后的旧回调不会污染新会话。
TEST(IEC61850ManagerTest, RoutesGooseFrameThroughRealtimeSessionGeneration) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState dataCenterState;
  manager.SetDataCenterStub(MakeStub(&dataCenterState));
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(MakeIedRequest(), &info).ok());
  ASSERT_TRUE(manager.UpsertPointMappings(MakeTripMappingRequest()).ok());
  ASSERT_TRUE(manager.StartIed("line-1").ok());
  ASSERT_TRUE(static_cast<bool>(stack->callbacks.onGooseFrame));
  ASSERT_EQ(stack->lastPlan.realtimeSignals.size(), 1u);
  ASSERT_EQ(stack->lastPlan.gooseSubscriptions.size(), 1u);

  IEC61850::ProtocolRealtimeValue value;
  value.valueType = IEC61850::ProtocolRealtimeValueType::BOOLEAN;
  value.value.booleanValue = true;
  const std::array values{value};
  stack->EmitGooseFrame(MakeTripGooseFrame(values));

  IEC61850Proto::RuntimeStatistics statistics;
  ASSERT_TRUE(manager.GetRuntimeStatistics("line-1", &statistics).ok());
  EXPECT_EQ(statistics.goose_frames_received(), 1u);

  const auto staleCallbacks = stack->CopyCallbacks();
  ASSERT_TRUE(manager.StopIed("line-1").ok());
  FakeProtocolStack::EmitGooseFrameWith(staleCallbacks,
                                        MakeTripGooseFrame(values, 2, 0));
  ASSERT_TRUE(manager.GetRuntimeStatistics("line-1", &statistics).ok());
  EXPECT_EQ(statistics.goose_frames_received(), 1u);

  ASSERT_TRUE(manager.StartIed("line-1").ok());
  stack->EmitGooseFrame(MakeTripGooseFrame(values));
  ASSERT_TRUE(manager.GetRuntimeStatistics("line-1", &statistics).ok());
  EXPECT_EQ(statistics.goose_frames_received(), 2u);
}

// 验证：GOOSE输入经实时总线进入保护引擎，并按稳定引用触发一次GOOSE动作。
TEST(IEC61850ManagerTest, RoutesGooseFrameThroughProtectionEngine) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState dataCenterState;
  manager.SetDataCenterStub(MakeStub(&dataCenterState));
  ImportModel(&manager);
  auto request = MakeIedRequest();
  auto* rule = request.mutable_config()->add_protection_rules();
  rule->set_rule_id("trip-rule");
  auto* condition = rule->add_conditions();
  condition->set_data_ref("IED1LD0/PTRC1.Tr.general");
  condition->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST);
  condition->set_comparator(
      IEC61850Proto::PROTECTION_COMPARATOR_BOOL_TRUE);
  condition->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_BOOL);
  rule->set_output_control_ref("IED1LD0/LLN0$GO$gcb1");
  rule->add_assert_values()->set_value_type(
      IEC61850Proto::POINT_VALUE_TYPE_BOOL);
  rule->mutable_assert_values(0)->set_bool_value(true);
  rule->add_release_values()->set_value_type(
      IEC61850Proto::POINT_VALUE_TYPE_BOOL);
  rule->mutable_release_values(0)->set_bool_value(false);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(request, &info).ok());
  ASSERT_TRUE(manager.UpsertPointMappings(MakeTripMappingRequest()).ok());
  ASSERT_TRUE(manager.StartIed("line-1").ok());

  IEC61850::ProtocolRealtimeValue value;
  value.valueType = IEC61850::ProtocolRealtimeValueType::BOOLEAN;
  value.timestampNs = 1'000'000;
  value.value.booleanValue = true;
  const std::array values{value};
  stack->EmitGooseFrame(MakeTripGooseFrame(values));
  for (int attempt = 0; attempt != 100 && stack->GoosePublishCount() == 0;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(stack->GoosePublishCount(), 1);
  EXPECT_NE(stack->GoosePublishThread(), std::this_thread::get_id());
  {
    std::lock_guard lock(stack->gooseMutex);
    EXPECT_EQ(stack->lastGoosePublishConnName, "line-1");
    EXPECT_EQ(stack->lastGoosePublishPublisherId, 1u);
    EXPECT_EQ(stack->lastGoosePublishSubscriptionId, 1u);
    ASSERT_EQ(stack->lastGoosePublishValues.size(), 1u);
    EXPECT_TRUE(stack->lastGoosePublishValues.front().value.booleanValue);
  }
  ASSERT_TRUE(manager.StopIed("line-1").ok());
}

// 验证SV启动计划和帧回调进入当前会话的SV状态引擎与统计链。
TEST(IEC61850ManagerTest, RoutesSvFrameThroughRealtimeEngine) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState dataCenterState;
  manager.SetDataCenterStub(MakeStub(&dataCenterState));
  ImportModel(&manager);
  auto request = MakeIedRequest();
  request.mutable_config()->set_enable_mms(false);
  request.mutable_config()->set_enable_goose(false);
  request.mutable_config()->set_enable_sv(true);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(request, &info).ok());

  ASSERT_TRUE(manager.StartIed("line-1").ok());
  ASSERT_TRUE(static_cast<bool>(stack->callbacks.onSvFrame));
  ASSERT_EQ(stack->lastPlan.svStreams.size(), 1u);
  EXPECT_EQ(stack->lastPlan.svStreams.front().svId, "MU01");
  ASSERT_EQ(stack->lastPlan.svStreams.front().endpoints.size(), 1u);
  EXPECT_EQ(stack->lastPlan.svStreams.front().endpoints.front().appId,
            0x4001u);
  ASSERT_EQ(stack->lastPlan.svStreams.front().derivedMembers.size(), 1u);
  EXPECT_EQ(stack->lastPlan.svStreams.front().derivedMembers.front().rmsDataRef,
            "SV_DERIVED/1/RMS/IED1LD0/PTRC1.Tr.measurement");

  IEC61850::ProtocolRealtimeValue value;
  value.valueType = IEC61850::ProtocolRealtimeValueType::FLOATING;
  value.value.floatingValue = 2.0;
  const std::array values{value};
  for (std::uint16_t sampleCount = 1; sampleCount <= 4; ++sampleCount) {
    stack->EmitSvFrame(MakeSampleSvFrame(values, sampleCount));
  }
  IEC61850Proto::RuntimeStatistics statistics;
  ASSERT_TRUE(manager.GetRuntimeStatistics("line-1", &statistics).ok());
  EXPECT_EQ(statistics.sv_frames_received(), 4u);

  auto invalid = MakeSampleSvFrame(values);
  invalid.svId = "OTHER";
  stack->EmitSvFrame(invalid);
  ASSERT_TRUE(manager.GetRuntimeStatistics("line-1", &statistics).ok());
  EXPECT_EQ(statistics.sv_frames_invalid(), 1u);
}

// 验证SV单周期RMS派生量经过实时消费者进入保护引擎并触发GOOSE发布。
TEST(IEC61850ManagerTest, RoutesSvMathOutputThroughProtectionEngine) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState dataCenterState;
  manager.SetDataCenterStub(MakeStub(&dataCenterState));
  ImportModel(&manager);
  auto request = MakeIedRequest();
  request.mutable_config()->set_enable_mms(false);
  request.mutable_config()->set_enable_sv(true);
  auto* rule = request.mutable_config()->add_protection_rules();
  rule->set_rule_id("sv-rms-trip");
  auto* condition = rule->add_conditions();
  condition->set_data_ref(
      "SV_DERIVED/1/RMS/IED1LD0/PTRC1.Tr.measurement");
  condition->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX);
  condition->set_comparator(
      IEC61850Proto::PROTECTION_COMPARATOR_GREATER_THAN);
  condition->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_DOUBLE);
  condition->set_double_value(1.0);
  rule->set_output_control_ref("IED1LD0/LLN0$GO$gcb1");
  rule->add_assert_values()->set_value_type(
      IEC61850Proto::POINT_VALUE_TYPE_BOOL);
  rule->mutable_assert_values(0)->set_bool_value(true);
  rule->add_release_values()->set_value_type(
      IEC61850Proto::POINT_VALUE_TYPE_BOOL);
  rule->mutable_release_values(0)->set_bool_value(false);

  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(request, &info).ok());
  ASSERT_TRUE(manager.UpsertPointMappings(MakeTripMappingRequest()).ok());
  ASSERT_TRUE(manager.StartIed("line-1").ok());
  ASSERT_EQ(stack->lastPlan.svStreams.front().derivedMembers.size(), 1u);
  EXPECT_EQ(stack->lastPlan.svStreams.front().derivedMembers.front().rmsSignalId,
            3u);

  IEC61850::ProtocolRealtimeValue value;
  value.valueType = IEC61850::ProtocolRealtimeValueType::FLOATING;
  value.value.floatingValue = 2.0;
  const std::array values{value};
  for (std::uint16_t sampleCount = 1; sampleCount <= 4; ++sampleCount) {
    stack->EmitSvFrame(MakeSampleSvFrame(values, sampleCount));
  }
  for (int attempt = 0; attempt != 100 && stack->GoosePublishCount() == 0;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(stack->GoosePublishCount(), 1);
  {
    std::lock_guard lock(stack->gooseMutex);
    ASSERT_EQ(stack->lastGoosePublishValues.size(), 1u);
    EXPECT_TRUE(stack->lastGoosePublishValues.front().value.booleanValue);
  }
  ASSERT_TRUE(manager.StopIed("line-1").ok());
}

// 验证：A/B通道按subnetwork_name绑定各自ConnectedAP，不依赖模型数组顺序猜测网络。
TEST(IEC61850ManagerTest, BindsEachNetworkChannelToConfiguredSubnetwork) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  manager.SetDataCenterStub(MakeStub(&state));
  ImportModel(&manager);
  auto request = MakeIedRequest();
  auto* channelB = request.mutable_config()->add_channels();
  channelB->set_channel(IEC61850Proto::NETWORK_CHANNEL_B);
  channelB->set_enabled(true);
  channelB->set_interface_name("eth1");
  channelB->set_subnetwork_name("station-b");
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(request, &info).ok());
  ASSERT_TRUE(manager.UpsertPointMappings(MakeTripMappingRequest()).ok());

  ASSERT_TRUE(manager.StartIed("line-1").ok());

  ASSERT_EQ(stack->lastPlan.networkBindings.size(), 2u);
  EXPECT_EQ(stack->lastPlan.networkBindings[0].channel.channel(),
            IEC61850Proto::NETWORK_CHANNEL_A);
  EXPECT_EQ(stack->lastPlan.networkBindings[0]
                .connectedAccessPoint.subnetwork_name(),
            "station");
  EXPECT_EQ(stack->lastPlan.networkBindings[1].channel.channel(),
            IEC61850Proto::NETWORK_CHANNEL_B);
  EXPECT_EQ(stack->lastPlan.networkBindings[1]
                .connectedAccessPoint.subnetwork_name(),
            "station-b");
  EXPECT_EQ(stack->lastPlan.connectedAccessPoints.size(), 2u);
  ASSERT_EQ(stack->lastPlan.realtimeSignals.size(), 1u);
  EXPECT_EQ(stack->lastPlan.realtimeSignals.front().signalId, 1u);
  ASSERT_EQ(stack->lastPlan.gooseSubscriptions.size(), 1u);
  const auto& goose = stack->lastPlan.gooseSubscriptions.front();
  ASSERT_EQ(goose.members.size(), 1u);
  EXPECT_EQ(goose.members.front().signalId, 1u);
  ASSERT_EQ(goose.endpoints.size(), 2u);
  EXPECT_EQ(goose.endpoints[0].appId, 0x1001u);
  EXPECT_EQ(goose.endpoints[1].appId, 0x1002u);
}

// 验证：同一IED和AP存在多个通信网段时，空subnetwork_name不能被模糊归属。
TEST(IEC61850ManagerTest, RejectsAmbiguousNetworkChannelBinding) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  manager.SetDataCenterStub(MakeStub(&state));
  ImportModel(&manager);
  auto request = MakeIedRequest();
  request.mutable_config()->mutable_channels(0)->clear_subnetwork_name();
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(request, &info).ok());

  const auto status = manager.StartIed("line-1");

  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_NE(status.error_message().find("subnetwork_name"),
            std::string::npos);
  EXPECT_EQ(stack->startCount, 0);
}

// 验证：选择第二个Server AP时，启动计划只包含AP2模型和通信记录，不混入AP1对象。
TEST(IEC61850ManagerTest, BuildsProtocolPlanForSelectedSecondAccessPoint) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  manager.SetDataCenterStub(MakeStub(&state));
  ImportModel(&manager);
  auto request = MakeIedRequest();
  request.mutable_config()->set_access_point("AP2");
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(request, &info).ok());

  ASSERT_TRUE(manager.StartIed("line-1").ok());

  ASSERT_EQ(stack->lastPlan.ied.access_points_size(), 1);
  EXPECT_EQ(stack->lastPlan.ied.access_points(0).name(), "AP2");
  ASSERT_EQ(stack->lastPlan.connectedAccessPoints.size(), 1u);
  EXPECT_EQ(stack->lastPlan.connectedAccessPoints.front().ap_name(), "AP2");
  EXPECT_EQ(stack->lastPlan.connectedAccessPoints.front().address(0).value(),
            "192.0.2.20");
  ASSERT_EQ(stack->lastPlan.ied.data_sets_size(), 1);
  EXPECT_EQ(stack->lastPlan.ied.data_sets(0).name(), "other-events");
  EXPECT_EQ(stack->lastPlan.ied.data_sets(0).access_point(), "AP2");
  ASSERT_EQ(stack->lastPlan.ied.report_controls_size(), 1);
  EXPECT_EQ(stack->lastPlan.ied.report_controls(0).name(), "brcb2");
  EXPECT_EQ(stack->lastPlan.ied.report_controls(0).config_revision(), 9u);
  ASSERT_EQ(stack->lastPlan.ied.gse_controls_size(), 1);
  EXPECT_EQ(stack->lastPlan.ied.gse_controls(0).name(), "gcb2");
  ASSERT_EQ(stack->lastPlan.ied.sampled_value_controls_size(), 1);
  EXPECT_EQ(stack->lastPlan.ied.sampled_value_controls(0).name(), "smv2");
  ASSERT_EQ(stack->lastPlan.ied.ext_refs_size(), 1);
  EXPECT_EQ(stack->lastPlan.ied.ext_refs(0).int_addr(), "other-trip-in");
}

// 验证：IED内部AccessPoint存在但Communication缺少匹配ConnectedAP时拒绝启动通信功能。
TEST(IEC61850ManagerTest, RejectsStartWithoutMatchingConnectedAp) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  manager.SetDataCenterStub(MakeStub(&state));
  ImportModel(&manager);
  auto request = MakeIedRequest();
  request.mutable_config()->set_access_point("AP3");
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(request, &info).ok());

  const auto status = manager.StartIed("line-1");

  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_NE(status.error_message().find("ConnectedAP"), std::string::npos);
  EXPECT_EQ(stack->startCount, 0);
  ASSERT_TRUE(manager.GetIed("line-1", &info).ok());
  EXPECT_EQ(info.state(), IEC61850Proto::IED_STATE_STOPPED);
}

// 验证：启用MMS但尚未收到连接快照时保持启动中，A/B完整快照原子驱动切换、降级和重连尝试统计。
TEST(IEC61850ManagerTest, TracksAtomicMmsConnectionSnapshotsAndReconnects) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  stack->emitConnectedOnStart = false;
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  manager.SetDataCenterStub(MakeStub(&state));
  ImportModel(&manager);
  auto request = MakeMmsIedRequest();
  auto* channelB = request.mutable_config()->add_channels();
  channelB->set_channel(IEC61850Proto::NETWORK_CHANNEL_B);
  channelB->set_enabled(true);
  channelB->set_interface_name("eth1");
  channelB->set_subnetwork_name("station-b");
  channelB->set_remote_ip("192.0.2.11");
  channelB->set_remote_port(102);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(request, &info).ok());
  ASSERT_TRUE(manager.StartIed("line-1").ok());

  ASSERT_TRUE(manager.GetIed("line-1", &info).ok());
  EXPECT_EQ(info.state(), IEC61850Proto::IED_STATE_STARTING);
  EXPECT_EQ(info.active_channel(),
            IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED);
  ASSERT_TRUE(manager.StartIed("line-1").ok());
  EXPECT_EQ(stack->startCount, 1);

  auto transportConnected = MakeMmsConnectionSnapshot(
      IEC61850::ProtocolSessionState::CONNECTED,
      IEC61850Proto::NETWORK_CHANNEL_A);
  AddMmsChannelStatus(&transportConnected,
                      IEC61850Proto::NETWORK_CHANNEL_A,
                      IEC61850Proto::CHANNEL_STATE_CONNECTED);
  AddMmsChannelStatus(&transportConnected,
                      IEC61850Proto::NETWORK_CHANNEL_B,
                      IEC61850Proto::CHANNEL_STATE_CONNECTED);
  stack->EmitMmsConnectionEvent(std::move(transportConnected));

  ASSERT_TRUE(manager.GetIed("line-1", &info).ok());
  EXPECT_EQ(info.state(), IEC61850Proto::IED_STATE_STARTING);
  EXPECT_EQ(info.active_channel(), IEC61850Proto::NETWORK_CHANNEL_A);

  auto readyA = MakeMmsConnectionSnapshot(
      IEC61850::ProtocolSessionState::READY,
      IEC61850Proto::NETWORK_CHANNEL_A);
  AddMmsChannelStatus(&readyA, IEC61850Proto::NETWORK_CHANNEL_A,
                      IEC61850Proto::CHANNEL_STATE_CONNECTED);
  AddMmsChannelStatus(&readyA, IEC61850Proto::NETWORK_CHANNEL_B,
                      IEC61850Proto::CHANNEL_STATE_CONNECTED);
  stack->EmitMmsConnectionEvent(std::move(readyA));

  ASSERT_TRUE(manager.GetIed("line-1", &info).ok());
  EXPECT_EQ(info.state(), IEC61850Proto::IED_STATE_RUNNING);
  EXPECT_EQ(info.active_channel(), IEC61850Proto::NETWORK_CHANNEL_A);
  ASSERT_EQ(info.channels_size(), 2);
  EXPECT_EQ(info.channels(0).state(),
            IEC61850Proto::CHANNEL_STATE_CONNECTED);
  EXPECT_EQ(info.channels(1).state(),
            IEC61850Proto::CHANNEL_STATE_CONNECTED);

  IEC61850::MmsConnectionEvent reconnectAttempt;
  reconnectAttempt.type =
      IEC61850::MmsConnectionEventType::RECONNECT_ATTEMPT;
  reconnectAttempt.reconnectChannel = IEC61850Proto::NETWORK_CHANNEL_B;
  stack->EmitMmsConnectionEvent(std::move(reconnectAttempt));

  auto switchedToB = MakeMmsConnectionSnapshot(
      IEC61850::ProtocolSessionState::READY,
      IEC61850Proto::NETWORK_CHANNEL_B);
  AddMmsChannelStatus(&switchedToB, IEC61850Proto::NETWORK_CHANNEL_A,
                      IEC61850Proto::CHANNEL_STATE_DISCONNECTED,
                      "A网连接中断");
  AddMmsChannelStatus(&switchedToB, IEC61850Proto::NETWORK_CHANNEL_B,
                      IEC61850Proto::CHANNEL_STATE_CONNECTED);
  stack->EmitMmsConnectionEvent(std::move(switchedToB));

  ASSERT_TRUE(manager.GetIed("line-1", &info).ok());
  EXPECT_EQ(info.state(), IEC61850Proto::IED_STATE_RUNNING);
  EXPECT_EQ(info.active_channel(), IEC61850Proto::NETWORK_CHANNEL_B);
  EXPECT_EQ(info.channels(0).state(),
            IEC61850Proto::CHANNEL_STATE_DISCONNECTED);
  EXPECT_EQ(info.channels(0).last_error(), "A网连接中断");
  EXPECT_EQ(info.channels(1).state(),
            IEC61850Proto::CHANNEL_STATE_CONNECTED);

  auto disconnected = MakeMmsConnectionSnapshot(
      IEC61850::ProtocolSessionState::DEGRADED);
  disconnected.error = "A/B网均断开";
  AddMmsChannelStatus(&disconnected, IEC61850Proto::NETWORK_CHANNEL_A,
                      IEC61850Proto::CHANNEL_STATE_DISCONNECTED,
                      "A网连接中断");
  AddMmsChannelStatus(&disconnected, IEC61850Proto::NETWORK_CHANNEL_B,
                      IEC61850Proto::CHANNEL_STATE_DISCONNECTED,
                      "B网连接中断");
  stack->EmitMmsConnectionEvent(std::move(disconnected));

  ASSERT_TRUE(manager.GetIed("line-1", &info).ok());
  EXPECT_EQ(info.state(), IEC61850Proto::IED_STATE_DEGRADED);
  EXPECT_EQ(info.active_channel(),
            IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED);
  ASSERT_TRUE(manager.StartIed("line-1").ok());
  EXPECT_EQ(stack->startCount, 1);
  IEC61850Proto::RuntimeStatistics statistics;
  ASSERT_TRUE(manager.GetRuntimeStatistics("line-1", &statistics).ok());
  EXPECT_EQ(statistics.reconnect_count(), 1u);
}

// 验证：未配置活动通道的矛盾MMS快照被拒绝且不污染最后一次有效连接状态。
TEST(IEC61850ManagerTest, RejectsContradictoryMmsConnectionSnapshot) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  manager.SetDataCenterStub(MakeStub(&state));
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(MakeMmsIedRequest(), &info).ok());
  ASSERT_TRUE(manager.StartIed("line-1").ok());

  auto invalid = MakeMmsConnectionSnapshot(
      IEC61850::ProtocolSessionState::READY,
      IEC61850Proto::NETWORK_CHANNEL_B);
  AddMmsChannelStatus(&invalid, IEC61850Proto::NETWORK_CHANNEL_A,
                      IEC61850Proto::CHANNEL_STATE_CONNECTED);
  stack->EmitMmsConnectionEvent(std::move(invalid));

  ASSERT_TRUE(manager.GetIed("line-1", &info).ok());
  EXPECT_EQ(info.state(), IEC61850Proto::IED_STATE_RUNNING);
  EXPECT_EQ(info.active_channel(), IEC61850Proto::NETWORK_CHANNEL_A);
  EXPECT_EQ(info.channels(0).state(),
            IEC61850Proto::CHANNEL_STATE_CONNECTED);
}

// 验证：停止并重启IED通信功能后，旧会话迟到的MMS连接错误快照不能污染新会话状态。
TEST(IEC61850ManagerTest, RejectsStaleMmsConnectionSnapshotAfterRestart) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  manager.SetDataCenterStub(MakeStub(&state));
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(MakeMmsIedRequest(), &info).ok());
  ASSERT_TRUE(manager.StartIed("line-1").ok());
  const auto staleCallbacks = stack->CopyCallbacks();
  ASSERT_TRUE(manager.StopIed("line-1").ok());
  ASSERT_TRUE(manager.StartIed("line-1").ok());

  auto staleError = MakeMmsConnectionSnapshot(
      IEC61850::ProtocolSessionState::ERROR);
  staleError.state = IEC61850::ProtocolSessionState::ERROR;
  staleError.error = "旧会话错误";
  AddMmsChannelStatus(&staleError, IEC61850Proto::NETWORK_CHANNEL_A,
                      IEC61850Proto::CHANNEL_STATE_ERROR, "旧会话错误");
  FakeProtocolStack::EmitMmsConnectionEventWith(staleCallbacks,
                                                 std::move(staleError));

  ASSERT_TRUE(manager.GetIed("line-1", &info).ok());
  EXPECT_EQ(info.state(), IEC61850Proto::IED_STATE_RUNNING);
  EXPECT_EQ(info.active_channel(), IEC61850Proto::NETWORK_CHANNEL_A);
  EXPECT_TRUE(info.last_error().empty());
}

// 验证：Manager析构并停止IED通信功能后，协议栈错误保留的旧MMS连接回调不会访问已销毁对象。
TEST(IEC61850ManagerTest,
     IgnoresStaleMmsConnectionCallbackAfterManagerDestruction) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  IEC61850::ProtocolEventCallbacks staleCallbacks;
  {
    IEC61850::Manager manager(directory.database(), stack);
    FakeDataCenterState state;
    manager.SetDataCenterStub(MakeStub(&state));
    ImportModel(&manager);
    IEC61850Proto::IedInfo info;
    ASSERT_TRUE(manager.UpsertIed(MakeMmsIedRequest(), &info).ok());
    ASSERT_TRUE(manager.StartIed("line-1").ok());
    staleCallbacks = stack->CopyCallbacks();
  }

  auto staleEvent = MakeMmsConnectionSnapshot(
      IEC61850::ProtocolSessionState::READY,
      IEC61850Proto::NETWORK_CHANNEL_A);
  AddMmsChannelStatus(&staleEvent, IEC61850Proto::NETWORK_CHANNEL_A,
                      IEC61850Proto::CHANNEL_STATE_CONNECTED);
  FakeProtocolStack::EmitMmsConnectionEventWith(staleCallbacks,
                                                 std::move(staleEvent));
  SUCCEED();
}

// 验证：协议栈停止失败时隔离旧MMS报告入口，并保留最后连接快照供诊断和重试停止。
TEST(IEC61850ManagerTest, StopFailurePreservesLastMmsConnectionSnapshot) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  manager.SetDataCenterStub(MakeStub(&state));
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(MakeMmsIedRequest(), &info).ok());
  auto mappingRequest = MakeTripMappingRequest();
  mappingRequest.mutable_points(0)->set_source(
      IEC61850Proto::POINT_SOURCE_MMS);
  ASSERT_TRUE(manager.UpsertPointMappings(mappingRequest).ok());
  ASSERT_TRUE(manager.StartIed("line-1").ok());
  const auto staleCallbacks = stack->CopyCallbacks();
  stack->stopStatus =
      grpc::Status(grpc::StatusCode::INTERNAL, "协议栈停止失败");

  EXPECT_FALSE(manager.StopIed("line-1").ok());
  FakeProtocolStack::EmitMmsReportWith(staleCallbacks,
                                       MakeTripReport(true, 900));

  ASSERT_TRUE(manager.GetIed("line-1", &info).ok());
  EXPECT_EQ(info.state(), IEC61850Proto::IED_STATE_ERROR);
  EXPECT_EQ(info.active_channel(), IEC61850Proto::NETWORK_CHANNEL_A);
  ASSERT_EQ(info.channels_size(), 1);
  EXPECT_EQ(info.channels(0).state(),
            IEC61850Proto::CHANNEL_STATE_CONNECTED);
  IEC61850Proto::RuntimeStatistics statistics;
  ASSERT_TRUE(manager.GetRuntimeStatistics("line-1", &statistics).ok());
  EXPECT_EQ(statistics.mms_reports_received(), 0u);
  EXPECT_EQ(state.GetPublishCount(info.conn_id(), "TRIP"), 0u);
  EXPECT_EQ(manager.StartIed("line-1").error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(stack->startCount, 1);
  auto replacement = MakeMmsIedRequest();
  EXPECT_EQ(manager.UpsertIed(replacement, &info).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(manager.UpsertPointMappings(mappingRequest).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);

  stack->stopStatus = grpc::Status::OK;
  EXPECT_TRUE(manager.StopIed("line-1").ok());
  EXPECT_EQ(stack->stopCount, 2);
  ASSERT_TRUE(manager.GetIed("line-1", &info).ok());
  EXPECT_EQ(info.state(), IEC61850Proto::IED_STATE_STOPPED);
  EXPECT_EQ(info.active_channel(),
            IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED);
  EXPECT_TRUE(info.channels().empty());
}

// 验证：协议栈启动接口抛异常时转换为内部错误，并清理已建立的MMS入口。
TEST(IEC61850ManagerTest, ConvertsProtocolStackStartExceptionToStatus) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  stack->throwOnStart = true;
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  manager.SetDataCenterStub(MakeStub(&state));
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(MakeMmsIedRequest(), &info).ok());

  const auto status = manager.StartIed("line-1");

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_NE(status.error_message().find("协议栈启动接口发生异常"),
            std::string::npos);
  ASSERT_TRUE(manager.GetIed("line-1", &info).ok());
  EXPECT_EQ(info.state(), IEC61850Proto::IED_STATE_ERROR);
  EXPECT_EQ(stack->startCount, 1);
  stack->throwOnStart = false;
  ASSERT_TRUE(manager.StartIed("line-1").ok());
  EXPECT_EQ(stack->startCount, 2);
}

// 验证：协议栈停止接口抛异常时保留会话活动标志，修复适配器后可以重试停止。
TEST(IEC61850ManagerTest, ConvertsProtocolStackStopExceptionAndAllowsRetry) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  manager.SetDataCenterStub(MakeStub(&state));
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(MakeIedRequest(), &info).ok());
  ASSERT_TRUE(manager.StartIed("line-1").ok());
  stack->throwOnStop = true;

  const auto firstStop = manager.StopIed("line-1");

  EXPECT_EQ(firstStop.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_NE(firstStop.error_message().find("协议栈停止接口发生异常"),
            std::string::npos);
  ASSERT_TRUE(manager.GetIed("line-1", &info).ok());
  EXPECT_EQ(info.state(), IEC61850Proto::IED_STATE_ERROR);
  EXPECT_EQ(manager.StartIed("line-1").error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);

  stack->throwOnStop = false;
  ASSERT_TRUE(manager.StopIed("line-1").ok());
  EXPECT_EQ(stack->stopCount, 2);
  ASSERT_TRUE(manager.GetIed("line-1", &info).ok());
  EXPECT_EQ(info.state(), IEC61850Proto::IED_STATE_STOPPED);
}

// 验证：协议栈已停止但MMS在途发布未收敛时，重复停止仍返回超时，收敛后才能停止成功。
TEST(IEC61850ManagerTest, RetriesMmsDeactivationAfterProtocolStackStopped) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  auto stub = std::make_shared<DataCenterProto::MockDataCenterServiceStub>();
  ON_CALL(*stub, GetOrCreateConnection(testing::_, testing::_, testing::_))
      .WillByDefault(testing::Invoke(
          [](grpc::ClientContext*,
             const DataCenterProto::GetOrCreateConnectionRequest& request,
             DataCenterProto::ConnectionInfo* response) {
            response->set_conn_id(42);
            response->set_module_name(request.key().module_name());
            response->set_conn_name(request.key().conn_name());
            return grpc::Status::OK;
          }));
  ON_CALL(*stub, UpsertConnTags(testing::_, testing::_, testing::_))
      .WillByDefault(testing::Return(grpc::Status::OK));
  std::mutex publishMutex;
  std::condition_variable publishCondition;
  bool publishEntered = false;
  bool releasePublish = false;
  struct PublishReleaseGuard {
    std::mutex& mutex;
    std::condition_variable& condition;
    bool& release;

    ~PublishReleaseGuard() { Release(); }

    void Release() {
      std::lock_guard lock(mutex);
      release = true;
      condition.notify_all();
    }
  } releaseGuard{publishMutex, publishCondition, releasePublish};
  EXPECT_CALL(*stub, BatchPublish(testing::_, testing::_, testing::_))
      .WillOnce(testing::Invoke(
          [&](grpc::ClientContext*,
              const DataCenterProto::BatchPublishRequest&,
              DataCenterProto::Empty*) {
            std::unique_lock lock(publishMutex);
            publishEntered = true;
            publishCondition.notify_all();
            publishCondition.wait(lock, [&]() { return releasePublish; });
            return grpc::Status::OK;
          }));
  IEC61850::Manager manager(directory.database(), stack);
  manager.SetDataCenterStub(stub);
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(MakeMmsIedRequest(), &info).ok());
  auto mappingRequest = MakeTripMappingRequest();
  mappingRequest.mutable_points(0)->set_source(
      IEC61850Proto::POINT_SOURCE_MMS);
  ASSERT_TRUE(manager.UpsertPointMappings(mappingRequest).ok());
  ASSERT_TRUE(manager.StartIed("line-1").ok());
  stack->EmitMmsReport(MakeTripReport(true, 900));
  {
    std::unique_lock lock(publishMutex);
    EXPECT_TRUE(publishCondition.wait_for(
        lock, std::chrono::seconds(2), [&]() { return publishEntered; }));
  }

  const auto firstStop = manager.StopIed("line-1");
  EXPECT_EQ(firstStop.error_code(), grpc::StatusCode::DEADLINE_EXCEEDED);
  EXPECT_EQ(stack->stopCount, 1);
  ASSERT_TRUE(manager.GetIed("line-1", &info).ok());
  EXPECT_EQ(info.state(), IEC61850Proto::IED_STATE_ERROR);
  EXPECT_EQ(info.active_channel(),
            IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED);

  const auto secondStop = manager.StopIed("line-1");
  EXPECT_EQ(secondStop.error_code(), grpc::StatusCode::DEADLINE_EXCEEDED);
  EXPECT_EQ(stack->stopCount, 1);
  releaseGuard.Release();

  const auto finalStop = manager.StopIed("line-1");
  EXPECT_TRUE(finalStop.ok()) << finalStop.error_message();
  EXPECT_EQ(stack->stopCount, 1);
  ASSERT_TRUE(manager.GetIed("line-1", &info).ok());
  EXPECT_EQ(info.state(), IEC61850Proto::IED_STATE_STOPPED);
}

// 验证：协议栈注入的MMS报告经Manager注册的异步管线批量发布到同一IED的DataCenter连接。
TEST(IEC61850ManagerTest, PublishesInjectedMmsReportToDataCenter) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  manager.SetDataCenterStub(MakeStub(&state));
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(MakeMmsIedRequest(), &info).ok());
  auto mappingRequest = MakeTripMappingRequest();
  mappingRequest.mutable_points(0)->set_source(
      IEC61850Proto::POINT_SOURCE_MMS);
  ASSERT_TRUE(manager.UpsertPointMappings(mappingRequest).ok());
  ASSERT_TRUE(manager.StartIed("line-1").ok());
  ASSERT_TRUE(manager.GetIed("line-1", &info).ok());
  ASSERT_NE(info.conn_id(), 0u);

  stack->EmitMmsReport(MakeTripReport(true, 900));

  ASSERT_TRUE(state.WaitForPublishCount(
      info.conn_id(), "TRIP", 1, std::chrono::seconds(2)));
  DataCenterProto::GetLatestRequest latestRequest;
  latestRequest.set_conn_id(info.conn_id());
  latestRequest.add_tags("TRIP");
  DataCenterProto::GetLatestResponse latest;
  ASSERT_TRUE(state.GetLatest(latestRequest, &latest).ok());
  ASSERT_EQ(latest.updates_size(), 1);
  EXPECT_TRUE(latest.updates(0).value().bool_value());
  EXPECT_EQ(latest.updates(0).ts_ms(), 900);
  EXPECT_EQ(latest.updates(0).quality(), DataCenterProto::QUALITY_GOOD);
  auto oversizedReport = MakeTripReport(true, 901);
  oversizedReport.values.front().value = std::string(
      mskdsp::kIec61850MaxMmsVariableValueBytes + 1, 'x');
  stack->EmitMmsReport(std::move(oversizedReport));
  IEC61850Proto::RuntimeStatistics statistics;
  ASSERT_TRUE(manager.GetRuntimeStatistics("line-1", &statistics).ok());
  EXPECT_EQ(statistics.mms_reports_received(), 2u);
  EXPECT_EQ(statistics.data_center_batches_published(), 1u);
  EXPECT_EQ(statistics.mms_values_oversized(), 1u);
  EXPECT_EQ(statistics.mms_reports_oversized(), 1u);
  EXPECT_GT(statistics.mms_queue_bytes_high_watermark(), 0u);
}

// 验证：停止后保留的旧MMS回调不会在同名IED重启后进入新会话。
TEST(IEC61850ManagerTest, RejectsStaleMmsCallbackAfterRestart) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  manager.SetDataCenterStub(MakeStub(&state));
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(MakeMmsIedRequest(), &info).ok());
  auto mappingRequest = MakeTripMappingRequest();
  mappingRequest.mutable_points(0)->set_source(
      IEC61850Proto::POINT_SOURCE_MMS);
  ASSERT_TRUE(manager.UpsertPointMappings(mappingRequest).ok());
  ASSERT_TRUE(manager.StartIed("line-1").ok());
  const auto staleCallbacks = stack->CopyCallbacks();
  ASSERT_TRUE(manager.StopIed("line-1").ok());
  ASSERT_TRUE(manager.StartIed("line-1").ok());
  ASSERT_TRUE(manager.GetIed("line-1", &info).ok());

  FakeProtocolStack::EmitMmsReportWith(
      staleCallbacks, MakeTripReport(false, 1000));
  stack->EmitMmsReport(MakeTripReport(true, 1001));

  IEC61850Proto::RuntimeStatistics statistics;
  ASSERT_TRUE(manager.GetRuntimeStatistics("line-1", &statistics).ok());
  EXPECT_EQ(statistics.mms_reports_received(), 1u);
  ASSERT_TRUE(state.WaitForPublishCount(
      info.conn_id(), "TRIP", 1, std::chrono::seconds(2)));
  EXPECT_EQ(state.GetPublishCount(info.conn_id(), "TRIP"), 1u);
}

// 验证：Start阻塞期间发起Stop时，两项操作串行进入协议栈且最终状态为STOPPED。
TEST(IEC61850ManagerTest, ConcurrentStartThenStopEndsStopped) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<BlockingProtocolStack>();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  manager.SetDataCenterStub(MakeStub(&state));
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(MakeIedRequest(), &info).ok());

  grpc::Status startStatus;
  grpc::Status stopStatus;
  std::jthread startThread([&]() { startStatus = manager.StartIed("line-1"); });
  const bool startEntered = stack->WaitUntilStartEntered();
  if (!startEntered) {
    stack->ReleaseStart();
  }
  ASSERT_TRUE(startEntered);
  std::promise<void> stopAttempted;
  auto stopAttemptedFuture = stopAttempted.get_future();
  std::jthread stopThread([&]() {
    stopAttempted.set_value();
    stopStatus = manager.StopIed("line-1");
  });
  stopAttemptedFuture.wait();

  EXPECT_EQ(stack->stopCount(), 0);
  stack->ReleaseStart();
  startThread.join();
  stopThread.join();

  ASSERT_TRUE(startStatus.ok()) << startStatus.error_message();
  ASSERT_TRUE(stopStatus.ok()) << stopStatus.error_message();
  EXPECT_EQ(stack->startCount(), 1);
  EXPECT_EQ(stack->stopCount(), 1);
  ASSERT_TRUE(manager.GetIed("line-1", &info).ok());
  EXPECT_EQ(info.state(), IEC61850Proto::IED_STATE_STOPPED);
}

// 验证：两个并发Start串行收敛，协议栈只启动一次IED通信功能。
TEST(IEC61850ManagerTest, ConcurrentStartsInvokeProtocolStackOnce) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<BlockingProtocolStack>();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  manager.SetDataCenterStub(MakeStub(&state));
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(MakeIedRequest(), &info).ok());

  grpc::Status firstStatus;
  grpc::Status secondStatus;
  std::jthread first([&]() { firstStatus = manager.StartIed("line-1"); });
  const bool startEntered = stack->WaitUntilStartEntered();
  if (!startEntered) {
    stack->ReleaseStart();
  }
  ASSERT_TRUE(startEntered);
  std::promise<void> secondAttempted;
  auto secondAttemptedFuture = secondAttempted.get_future();
  std::jthread second([&]() {
    secondAttempted.set_value();
    secondStatus = manager.StartIed("line-1");
  });
  secondAttemptedFuture.wait();

  EXPECT_EQ(stack->startCount(), 1);
  stack->ReleaseStart();
  first.join();
  second.join();

  ASSERT_TRUE(firstStatus.ok()) << firstStatus.error_message();
  ASSERT_TRUE(secondStatus.ok()) << secondStatus.error_message();
  EXPECT_EQ(stack->startCount(), 1);
}

// 验证：模块关闭先取得生命周期串行权时，排队中的Start不能在关闭后重新启动IED通信功能。
TEST(IEC61850ManagerTest, ShutdownRejectsQueuedStart) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<BlockingProtocolStack>();
  stack->ReleaseStart();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  manager.SetDataCenterStub(MakeStub(&state));
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(MakeIedRequest(), &info).ok());
  ASSERT_TRUE(manager.StartIed("line-1").ok());
  stack->BlockStop();

  std::jthread shutdownThread([&]() { manager.Shutdown(); });
  const bool stopEntered = stack->WaitUntilStopEntered();
  if (!stopEntered) {
    stack->ReleaseStop();
  }
  ASSERT_TRUE(stopEntered);
  grpc::Status queuedStartStatus;
  std::promise<void> startAttempted;
  auto startAttemptedFuture = startAttempted.get_future();
  std::jthread queuedStart([&]() {
    startAttempted.set_value();
    queuedStartStatus = manager.StartIed("line-1");
  });
  startAttemptedFuture.wait();

  stack->ReleaseStop();
  shutdownThread.join();
  queuedStart.join();

  EXPECT_EQ(queuedStartStatus.error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(stack->startCount(), 1);
  EXPECT_EQ(stack->stopCount(), 1);
  ASSERT_TRUE(manager.GetIed("line-1", &info).ok());
  EXPECT_EQ(info.state(), IEC61850Proto::IED_STATE_STOPPED);
}

// 验证：完整目标态可收敛运行中的IED，并在提交后按目标运行状态重新启动通信功能。
TEST(IEC61850ManagerTest, ApplyTargetConfigRestartsRunningIed) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  manager.SetDataCenterStub(MakeStub(&state));
  IEC61850Proto::ApplyTargetConfigResponse response;
  auto request = MakeTargetRequest(true);
  ASSERT_TRUE(manager.ApplyTargetConfig(request, &response).ok());
  ASSERT_EQ(stack->startCount, 1);

  response.Clear();
  const auto status = manager.ApplyTargetConfig(request, &response);

  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(stack->stopCount, 1);
  EXPECT_EQ(stack->startCount, 2);
  ASSERT_EQ(response.ieds_size(), 1);
  EXPECT_EQ(response.ieds(0).state(), IEC61850Proto::IED_STATE_RUNNING);
}

// 验证：删除运行中的IED时由Manager先停止通信功能，再删除本地配置和DataCenter连接。
TEST(IEC61850ManagerTest, DeleteRunningIedStopsCommunicationFirst) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  manager.SetDataCenterStub(stub);
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(MakeIedRequest(), &info).ok());
  ASSERT_TRUE(manager.StartIed("line-1").ok());

  const auto status = manager.DeleteIed("line-1");

  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(stack->stopCount, 1);
  EXPECT_EQ(stack->lastStopped, "line-1");
  EXPECT_FALSE(state.HasConnection("IEC61850", "line-1"));
  EXPECT_EQ(manager.GetIed("line-1", &info).error_code(),
            grpc::StatusCode::NOT_FOUND);
}

// 验证：模块重启后恢复desired_running的IED，模块关闭不清除该目标状态。
TEST(IEC61850ManagerTest, RestoresDesiredIedAndShutdownPreservesTargetState) {
  TemporaryDirectory directory;
  auto firstStack = std::make_shared<FakeProtocolStack>();
  IEC61850::Manager first(directory.database(), firstStack);
  FakeDataCenterState state;
  first.SetDataCenterStub(MakeStub(&state));
  ImportModel(&first);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(first.UpsertIed(MakeIedRequest(), &info).ok());
  ASSERT_TRUE(first.StartIed("line-1").ok());
  first.Shutdown();
  EXPECT_EQ(firstStack->stopCount, 1);

  auto restoredStack = std::make_shared<FakeProtocolStack>();
  IEC61850::Manager restored(directory.database(), restoredStack);
  restored.SetDataCenterStub(MakeStub(&state));
  ASSERT_TRUE(restored.LoadPersistedConfig().ok());
  restored.RestoreConfiguredIeds();

  EXPECT_EQ(restoredStack->startCount, 1);
  EXPECT_EQ(restoredStack->lastStarted, "line-1");
  ASSERT_TRUE(restored.GetIed("line-1", &info).ok());
  EXPECT_EQ(info.state(), IEC61850Proto::IED_STATE_RUNNING);
}

// 验证：模型仍被IED配置引用时不能删除。
TEST(IEC61850ManagerTest, DoesNotDeleteReferencedModel) {
  TemporaryDirectory directory;
  IEC61850::Manager manager(directory.database());
  FakeDataCenterState state;
  manager.SetDataCenterStub(MakeStub(&state));
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(MakeIedRequest(), &info).ok());

  const auto status = manager.DeleteModel("station-model");

  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证：DataCenter删除失败形成待删除墓碑后，该IED不能再次启动通信功能。
TEST(IEC61850ManagerTest, PendingDeleteIedCannotRestart) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  state.FailDeleteForConnName("line-1");
  manager.SetDataCenterStub(MakeStub(&state));
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(MakeIedRequest(), &info).ok());

  EXPECT_FALSE(manager.DeleteIed("line-1").ok());
  ASSERT_TRUE(manager.GetIed("line-1", &info).ok());
  EXPECT_EQ(info.state(), IEC61850Proto::IED_STATE_PENDING_DELETE);
  EXPECT_EQ(manager.StartIed("line-1").error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(stack->startCount, 0);
}

// 验证：删除墓碑跨模块重启保留，DataCenter恢复后对账自动删除本地IED、点映射和外部连接。
TEST(IEC61850ManagerTest, ReconcileRetriesPersistedPendingDelete) {
  TemporaryDirectory directory;
  FakeDataCenterState state;
  auto stack = std::make_shared<FakeProtocolStack>();
  {
    IEC61850::Manager manager(directory.database(), stack);
    manager.SetDataCenterStub(MakeStub(&state));
    ImportModel(&manager);
    IEC61850Proto::IedInfo info;
    ASSERT_TRUE(manager.UpsertIed(MakeIedRequest(), &info).ok());
    ASSERT_TRUE(manager.UpsertPointMappings(MakeTripMappingRequest()).ok());
    state.FailDeleteForConnName("line-1");
    EXPECT_FALSE(manager.DeleteIed("line-1").ok());
  }

  IEC61850::Manager restored(directory.database(), stack);
  restored.SetDataCenterStub(MakeStub(&state));
  ASSERT_TRUE(restored.LoadPersistedConfig().ok());
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(restored.GetIed("line-1", &info).ok());
  EXPECT_EQ(info.state(), IEC61850Proto::IED_STATE_PENDING_DELETE);
  restored.RestoreConfiguredIeds();
  EXPECT_EQ(stack->startCount, 0);

  state.AllowDeleteForConnName("line-1");
  restored.ReconcileDataCenter();

  EXPECT_EQ(restored.GetIed("line-1", &info).error_code(),
            grpc::StatusCode::NOT_FOUND);
  IEC61850Proto::PointMappings mappings;
  EXPECT_EQ(restored.GetPointMappings("line-1", &mappings).error_code(),
            grpc::StatusCode::NOT_FOUND);
  EXPECT_FALSE(state.HasConnection("IEC61850", "line-1"));

  IEC61850::Manager reloaded(directory.database(), stack);
  ASSERT_TRUE(reloaded.LoadPersistedConfig().ok());
  EXPECT_EQ(reloaded.GetIed("line-1", &info).error_code(),
            grpc::StatusCode::NOT_FOUND);
}

// 验证：重试待删除IED时不重复停止已经停止的通信功能。
TEST(IEC61850ManagerTest, RetryingPendingDeleteDoesNotStopAgain) {
  TemporaryDirectory directory;
  auto stack = std::make_shared<FakeProtocolStack>();
  IEC61850::Manager manager(directory.database(), stack);
  FakeDataCenterState state;
  manager.SetDataCenterStub(MakeStub(&state));
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(MakeIedRequest(), &info).ok());
  ASSERT_TRUE(manager.StartIed("line-1").ok());
  state.FailDeleteForConnName("line-1");
  EXPECT_FALSE(manager.DeleteIed("line-1").ok());
  ASSERT_EQ(stack->stopCount, 1);

  state.AllowDeleteForConnName("line-1");
  ASSERT_TRUE(manager.DeleteIed("line-1").ok());

  EXPECT_EQ(stack->stopCount, 1);
  EXPECT_EQ(manager.GetIed("line-1", &info).error_code(),
            grpc::StatusCode::NOT_FOUND);
}

// 验证：完整目标态移除旧IED时，失败的DataCenter连接删除会跨重启继续对账。
TEST(IEC61850ManagerTest, TargetConfigPersistsStaleConnectionCleanup) {
  TemporaryDirectory directory;
  FakeDataCenterState state;
  {
    IEC61850::Manager manager(directory.database());
    manager.SetDataCenterStub(MakeStub(&state));
    IEC61850Proto::ApplyTargetConfigResponse response;
    ASSERT_TRUE(manager.ApplyTargetConfig(MakeTargetRequest(false), &response).ok());
    ASSERT_TRUE(state.HasConnection("IEC61850", "line-1"));
    state.FailDeleteForConnName("line-1");
    IEC61850Proto::ApplyTargetConfigRequest emptyTarget;
    response.Clear();
    ASSERT_TRUE(manager.ApplyTargetConfig(emptyTarget, &response).ok());
    EXPECT_TRUE(state.HasConnection("IEC61850", "line-1"));
  }

  state.AllowDeleteForConnName("line-1");
  IEC61850::Manager restored(directory.database());
  restored.SetDataCenterStub(MakeStub(&state));
  ASSERT_TRUE(restored.LoadPersistedConfig().ok());
  restored.ReconcileDataCenter();

  EXPECT_FALSE(state.HasConnection("IEC61850", "line-1"));
}

// 验证：点映射已本地提交后，DataCenter标签同步失败只报告降级，不把已生效配置返回为失败。
TEST(IEC61850ManagerTest, PointMappingSyncFailureKeepsLocalSuccess) {
  TemporaryDirectory directory;
  IEC61850::Manager manager(directory.database());
  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  manager.SetDataCenterStub(stub);
  ImportModel(&manager);
  IEC61850Proto::IedInfo info;
  ASSERT_TRUE(manager.UpsertIed(MakeIedRequest(), &info).ok());
  EXPECT_CALL(*stub, UpsertConnTags(testing::_, testing::_, testing::_))
      .WillOnce(testing::Return(
          grpc::Status(grpc::StatusCode::UNAVAILABLE,
                       "DataCenter标签服务不可用")));
  IEC61850Proto::UpsertPointMappingsRequest request;
  request.set_conn_name("line-1");
  request.set_replace(true);
  auto* point = request.add_points();
  point->set_tag("TRIP");
  point->set_data_ref("IED1LD0/PTRC1.Tr.general");
  point->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST);
  point->set_source(IEC61850Proto::POINT_SOURCE_GOOSE);
  point->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_BOOL);

  const auto status = manager.UpsertPointMappings(request);

  EXPECT_TRUE(status.ok()) << status.error_message();
  IEC61850Proto::PointMappings mappings;
  ASSERT_TRUE(manager.GetPointMappings("line-1", &mappings).ok());
  ASSERT_EQ(mappings.points_size(), 1);
  EXPECT_EQ(mappings.points(0).tag(), "TRIP");
  ASSERT_TRUE(manager.GetIed("line-1", &info).ok());
  EXPECT_FALSE(info.data_center_available());
  EXPECT_NE(info.last_error().find("DataCenter标签同步失败"),
            std::string::npos);
}

}  // namespace
