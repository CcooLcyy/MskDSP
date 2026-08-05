#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

#include <grpcpp/support/status.h>

#include "IEC61850MmsService.h"

namespace IEC61850 {

// 在线NameList核对后的SGCB服务能力。NameList只能证明对象存在，实际写权限
// 仍需由服务端Write结果确认；permission字段用于上层策略提前拒绝越权操作。
struct MmsSettingGroupCapabilities {
  bool discovered = false;
  bool supportsRead = false;
  bool supportsWrite = false;
  bool supportsSelect = false;
  bool supportsConfirm = false;
  bool supportsCancel = false;
  bool supportsActivate = false;
  bool readPermission = true;
  bool writePermission = true;
  std::optional<std::chrono::milliseconds> timeout;
};

enum class MmsSettingGroupState : std::uint8_t {
  UNKNOWN = 0,
  SYNCHRONIZED = 1,
  EDITING = 2,
  CONFIRMED = 3,
  INDETERMINATE = 4,
};

// AR502H常用SGCB对象引用。引用由SCL/IED配置提供，不在协议层硬编码厂商名称。
struct MmsSettingGroupPlan {
  MmsObjectName numberOfGroups;
  MmsObjectName activeGroup;
  MmsObjectName editGroup;
  MmsObjectName confirmEdit;
  std::optional<MmsObjectName> lastActivationTime;
  std::uint32_t maxGroups = 256;
  // 由在线NameList核对后填写；未填写时保持历史调用方的兼容行为。
  std::optional<MmsSettingGroupCapabilities> capabilities;
};

struct MmsSettingGroupStatus {
  std::uint32_t numberOfGroups = 0;
  std::uint32_t activeGroup = 0;
  std::uint32_t editGroup = 0;
  bool confirmEdit = false;
  std::optional<std::int64_t> lastActivationTimeMs;
  MmsSettingGroupState state = MmsSettingGroupState::UNKNOWN;
};

using MmsSettingGroupRead =
    std::function<grpc::Status(const MmsReadRequest&, MmsReadResponse*)>;
using MmsSettingGroupWrite =
    std::function<grpc::Status(const MmsWriteRequest&, MmsWriteResponse*)>;

// SGCB高层流程适配器。它只负责引用、类型、组号和写入顺序校验，网络交换
// 由调用方提供的串行MMS Read/Write回调完成。
class MmsSettingGroupClient {
public:
  MmsSettingGroupClient(MmsSettingGroupRead read,
                        MmsSettingGroupWrite write);

  grpc::Status ReadStatus(const MmsSettingGroupPlan& plan,
                          MmsSettingGroupStatus* status) const;
  grpc::Status Select(const MmsSettingGroupPlan& plan,
                      std::uint32_t group) const;
  grpc::Status ConfirmEdit(const MmsSettingGroupPlan& plan) const;
  grpc::Status CancelEdit(const MmsSettingGroupPlan& plan) const;
  grpc::Status Activate(const MmsSettingGroupPlan& plan,
                        std::uint32_t group) const;

  // 读取/写入当前已选定定值组的定值对象。对象列表按调用方提供的顺序
  // 进入同一MMS串行请求；写入失败时尝试恢复写入前快照。
  grpc::Status ReadValues(const MmsSettingGroupPlan& plan,
                          std::span<const MmsObjectName> variables,
                          MmsReadResponse* response) const;
  grpc::Status WriteValues(const MmsSettingGroupPlan& plan,
                           std::span<const MmsWriteRequestItem> variables,
                           MmsWriteResponse* response) const;

  // 从当前MMS会话的NameList对象核对SGCB能力。缺少对象时返回OK但将对应
  // 能力置为false，调用方必须把结果写回plan.capabilities后再执行操作。
  static grpc::Status DiscoverCapabilities(
      const MmsSettingGroupPlan& plan,
      std::span<const MmsObjectName> onlineObjects,
      MmsSettingGroupCapabilities* capabilities);
  static grpc::Status DiscoverCapabilities(
      const MmsSettingGroupPlan& plan,
      std::span<const std::string> onlineIdentifiers,
      MmsSettingGroupCapabilities* capabilities);

  static grpc::Status ValidatePlan(const MmsSettingGroupPlan& plan);
  static grpc::Status ValidateGroup(const MmsSettingGroupPlan& plan,
                                    std::uint32_t group);

private:
  grpc::Status WriteInteger(const MmsObjectName& object,
                            std::uint32_t value,
                            std::optional<std::chrono::milliseconds> timeout =
                                std::nullopt) const;
  grpc::Status WriteBoolean(
      const MmsObjectName& object, bool value,
      std::optional<std::chrono::milliseconds> timeout = std::nullopt) const;
  grpc::Status CheckReadCapability(const MmsSettingGroupPlan& plan) const;
  grpc::Status CheckWriteCapability(const MmsSettingGroupPlan& plan,
                                    std::string_view operation) const;
  grpc::Status Rollback(const MmsSettingGroupPlan& plan,
                        const MmsSettingGroupStatus& previous,
                        bool restoreEdit) const;

  MmsSettingGroupRead read_;
  MmsSettingGroupWrite write_;
};

}  // namespace IEC61850
