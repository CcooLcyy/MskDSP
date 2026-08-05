#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <grpcpp/support/status.h>

#include "IEC61850.pb.h"
#include "IEC61850MmsControl.h"
#include "IEC61850MmsSettingGroup.h"
#include "IEC61850MmsService.h"
#include "IEC61850ThreadRuntimePolicy.h"

namespace IEC61850 {

enum class MmsValidity {
  GOOD,
  INVALID,
  RESERVED,
  QUESTIONABLE,
};

struct MmsQuality {
  MmsValidity validity = MmsValidity::GOOD;
  bool overflow = false;
  bool outOfRange = false;
  bool badReference = false;
  bool oscillatory = false;
  bool failure = false;
  bool oldData = false;
  bool inconsistent = false;
  bool inaccurate = false;
  bool sourceSubstituted = false;
  bool test = false;
  bool operatorBlocked = false;

  bool operator==(const MmsQuality&) const = default;
};

struct MmsCompositeValue;

// MMS数组或结构值；elements保留原始成员顺序，encodedContent保存其TLV内容
// 便于后续需要原始表示的适配层使用。树深度和单值大小由报告解码器限制。
using MmsValue = std::variant<bool, int64_t, double, std::string,
                               std::vector<uint8_t>,
                               std::shared_ptr<MmsCompositeValue>>;

struct MmsCompositeValue {
  enum class Kind : std::uint8_t {
    ARRAY = 1,
    STRUCTURE = 2,
  };

  Kind kind = Kind::STRUCTURE;
  std::vector<MmsValue> elements;
  std::vector<std::uint8_t> encodedContent;
};

struct MmsDataValue {
  std::string dataRef;
  IEC61850Proto::FunctionalConstraint fc =
      IEC61850Proto::FUNCTIONAL_CONSTRAINT_UNSPECIFIED;
  MmsValue value;
  MmsQuality quality;
  int64_t timestampMs = 0;
  bool timestampValid = false;

  bool operator==(const MmsDataValue&) const = default;
};

struct MmsReportEvent {
  std::string reportRef;
  std::string dataSetRef;
  uint64_t confRev = 0;
  uint64_t sequenceNumber = 0;
  int64_t receiveTimestampMs = 0;
  // 报告中至少一个Reason字段声明GI；分段合并时按段做逻辑或。
  bool generalInterrogation = false;
  std::vector<MmsDataValue> values;
};

// 在线MMS目录中的数据属性。
struct MmsDirectoryDataAttribute {
  std::string dataRef;
  IEC61850Proto::FunctionalConstraint fc =
      IEC61850Proto::FUNCTIONAL_CONSTRAINT_UNSPECIFIED;
  // 在线GetVariableAccessAttributes返回的类型；旧测试/兼容快照可省略。
  std::optional<MmsTypeSpecification> typeSpecification;
  // 仅对在线读取的控制参数保存完整MMS Data选择；普通数据属性不读取值。
  std::vector<std::uint8_t> encodedValue;
};

// 在线MMS目录中的DataSet及其有序成员。
struct MmsDirectoryDataSet {
  std::string dataSetRef;
  std::vector<MmsDirectoryDataAttribute> members;
};

// 在线MMS目录中的ReportControl。
struct MmsDirectoryReportControl {
  std::string rcbRef;
  std::string dataSetRef;
  std::string reportId;
  bool reportEnabled = false;
  bool buffered = false;
  uint64_t configRevision = 0;
  uint32_t maxInstances = 0;
  uint32_t integrityPeriodMs = 0;
  uint32_t bufferTimeMs = 0;
  IEC61850Proto::SclTriggerOptions triggerOptions;
  IEC61850Proto::SclOptionalFields optionalFields;
};

// 适配器读取服务端目录后提交的最小核对快照。
struct MmsOnlineDirectory {
  std::string iedName;
  std::string accessPoint;
  std::vector<std::string> logicalNodeRefs;
  // 服务端Domain NameList中的有序变量对象，用于控制能力模型发现。
  std::vector<MmsObjectName> namedVariables;
  std::vector<MmsDirectoryDataAttribute> dataAttributes;
  std::vector<MmsDirectoryDataSet> dataSets;
  std::vector<MmsDirectoryReportControl> reportControls;
};

// 报告分段；只在最后一段到达后转换为MmsReportEvent。
struct MmsReportSegment {
  std::string reportRef;
  std::string dataSetRef;
  uint64_t confRev = 0;
  uint64_t sequenceNumber = 0;
  uint32_t segmentNumber = 0;
  bool moreSegmentsFollow = false;
  int64_t receiveTimestampMs = 0;
  // 报告中至少一个Reason字段声明GI；分段合并时按段做逻辑或。
  bool generalInterrogation = false;
  std::vector<MmsDataValue> values;
};

// 一次BRCB/URCB启用请求的期望参数。
struct MmsRcbActivationRequest {
  std::string rcbRef;
  std::string dataSetRef;
  std::string reportId;
  bool buffered = false;
  uint64_t configRevision = 0;
  uint32_t maxInstances = 0;
  uint32_t integrityPeriodMs = 0;
  uint32_t bufferTimeMs = 0;
  IEC61850Proto::SclTriggerOptions triggerOptions;
  IEC61850Proto::SclOptionalFields optionalFields;
  bool generalInterrogation = false;
};

// 协议栈观察到的IED会话连接状态。
enum class ProtocolSessionState {
  CONNECTING,
  CONNECTED,
  READY,
  DEGRADED,
  DISCONNECTED,
  ERROR,
};

// MMS连接事件类型；状态快照与重连尝试分开计量。
enum class MmsConnectionEventType {
  STATE_SNAPSHOT,
  RECONNECT_ATTEMPT,
};

// 单个MMS网络通道的完整状态。
struct MmsChannelStatus {
  IEC61850Proto::NetworkChannel channel =
      IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED;
  IEC61850Proto::ChannelState state =
      IEC61850Proto::CHANNEL_STATE_UNSPECIFIED;
  std::string error;
};

// 协议栈MMS连接事件；状态快照必须包含全部已启用A/B通道。
struct MmsConnectionEvent {
  MmsConnectionEventType type = MmsConnectionEventType::STATE_SNAPSHOT;
  ProtocolSessionState state = ProtocolSessionState::DISCONNECTED;
  IEC61850Proto::NetworkChannel activeChannel =
      IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED;
  IEC61850Proto::NetworkChannel reconnectChannel =
      IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED;
  std::vector<MmsChannelStatus> channels;
  int64_t timestampMs = 0;
  std::string error;
};

enum class ProtocolRealtimeValueType : std::uint8_t {
  BOOLEAN = 1,
  INTEGER = 2,
  FLOATING = 3,
};

union ProtocolRealtimeScalar {
  bool booleanValue;
  std::int64_t integerValue;
  double floatingValue;
};

// 协议栈回调中的定长标量；STRING/BYTES不进入实时路径。
struct ProtocolRealtimeValue {
  ProtocolRealtimeValueType valueType = ProtocolRealtimeValueType::BOOLEAN;
  std::uint32_t qualityBits = 0;
  std::int64_t timestampNs = 0;
  ProtocolRealtimeScalar value{};
};

// 已按启动计划解析的GOOSE帧视图；values只在回调执行期间有效。
struct ProtocolGooseFrameView {
  std::uint32_t subscriptionId = 0;
  std::string_view gocbRef;
  std::string_view dataSetRef;
  std::string_view goId;
  IEC61850Proto::NetworkChannel channel =
      IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED;
  std::uint16_t appId = 0;
  std::uint64_t configRevision = 0;
  std::uint32_t timeAllowedToLiveMs = 0;
  std::uint32_t stateNumber = 0;
  std::uint32_t sequenceNumber = 0;
  bool simulation = false;
  bool needsCommissioning = false;
  std::int64_t receiveTimestampNs = 0;
  // Linux内核CLOCK_REALTIME软件时间戳；实时算法仍使用receiveTimestampNs。
  std::int64_t kernelTimestampNs = 0;
  std::span<const ProtocolRealtimeValue> values;
};

// 已按启动计划解析的单个SV ASDU视图；values只在回调执行期间有效。
struct ProtocolSvFrameView {
  std::uint32_t streamId = 0;
  std::string_view svId;
  IEC61850Proto::NetworkChannel channel =
      IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED;
  std::uint16_t appId = 0;
  std::uint64_t configRevision = 0;
  std::uint16_t sampleCount = 0;
  std::uint32_t asduCount = 1;
  std::uint32_t asduIndex = 0;
  std::uint8_t sampleSynchronization = 0;
  std::int64_t receiveTimestampNs = 0;
  // Linux内核CLOCK_REALTIME软件时间戳；实时算法仍使用receiveTimestampNs。
  std::int64_t kernelTimestampNs = 0;
  std::span<const ProtocolRealtimeValue> values;
};

static_assert(std::is_trivially_copyable_v<ProtocolRealtimeScalar>);
static_assert(std::is_trivially_copyable_v<ProtocolRealtimeValue>);
static_assert(std::is_trivially_copyable_v<ProtocolGooseFrameView>);
static_assert(std::is_trivially_copyable_v<ProtocolSvFrameView>);

struct ProtocolEventCallbacks {
  std::function<void(MmsConnectionEvent)> onMmsConnection;
  std::function<void(MmsReportEvent)> onMmsReport;
  std::function<void(ProtocolGooseFrameView)> onGooseFrame;
  std::function<void(ProtocolSvFrameView)> onSvFrame;
};

struct ProtocolGoosePublishCommand {
  // 新配置使用本地GSEControl对应的发布端编号。
  std::uint32_t publisherId = 0;
  // 兼容旧的内部调用和测试；publisherId为0时才使用该字段。
  std::uint32_t subscriptionId = 0;
  bool stateChanged = false;
  std::span<const ProtocolRealtimeValue> values;
};

// 一个A/B运行通道与SCL通信网段的确定绑定。
struct ProtocolNetworkBinding {
  IEC61850Proto::NetworkChannelConfig channel;
  IEC61850Proto::SclConnectedAp connectedAccessPoint;
};

// 启动阶段编译的实时信号定义，协议热路径只传递signalId。
struct ProtocolSignalDefinition {
  std::uint32_t signalId = 0;
  std::string tag;
  std::string dataRef;
  IEC61850Proto::FunctionalConstraint fc =
      IEC61850Proto::FUNCTIONAL_CONSTRAINT_UNSPECIFIED;
  IEC61850Proto::PointSource source =
      IEC61850Proto::POINT_SOURCE_UNSPECIFIED;
  IEC61850Proto::PointValueType valueType =
      IEC61850Proto::POINT_VALUE_TYPE_UNSPECIFIED;
};

// GOOSE DataSet中的一个有序成员；signalId为0表示只解码、不进入实时总线。
struct ProtocolGooseMemberPlan {
  std::uint32_t signalId = 0;
  std::string dataRef;
  IEC61850Proto::FunctionalConstraint fc =
      IEC61850Proto::FUNCTIONAL_CONSTRAINT_UNSPECIFIED;
  IEC61850Proto::PointValueType valueType =
      IEC61850Proto::POINT_VALUE_TYPE_UNSPECIFIED;
  // FLOAT32/FLOAT64的线编码宽度；非浮点成员必须为0。
  std::uint8_t encodedSize = 0;
  bool qualityValue = false;
};

// 单个GOOSE订阅或发布端在一个A/B网络通道上的二层参数。
struct ProtocolGooseEndpointPlan {
  IEC61850Proto::NetworkChannel channel =
      IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED;
  std::string interfaceName;
  std::string subnetworkName;
  std::string destinationMac;
  std::uint16_t appId = 0;
  bool vlanTagged = false;
  std::uint16_t vlanId = 0;
  std::uint8_t vlanPriority = 0;
};

// 从ExtRef解析出的一个逻辑GOOSE订阅及其A/B端点。
struct ProtocolGooseSubscriptionPlan {
  std::uint32_t subscriptionId = 0;
  std::string publisherIed;
  std::string controlRef;
  std::string dataSetRef;
  std::string goId;
  std::uint64_t configRevision = 0;
  std::vector<ProtocolGooseMemberPlan> members;
  std::vector<ProtocolGooseEndpointPlan> endpoints;
};

// 当前目标IED本地GSEControl的独立发布计划；不承担GOOSE接收路由。
struct ProtocolGoosePublisherPlan {
  std::uint32_t publisherId = 0;
  std::string publisherIed;
  std::string controlRef;
  std::string dataSetRef;
  std::string goId;
  std::uint64_t configRevision = 0;
  std::vector<ProtocolGooseMemberPlan> members;
  std::vector<ProtocolGooseEndpointPlan> endpoints;
};

enum class ProtocolSvMemberEncoding : std::uint8_t {
  BOOLEAN = 1,
  SIGNED_INTEGER = 2,
  UNSIGNED_INTEGER = 3,
  FLOATING_POINT = 4,
};

// SV数据集中的一个有序采样成员。
struct ProtocolSvMemberPlan {
  std::uint32_t signalId = 0;
  std::string dataRef;
  IEC61850Proto::FunctionalConstraint fc =
      IEC61850Proto::FUNCTIONAL_CONSTRAINT_UNSPECIFIED;
  IEC61850Proto::PointValueType valueType =
      IEC61850Proto::POINT_VALUE_TYPE_UNSPECIFIED;
  ProtocolSvMemberEncoding encoding =
      ProtocolSvMemberEncoding::SIGNED_INTEGER;
  std::uint8_t encodedSize = 4;
};

// SV采样成员对应的内部派生量；当前首期只生成单周期RMS。
struct ProtocolSvDerivedMemberPlan {
  std::uint32_t inputSignalId = 0;
  std::uint32_t rmsSignalId = 0;
  std::string rmsDataRef;
};

// 单个SV采样流在一个A/B网络通道上的二层接收参数。
struct ProtocolSvEndpointPlan {
  IEC61850Proto::NetworkChannel channel =
      IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED;
  std::string interfaceName;
  std::string subnetworkName;
  std::string destinationMac;
  std::uint16_t appId = 0;
  bool vlanTagged = false;
  std::uint16_t vlanId = 0;
  std::uint8_t vlanPriority = 0;
};

// 从ExtRef解析出的一个逻辑SV采样流及其A/B端点。
struct ProtocolSvStreamPlan {
  std::uint32_t streamId = 0;
  std::string publisherIed;
  std::string controlRef;
  std::string dataSetRef;
  std::string svId;
  std::uint64_t configRevision = 0;
  // SCL smpRate：每个额定周期的采样点数，不是绝对Hz采样率。
  std::uint32_t sampleRate = 0;
  // 由IED配置解析出的额定频率；用于绝对采样率和数学窗口语义。
  double nominalFrequencyHz = 50.0;
  std::uint32_t nofAsdu = 0;
  std::vector<ProtocolSvMemberPlan> members;
  std::vector<ProtocolSvDerivedMemberPlan> derivedMembers;
  std::vector<ProtocolSvEndpointPlan> endpoints;
};

// 单个逻辑IED的协议启动计划，避免协议栈自行遍历和选择聚合SCL模型。
struct ProtocolIedPlan {
  IEC61850Proto::IedConfig config;
  ThreadRuntimePolicy realtimePolicy;
  IEC61850Proto::SclIed ied;
  std::vector<IEC61850Proto::SclConnectedAp> connectedAccessPoints;
  std::vector<ProtocolNetworkBinding> networkBindings;
  std::vector<ProtocolSignalDefinition> realtimeSignals;
  std::vector<ProtocolGooseSubscriptionPlan> gooseSubscriptions;
  std::vector<ProtocolGoosePublisherPlan> goosePublishers;
  std::vector<ProtocolSvStreamPlan> svStreams;
};

class ProtocolStackAdapter {
public:
  virtual ~ProtocolStackAdapter() = default;

  virtual grpc::Status StartIed(ProtocolIedPlan plan,
                                ProtocolEventCallbacks callbacks) = 0;
  virtual grpc::Status StopIed(std::string_view connName) = 0;

  // 在已就绪的活动MMS通道上串行读取变量；协议栈负责保证不与报告接收并发访问传输对象。
  virtual grpc::Status ReadMms(std::string_view,
                               const MmsReadRequest&,
                               MmsReadResponse*) {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                        "当前IEC61850协议栈不支持MMS Read控制请求");
  }

  // 在已就绪的活动MMS通道上串行写入变量；控制策略和权限校验由上层负责。
  virtual grpc::Status WriteMms(std::string_view,
                                const MmsWriteRequest&,
                                MmsWriteResponse*) {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                        "当前IEC61850协议栈不支持MMS Write控制请求");
  }

  // 普通SBO选择；协议栈必须在READY活动MMS通道上串行执行Read。
  virtual grpc::Status SelectMmsControl(std::string_view,
                                        const MmsObjectName&,
                                        MmsReadResponse*) {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                        "当前IEC61850协议栈不支持MMS SBO选择请求");
  }

  // 执行SBOw、Oper或Cancel；控制策略由调用方完成，协议栈只负责串行交换。
  virtual grpc::Status WriteMmsControl(std::string_view,
                                       const MmsControlCommand&,
                                       MmsWriteResponse*) {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                        "当前IEC61850协议栈不支持MMS控制Write请求");
  }

  virtual grpc::Status ReadSettingGroupStatus(
      std::string_view, const MmsSettingGroupPlan&,
      MmsSettingGroupStatus*) {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                        "当前IEC61850协议栈不支持SGCB状态读取");
  }

  virtual grpc::Status SelectSettingGroup(std::string_view,
                                          const MmsSettingGroupPlan&,
                                          std::uint32_t) {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                        "当前IEC61850协议栈不支持SGCB选择");
  }

  virtual grpc::Status ConfirmSettingGroupEdit(
      std::string_view, const MmsSettingGroupPlan&) {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                        "当前IEC61850协议栈不支持SGCB确认");
  }

  virtual grpc::Status CancelSettingGroupEdit(
      std::string_view, const MmsSettingGroupPlan&) {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                        "当前IEC61850协议栈不支持SGCB取消");
  }

  virtual grpc::Status ActivateSettingGroup(std::string_view,
                                            const MmsSettingGroupPlan&,
                                            std::uint32_t) {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                        "当前IEC61850协议栈不支持SGCB激活");
  }

  // DataCenter同步命令入口；协议栈按在线ctlModel完成完整控制操作。
  virtual grpc::Status ExecuteMmsPointControl(
      std::string_view, const MmsPointControlCommand&, MmsWriteResponse*) {
    return grpc::Status(
        grpc::StatusCode::UNIMPLEMENTED,
        "当前IEC61850协议栈不支持DataCenter同步MMS控制请求");
  }

  // 发布端点由协议栈内部串行处理；实时保护链路不调用外部服务。
  virtual grpc::Status PublishGoose(
      std::string_view,
      const ProtocolGoosePublishCommand&) {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                        "当前IEC61850协议栈不支持GOOSE发布");
  }
};

std::shared_ptr<ProtocolStackAdapter> MakeUnavailableProtocolStack();

}  // namespace IEC61850
