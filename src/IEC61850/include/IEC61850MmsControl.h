#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <grpcpp/support/status.h>

#include "IEC61850.pb.h"
#include "IEC61850MmsService.h"

namespace IEC61850 {

struct MmsOnlineDirectory;
struct ProtocolIedPlan;

// IEC 61850控制服务对应的协议级操作；权限和联锁由上层策略决定。
enum class MmsControlOperation : std::uint8_t {
  SELECT = 0,
  SELECT_WITH_VALUE = 1,
  OPERATE = 2,
  CANCEL = 3,
};

// 一个已经通过上层控制策略检查的MMS控制命令。
struct MmsControlCommand {
  MmsControlOperation operation = MmsControlOperation::OPERATE;
  MmsObjectName controlObject;
  // ctlVal必须是一个完整的MMS Data选择TLV。
  std::vector<std::uint8_t> controlValue;
  // IEC 61850 ctlNum为无符号8位值。
  std::uint8_t controlNumber = 0;
  std::uint8_t originCategory = 0;
  std::vector<std::uint8_t> originIdentifier;
  // Oper结构中的T，使用Unix epoch毫秒；UTC time质量固定为已同步。
  std::int64_t timestampMs = 0;
  // operTm为可选的Unix epoch毫秒时间戳。
  std::optional<std::int64_t> operateTimestampMs;
  bool test = false;
  // Check只使用低两位：bit0为interlock-check，bit1为 synchrocheck。
  std::uint8_t check = 0;
  // 当前控制阶段可使用的剩余等待时间；由同步控制入口逐阶段收紧。
  std::optional<std::chrono::milliseconds> requestTimeout;
  // RPC取消状态；只允许读取，不保存ServerContext指针。
  std::shared_ptr<std::atomic_bool> cancellation;
};

// DataCenter同步命令转换后的标量控制输入；工程量换算在MMS控制层完成。
struct MmsPointControlCommand {
  MmsObjectName controlObject;
  IEC61850Proto::PointValueType valueType =
      IEC61850Proto::POINT_VALUE_TYPE_UNSPECIFIED;
  bool boolValue = false;
  std::int64_t intValue = 0;
  double doubleValue = 0.0;
  double scale = 1.0;
  double offset = 0.0;
  // DataCenter调用方提供的整次控制序列截止时间；未设置时使用在线控制参数。
  std::optional<std::chrono::milliseconds> requestTimeout;
  // RPC取消状态；生命周期由本次控制命令共享管理。
  std::shared_ptr<std::atomic_bool> cancellation;
};

// IEC 61850增强安全控制失败时由CommandTermination携带的LastApplError。
struct MmsLastApplError {
  MmsObjectName controlObject;
  std::int64_t error = 0;
  std::uint8_t originCategory = 0;
  std::vector<std::uint8_t> originIdentifier;
  std::uint8_t controlNumber = 0;
  std::int64_t addCause = 0;
};

// 一份与当前Oper请求关联的CommandTermination结果。
struct MmsCommandTermination {
  MmsObjectName operationObject;
  bool success = false;
  std::optional<MmsLastApplError> lastApplError;
};

// 一个在线控制对象的协议能力；权限和联锁不在该结构中表达。
struct MmsControlCapability {
  MmsObjectName object;
  bool supportsSbo = false;
  bool supportsSboWithValue = false;
  bool supportsOperate = false;
  bool supportsCancel = false;
  // 在线ctlModel；0表示不可控，1/3为direct，2/4为SBO。
  std::optional<std::int64_t> ctlModel;
  // 在线sboTimeout/operTimeout，单位毫秒；缺失时由兼容默认值兜底。
  std::optional<std::uint32_t> sboTimeoutMs;
  std::optional<std::uint32_t> operTimeoutMs;
  // 在线GetVariableAccessAttributes返回的ctlVal类型；缺少在线类型时
  // 仅保留协议级标签校验，不能把未知类型当作已知标量。
  std::optional<MmsTypeSpecification> ctlValType;
};

// 由SCL的FC=CO对象和服务端NameList交集编译出的控制能力模型。
struct MmsControlModel {
  std::vector<MmsControlCapability> controls;

  const MmsControlCapability* Find(const MmsObjectName& object) const noexcept;
};

// 构造控制能力模型；缺少计划内在线成员时返回FAILED_PRECONDITION。
grpc::Status BuildMmsControlModel(const ProtocolIedPlan& plan,
                                   const MmsOnlineDirectory& directory,
                                   MmsControlModel* model);

// 当前物理MMS会话内的SBO选择保持状态。
class MmsSboState {
public:
  explicit MmsSboState(std::chrono::milliseconds holdTime =
                           std::chrono::milliseconds(5000));

  grpc::Status RecordSelection(const MmsObjectName& object,
                               std::int64_t nowMs,
                               std::optional<std::chrono::milliseconds>
                                   holdTime = std::nullopt);
  bool IsSelected(const MmsObjectName& object, std::int64_t nowMs) const;
  // 返回结果不确定时锁定对象，防止调用方在超时后重复执行。
  bool IsControlUncertain(const MmsObjectName& object) const;
  // Oper入队前建立执行占用；增强安全控制在CommandTermination或Cancel完成前保留。
  bool IsOperationPending(const MmsObjectName& object) const;
  // 已知远端拒绝但SBO保持仍需显式Cancel时为true。
  bool IsCancelRequired(const MmsObjectName& object) const;
  // 为一次即将发送的Oper建立唯一执行占用；同一对象不能重复占用。
  grpc::Status RecordPendingOperation(const MmsObjectName& object);
  // 远端已明确拒绝本次Oper；保留选择保持并要求Cancel，禁止再次Oper。
  grpc::Status MarkOperationRejected(const MmsObjectName& object);
  // 在请求尚未发送或已明确本地取消时释放执行占用，但保留SBO选择。
  void ClearPendingOperation(const MmsObjectName& object);
  grpc::Status MarkUncertain(const MmsObjectName& object);
  void ClearSelection(const MmsObjectName& object);
  void Clear();

private:
  struct Selection {
    MmsObjectName object;
    std::int64_t expiresAtMs = 0;
  };

  struct Execution {
    MmsObjectName object;
    bool pending = false;
    bool cancelRequired = false;
    bool uncertain = false;
  };

  std::chrono::milliseconds holdTime_;
  mutable std::mutex mutex_;
  std::vector<Selection> selections_;
  std::vector<Execution> executions_;
  // 不确定状态无法入表时的全局保守门禁，只能由新会话Clear解除。
  bool uncertaintyOverflow_ = false;
};

// 校验一个控制操作是否符合在线能力和当前SBO保持状态。
grpc::Status ValidateMmsControlOperation(
    const MmsControlModel& model, const MmsObjectName& object,
    MmsControlOperation operation, const MmsSboState& sboState,
    std::int64_t nowMs);

// 按在线控制参数选择一次控制请求的等待窗口；缺少在线参数时使用兼容默认值。
std::chrono::milliseconds ResolveMmsControlTimeout(
    const MmsControlCapability& capability, MmsControlOperation operation,
    std::chrono::milliseconds fallback) noexcept;

// 按在线TypeSpecification校验控制命令中的ctlVal Data选择。
grpc::Status ValidateMmsControlValue(
    const MmsControlModel& model, const MmsObjectName& object,
    std::span<const std::uint8_t> encodedValue);

// 按在线ctlVal TypeSpecification编码DataCenter标量控制值，并执行反向工程量换算。
grpc::Status EncodeMmsPointControlValue(
    const MmsPointControlCommand& command,
    const MmsControlCapability& capability,
    std::vector<std::uint8_t>* encodedValue);

// 校验普通SBO Read返回的单个非空VisibleString；传入目标对象时同时核对对象引用。
grpc::Status ValidateMmsControlSelectResponse(
    const MmsReadResponse& response,
    const MmsObjectName* expectedObject = nullptr);

// 解码增强安全控制的CommandTermination InformationReport；expectedOper和
// expectedControlNumber用于阻止其他控制对象或旧ctlNum的报告误完成当前请求。
grpc::Status DecodeMmsCommandTermination(
    std::span<const std::uint8_t> input, const MmsObjectName& expectedOper,
    std::uint8_t expectedControlNumber, MmsCommandTermination* result);

// 编码SBOw、Oper或Cancel使用的各自Data.structure。
grpc::Status EncodeMmsControlStructure(
    const MmsControlCommand& command,
    std::vector<std::uint8_t>* encodedData);

// 普通SBO选择通过读取控制对象的$SBO完成。
grpc::Status BuildMmsControlSelectRequest(
    const MmsObjectName& controlObject, MmsReadRequest* request);

// 构造SBOw、Oper或Cancel的单项MMS Write请求；SELECT操作会被拒绝。
grpc::Status BuildMmsControlWriteRequest(
    const MmsControlCommand& command, MmsWriteRequest* request);

}  // namespace IEC61850
