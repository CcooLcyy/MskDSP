#include "IEC61850MmsSettingGroup.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <limits>
#include <string>

#include "Logger.h"

namespace IEC61850 {
namespace {

grpc::Status Invalid(std::string message) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::move(message));
}

grpc::Status Failed(std::string message) {
  return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                      std::move(message));
}

bool ValidObject(const MmsObjectName& object) {
  return !object.identifier.empty() &&
         (object.type == MmsObjectNameType::VMD_SPECIFIC ||
          (object.type == MmsObjectNameType::DOMAIN_SPECIFIC &&
           !object.domain.empty()) ||
          object.type == MmsObjectNameType::AA_SPECIFIC);
}

struct Tlv {
  std::uint8_t tag = 0;
  std::span<const std::uint8_t> value;
};

bool ReadTlv(std::span<const std::uint8_t> input, std::size_t* offset,
             Tlv* output) {
  if (offset == nullptr || output == nullptr || *offset >= input.size()) {
    return false;
  }
  const auto tag = input[(*offset)++];
  if (*offset >= input.size()) {
    return false;
  }
  const auto first = input[(*offset)++];
  std::size_t length = first;
  if ((first & 0x80) != 0) {
    const auto count = static_cast<std::size_t>(first & 0x7f);
    if (count == 0 || count > sizeof(std::size_t) ||
        count > input.size() - *offset) {
      return false;
    }
    length = 0;
    for (std::size_t index = 0; index < count; ++index) {
      if (length > (std::numeric_limits<std::size_t>::max() >> 8)) {
        return false;
      }
      length = (length << 8) | input[(*offset)++];
    }
  }
  if (length > input.size() - *offset) {
    return false;
  }
  output->tag = tag;
  output->value = input.subspan(*offset, length);
  *offset += length;
  return true;
}

std::optional<std::uint32_t> DecodeGroupNumber(
    std::span<const std::uint8_t> encoded) {
  std::size_t offset = 0;
  Tlv outer;
  if (!ReadTlv(encoded, &offset, &outer) || offset != encoded.size() ||
      (outer.tag != 0x85 && outer.tag != 0x86 && outer.tag != 0x02) ||
      outer.value.empty() || outer.value.size() > sizeof(std::uint32_t) + 1) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  auto bytes = outer.value;
  if (outer.tag != 0x86 && outer.tag != 0x02 && bytes.front() & 0x80) {
    return std::nullopt;
  }
  if (bytes.size() > 1 && bytes.front() == 0) {
    bytes = bytes.subspan(1);
  }
  for (const auto byte : bytes) {
    if (value > (std::numeric_limits<std::uint32_t>::max() >> 8)) {
      return std::nullopt;
    }
    value = (value << 8) | byte;
  }
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(value);
}

std::optional<bool> DecodeBoolean(std::span<const std::uint8_t> encoded) {
  std::size_t offset = 0;
  Tlv outer;
  if (!ReadTlv(encoded, &offset, &outer) || offset != encoded.size() ||
      outer.tag != 0x83 || outer.value.size() != 1) {
    return std::nullopt;
  }
  return outer.value.front() != 0;
}

std::optional<std::int64_t> DecodeTime(std::span<const std::uint8_t> encoded) {
  std::size_t offset = 0;
  Tlv outer;
  if (!ReadTlv(encoded, &offset, &outer) || offset != encoded.size() ||
      (outer.tag != 0x8c && outer.tag != 0x91) ||
      (outer.tag == 0x8c && outer.value.size() != 6) ||
      (outer.tag == 0x91 && outer.value.size() != 8)) {
    return std::nullopt;
  }
  if (outer.tag == 0x8c) {
    std::uint64_t value = 0;
    for (const auto byte : outer.value) {
      value = (value << 8) | byte;
    }
    if (value > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) {
      return std::nullopt;
    }
    return static_cast<std::int64_t>(value);
  }
  std::uint64_t seconds = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    seconds = (seconds << 8) | outer.value[index];
  }
  return static_cast<std::int64_t>(seconds) * 1000;
}

grpc::Status CheckWrite(const MmsWriteResponse& response) {
  if (response.items.size() != 1 || !response.items.front().success) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "IEC61850 SGCB远端写入失败");
  }
  return grpc::Status::OK;
}

grpc::Status PermissionDenied(std::string message) {
  return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                      std::move(message));
}

grpc::Status Aborted(std::string message) {
  return grpc::Status(grpc::StatusCode::ABORTED, std::move(message));
}

bool ContainsObject(std::span<const MmsObjectName> objects,
                    const MmsObjectName& expected) {
  return std::find(objects.begin(), objects.end(), expected) != objects.end();
}

bool ContainsIdentifier(std::span<const std::string> identifiers,
                        const MmsObjectName& expected) {
  for (const auto& identifier : identifiers) {
    if (identifier == expected.identifier ||
        identifier == expected.domain + "/" + expected.identifier ||
        identifier == expected.domain + expected.identifier) {
      return true;
    }
  }
  return false;
}

std::optional<std::chrono::milliseconds> CapabilityTimeout(
    const MmsSettingGroupPlan& plan) {
  if (!plan.capabilities.has_value()) {
    return std::nullopt;
  }
  return plan.capabilities->timeout;
}

grpc::Status CheckElapsed(
    std::chrono::steady_clock::time_point started,
    std::optional<std::chrono::milliseconds> timeout,
    grpc::Status status, std::string_view operation, bool write) {
  if (!status.ok()) {
    if (write && (status.error_code() == grpc::StatusCode::UNAVAILABLE ||
                  status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED ||
                  status.error_code() == grpc::StatusCode::CANCELLED ||
                  status.error_code() == grpc::StatusCode::UNKNOWN)) {
      return Aborted(std::format("IEC61850 SGCB{}结果不确定，请重新读取状态后再操作",
                                 operation));
    }
    return status;
  }
  if (timeout.has_value() &&
      std::chrono::steady_clock::now() - started > *timeout) {
    if (write) {
      return Aborted(std::format("IEC61850 SGCB{}超时，远端状态不确定",
                                 operation));
    }
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        std::format("IEC61850 SGCB{}超时", operation));
  }
  return grpc::Status::OK;
}

}  // namespace

MmsSettingGroupClient::MmsSettingGroupClient(MmsSettingGroupRead read,
                                             MmsSettingGroupWrite write)
    : read_(std::move(read)), write_(std::move(write)) {}

grpc::Status MmsSettingGroupClient::ValidatePlan(
    const MmsSettingGroupPlan& plan) {
  if (!ValidObject(plan.numberOfGroups) || !ValidObject(plan.activeGroup) ||
      !ValidObject(plan.editGroup) || !ValidObject(plan.confirmEdit) ||
      plan.maxGroups == 0) {
    return Invalid("IEC61850 SGCB对象引用或组数上限无效");
  }
  if (plan.lastActivationTime.has_value() &&
      !ValidObject(*plan.lastActivationTime)) {
    return Invalid("IEC61850 SGCB最后激活时间对象引用无效");
  }
  if (plan.numberOfGroups == plan.activeGroup ||
      plan.numberOfGroups == plan.editGroup ||
      plan.numberOfGroups == plan.confirmEdit ||
      plan.activeGroup == plan.editGroup ||
      plan.activeGroup == plan.confirmEdit ||
      plan.editGroup == plan.confirmEdit) {
    return Invalid("IEC61850 SGCB对象引用不能重复");
  }
  if (plan.capabilities.has_value() && plan.capabilities->timeout.has_value() &&
      *plan.capabilities->timeout <= std::chrono::milliseconds::zero()) {
    return Invalid("IEC61850 SGCB统一超时必须大于零");
  }
  return grpc::Status::OK;
}

grpc::Status MmsSettingGroupClient::DiscoverCapabilities(
    const MmsSettingGroupPlan& plan,
    std::span<const MmsObjectName> onlineObjects,
    MmsSettingGroupCapabilities* capabilities) {
  if (capabilities == nullptr) {
    return Invalid("IEC61850 SGCB能力输出参数为空");
  }
  auto validation = ValidatePlan(plan);
  if (!validation.ok()) {
    return validation;
  }
  *capabilities = {};
  capabilities->discovered = true;
  const bool number = ContainsObject(onlineObjects, plan.numberOfGroups);
  const bool active = ContainsObject(onlineObjects, plan.activeGroup);
  const bool edit = ContainsObject(onlineObjects, plan.editGroup);
  const bool confirm = ContainsObject(onlineObjects, plan.confirmEdit);
  const bool lastActivation =
      !plan.lastActivationTime.has_value() ||
      ContainsObject(onlineObjects, *plan.lastActivationTime);
  capabilities->supportsRead = number && active && edit && confirm &&
                               lastActivation;
  capabilities->supportsWrite = edit && confirm && active;
  capabilities->supportsSelect = edit;
  capabilities->supportsConfirm = confirm;
  capabilities->supportsCancel = confirm;
  capabilities->supportsActivate = active;
  // 保留调用方预先指定的超时，在线目录本身不提供该策略参数。
  if (plan.capabilities.has_value()) {
    capabilities->timeout = plan.capabilities->timeout;
    capabilities->readPermission = plan.capabilities->readPermission;
    capabilities->writePermission = plan.capabilities->writePermission;
  }
  LOG_INFO("IEC61850 SGCB在线能力核对: Read={}, Write={}, Select={}, Confirm={}, Activate={}",
           capabilities->supportsRead ? "支持" : "不支持",
           capabilities->supportsWrite ? "支持" : "不支持",
           capabilities->supportsSelect ? "支持" : "不支持",
           capabilities->supportsConfirm ? "支持" : "不支持",
           capabilities->supportsActivate ? "支持" : "不支持");
  return grpc::Status::OK;
}

grpc::Status MmsSettingGroupClient::DiscoverCapabilities(
    const MmsSettingGroupPlan& plan,
    std::span<const std::string> onlineIdentifiers,
    MmsSettingGroupCapabilities* capabilities) {
  if (capabilities == nullptr) {
    return Invalid("IEC61850 SGCB能力输出参数为空");
  }
  auto validation = ValidatePlan(plan);
  if (!validation.ok()) {
    return validation;
  }
  std::vector<MmsObjectName> matched;
  const std::array<const MmsObjectName*, 4> objects = {
      &plan.numberOfGroups, &plan.activeGroup, &plan.editGroup,
      &plan.confirmEdit};
  for (const auto* object : objects) {
    if (ContainsIdentifier(onlineIdentifiers, *object)) {
      matched.push_back(*object);
    }
  }
  if (plan.lastActivationTime.has_value() &&
      ContainsIdentifier(onlineIdentifiers, *plan.lastActivationTime)) {
    matched.push_back(*plan.lastActivationTime);
  }
  return DiscoverCapabilities(plan, matched, capabilities);
}

grpc::Status MmsSettingGroupClient::CheckReadCapability(
    const MmsSettingGroupPlan& plan) const {
  if (!plan.capabilities.has_value()) {
    return grpc::Status::OK;
  }
  const auto& capabilities = *plan.capabilities;
  if (!capabilities.discovered || !capabilities.supportsRead) {
    return Failed("IEC61850 SGCB在线能力未协商Read或对象不完整");
  }
  if (!capabilities.readPermission) {
    return PermissionDenied("IEC61850 SGCB没有读取权限");
  }
  return grpc::Status::OK;
}

grpc::Status MmsSettingGroupClient::CheckWriteCapability(
    const MmsSettingGroupPlan& plan, std::string_view operation) const {
  if (!plan.capabilities.has_value()) {
    return grpc::Status::OK;
  }
  const auto& capabilities = *plan.capabilities;
  if (!capabilities.discovered || !capabilities.supportsWrite) {
    return Failed(std::format("IEC61850 SGCB在线能力未协商{}或对象不完整",
                              operation));
  }
  if (!capabilities.writePermission) {
    return PermissionDenied(
        std::format("IEC61850 SGCB没有{}写入权限", operation));
  }
  if (capabilities.timeout.has_value() &&
      *capabilities.timeout <= std::chrono::milliseconds::zero()) {
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "IEC61850 SGCB统一超时已耗尽");
  }
  return grpc::Status::OK;
}

grpc::Status MmsSettingGroupClient::ValidateGroup(
    const MmsSettingGroupPlan& plan, std::uint32_t group) {
  auto status = ValidatePlan(plan);
  if (!status.ok()) {
    return status;
  }
  if (group == 0 || group > plan.maxGroups) {
    return Invalid("IEC61850 SGCB组号必须从1开始且不超过配置上限");
  }
  return grpc::Status::OK;
}

grpc::Status MmsSettingGroupClient::ReadStatus(
    const MmsSettingGroupPlan& plan, MmsSettingGroupStatus* status) const {
  if (status == nullptr) {
    return Invalid("IEC61850 SGCB状态输出参数为空");
  }
  *status = {};
  auto validation = ValidatePlan(plan);
  if (!validation.ok()) {
    return validation;
  }
  validation = CheckReadCapability(plan);
  if (!validation.ok()) {
    return validation;
  }
  if (!read_) {
    return Failed("IEC61850 SGCB读取回调未配置");
  }
  MmsReadRequest request;
  request.variables = {plan.numberOfGroups, plan.activeGroup, plan.editGroup,
                       plan.confirmEdit};
  if (plan.lastActivationTime.has_value()) {
    request.variables.push_back(*plan.lastActivationTime);
  }
  MmsReadResponse response;
  const auto started = std::chrono::steady_clock::now();
  auto readStatus = read_(request, &response);
  readStatus = CheckElapsed(started, CapabilityTimeout(plan), readStatus,
                            "状态读取", false);
  if (!readStatus.ok()) {
    return readStatus;
  }
  if (response.items.size() != request.variables.size()) {
    return grpc::Status(grpc::StatusCode::DATA_LOSS,
                        "IEC61850 SGCB读取结果数量不匹配");
  }
  const auto number = DecodeGroupNumber(response.items[0].encodedData);
  const auto active = DecodeGroupNumber(response.items[1].encodedData);
  const auto edit = DecodeGroupNumber(response.items[2].encodedData);
  const auto confirmed = DecodeBoolean(response.items[3].encodedData);
  if (!response.items[0].success || !response.items[1].success ||
      !response.items[2].success || !response.items[3].success ||
      !number.has_value() || !active.has_value() || !edit.has_value() ||
      !confirmed.has_value() || *number == 0 || *number > plan.maxGroups ||
      *active > *number || *edit > *number) {
    return grpc::Status(grpc::StatusCode::DATA_LOSS,
                        "IEC61850 SGCB状态数据类型或范围无效");
  }
  status->numberOfGroups = *number;
  status->activeGroup = *active;
  status->editGroup = *edit;
  status->confirmEdit = *confirmed;
  status->state = *confirmed
                      ? MmsSettingGroupState::CONFIRMED
                      : (*edit == 0 ? MmsSettingGroupState::SYNCHRONIZED
                                    : MmsSettingGroupState::EDITING);
  if (plan.lastActivationTime.has_value()) {
    if (response.items.size() != 5 || !response.items[4].success) {
      return grpc::Status(grpc::StatusCode::DATA_LOSS,
                          "IEC61850 SGCB最后激活时间读取失败");
    }
    status->lastActivationTimeMs =
        DecodeTime(response.items[4].encodedData);
    if (!status->lastActivationTimeMs.has_value()) {
      return grpc::Status(grpc::StatusCode::DATA_LOSS,
                          "IEC61850 SGCB最后激活时间类型无效");
    }
  }
  return grpc::Status::OK;
}

grpc::Status MmsSettingGroupClient::WriteInteger(const MmsObjectName& object,
                                                 std::uint32_t value,
                                                 std::optional<std::chrono::milliseconds>
                                                     timeout) const {
  if (!write_) {
    return Failed("IEC61850 SGCB写入回调未配置");
  }
  MmsWriteRequest request;
  MmsWriteRequestItem& item = request.items.emplace_back();
  item.variable = object;
  auto status = EncodeMmsDataSigned(static_cast<std::int64_t>(value),
                                    &item.encodedData);
  if (!status.ok()) {
    return status;
  }
  MmsWriteResponse response;
  const auto started = std::chrono::steady_clock::now();
  status = write_(request, &response);
  status = CheckElapsed(started, timeout, status, "组号写入", true);
  if (!status.ok()) {
    return status;
  }
  LOG_INFO("IEC61850 SGCB写入组号: 对象={}, 组号={}", object.identifier,
           value);
  return CheckWrite(response);
}

grpc::Status MmsSettingGroupClient::WriteBoolean(const MmsObjectName& object,
                                                 bool value,
                                                 std::optional<std::chrono::milliseconds>
                                                     timeout) const {
  if (!write_) {
    return Failed("IEC61850 SGCB写入回调未配置");
  }
  MmsWriteRequest request;
  MmsWriteRequestItem& item = request.items.emplace_back();
  item.variable = object;
  auto status = EncodeMmsDataBoolean(value, &item.encodedData);
  if (!status.ok()) {
    return status;
  }
  MmsWriteResponse response;
  const auto started = std::chrono::steady_clock::now();
  status = write_(request, &response);
  status = CheckElapsed(started, timeout, status, "确认标志写入", true);
  if (!status.ok()) {
    return status;
  }
  LOG_INFO("IEC61850 SGCB写入确认标志: 对象={}, 值={}", object.identifier,
           value);
  return CheckWrite(response);
}

grpc::Status MmsSettingGroupClient::Select(const MmsSettingGroupPlan& plan,
                                           std::uint32_t group) const {
  auto status = ValidateGroup(plan, group);
  if (!status.ok()) {
    return status;
  }
  status = CheckWriteCapability(plan, "选择");
  if (!status.ok()) {
    return status;
  }
  if (plan.capabilities.has_value() &&
      (!plan.capabilities->supportsSelect ||
       !plan.capabilities->supportsCancel)) {
    return Failed("IEC61850 SGCB在线能力未协商选择或取消");
  }
  MmsSettingGroupStatus current;
  status = ReadStatus(plan, &current);
  if (!status.ok()) {
    return status;
  }
  if (group > current.numberOfGroups) {
    return Invalid("IEC61850 SGCB选择组号超过远端组数");
  }
  // AR502H选择前清除旧编辑状态，避免远端拒绝新的EditSG。
  status = WriteBoolean(plan.confirmEdit, false, CapabilityTimeout(plan));
  if (!status.ok()) {
    return status;
  }
  status = WriteInteger(plan.editGroup, group, CapabilityTimeout(plan));
  if (status.ok()) {
    return status;
  }
  const auto rollbackStatus = Rollback(plan, current, true);
  if (!rollbackStatus.ok()) {
    return rollbackStatus;
  }
  return status;
}

grpc::Status MmsSettingGroupClient::ConfirmEdit(
    const MmsSettingGroupPlan& plan) const {
  auto status = ValidatePlan(plan);
  if (!status.ok()) {
    return status;
  }
  status = CheckWriteCapability(plan, "确认");
  if (!status.ok()) {
    return status;
  }
  if (plan.capabilities.has_value() && !plan.capabilities->supportsConfirm) {
    return Failed("IEC61850 SGCB在线能力未协商确认");
  }
  return WriteBoolean(plan.confirmEdit, true, CapabilityTimeout(plan));
}

grpc::Status MmsSettingGroupClient::CancelEdit(
    const MmsSettingGroupPlan& plan) const {
  auto status = ValidatePlan(plan);
  if (!status.ok()) {
    return status;
  }
  status = CheckWriteCapability(plan, "取消");
  if (!status.ok()) {
    return status;
  }
  if (plan.capabilities.has_value() && !plan.capabilities->supportsCancel) {
    return Failed("IEC61850 SGCB在线能力未协商取消");
  }
  return WriteBoolean(plan.confirmEdit, false, CapabilityTimeout(plan));
}

grpc::Status MmsSettingGroupClient::Activate(
    const MmsSettingGroupPlan& plan, std::uint32_t group) const {
  auto status = ValidateGroup(plan, group);
  if (!status.ok()) {
    return status;
  }
  status = CheckWriteCapability(plan, "激活");
  if (!status.ok()) {
    return status;
  }
  if (plan.capabilities.has_value() &&
      (!plan.capabilities->supportsActivate ||
       !plan.capabilities->supportsConfirm)) {
    return Failed("IEC61850 SGCB在线能力未协商激活或确认");
  }
  MmsSettingGroupStatus current;
  status = ReadStatus(plan, &current);
  if (!status.ok()) {
    return status;
  }
  if (group > current.numberOfGroups || current.editGroup != group) {
    return Failed("IEC61850 SGCB激活组号不是当前编辑组");
  }
  status = WriteBoolean(plan.confirmEdit, true, CapabilityTimeout(plan));
  if (!status.ok()) {
    return status;
  }
  status = WriteInteger(plan.activeGroup, group, CapabilityTimeout(plan));
  if (status.ok()) {
    return status;
  }
  const auto rollbackStatus = Rollback(plan, current, false);
  if (!rollbackStatus.ok()) {
    return rollbackStatus;
  }
  return status;
}

grpc::Status MmsSettingGroupClient::ReadValues(
    const MmsSettingGroupPlan& plan,
    std::span<const MmsObjectName> variables,
    MmsReadResponse* response) const {
  if (response == nullptr) {
    return Invalid("IEC61850 SGCB定值读取输出为空");
  }
  response->items.clear();
  auto status = ValidatePlan(plan);
  if (!status.ok()) {
    return status;
  }
  status = CheckReadCapability(plan);
  if (!status.ok()) {
    return status;
  }
  if (!read_) {
    return Failed("IEC61850 SGCB定值读取回调未配置");
  }
  if (variables.empty()) {
    return Invalid("IEC61850 SGCB定值读取对象不能为空");
  }
  MmsReadRequest request;
  request.variables.assign(variables.begin(), variables.end());
  for (const auto& variable : variables) {
    if (!ValidObject(variable)) {
      return Invalid("IEC61850 SGCB定值对象引用无效");
    }
  }
  const auto started = std::chrono::steady_clock::now();
  status = read_(request, response);
  status = CheckElapsed(started, CapabilityTimeout(plan), status, "定值读取",
                        false);
  if (!status.ok()) {
    response->items.clear();
    return status;
  }
  if (response->items.size() != variables.size()) {
    response->items.clear();
    return grpc::Status(grpc::StatusCode::DATA_LOSS,
                        "IEC61850 SGCB定值读取结果数量不匹配");
  }
  for (const auto& item : response->items) {
    if (!item.success) {
      return Failed("IEC61850 SGCB定值读取包含失败项");
    }
  }
  LOG_INFO("IEC61850 SGCB定值读取完成: 数量={}", variables.size());
  return grpc::Status::OK;
}

grpc::Status MmsSettingGroupClient::WriteValues(
    const MmsSettingGroupPlan& plan,
    std::span<const MmsWriteRequestItem> variables,
    MmsWriteResponse* response) const {
  if (response == nullptr) {
    return Invalid("IEC61850 SGCB定值写入输出为空");
  }
  response->items.clear();
  auto status = ValidatePlan(plan);
  if (!status.ok()) {
    return status;
  }
  status = CheckWriteCapability(plan, "定值");
  if (!status.ok()) {
    return status;
  }
  if (!write_) {
    return Failed("IEC61850 SGCB定值写入回调未配置");
  }
  if (variables.empty()) {
    return Invalid("IEC61850 SGCB定值写入对象不能为空");
  }
  std::vector<MmsObjectName> names;
  names.reserve(variables.size());
  for (const auto& variable : variables) {
    if (!ValidObject(variable.variable) || variable.encodedData.empty()) {
      return Invalid("IEC61850 SGCB定值对象或数据无效");
    }
    names.push_back(variable.variable);
  }

  // 写入前读取快照，保证服务端返回部分失败或传输不确定时可以恢复。
  MmsReadResponse previous;
  status = ReadValues(plan, names, &previous);
  if (!status.ok()) {
    return status;
  }
  MmsWriteRequest request;
  request.items.assign(variables.begin(), variables.end());
  const auto started = std::chrono::steady_clock::now();
  status = write_(request, response);
  status = CheckElapsed(started, CapabilityTimeout(plan), status, "定值写入",
                        true);
  if (status.ok() && response->items.size() == variables.size()) {
    bool allSucceeded = true;
    for (const auto& item : response->items) {
      allSucceeded = allSucceeded && item.success;
    }
    if (allSucceeded) {
      LOG_INFO("IEC61850 SGCB定值写入完成: 数量={}", variables.size());
      return grpc::Status::OK;
    }
    status = Failed("IEC61850 SGCB定值写入包含失败项");
  } else if (status.ok()) {
    status = grpc::Status(grpc::StatusCode::DATA_LOSS,
                          "IEC61850 SGCB定值写入结果数量不匹配");
  }

  // 任何失败都先恢复快照；恢复失败必须阻止调用方继续使用旧状态假设。
  MmsWriteRequest rollback;
  rollback.items.reserve(previous.items.size());
  for (std::size_t index = 0; index < previous.items.size(); ++index) {
    MmsWriteRequestItem& item = rollback.items.emplace_back();
    item.variable = names[index];
    item.encodedData = previous.items[index].encodedData;
  }
  MmsWriteResponse rollbackResponse;
  const auto rollbackStarted = std::chrono::steady_clock::now();
  auto rollbackStatus = write_(rollback, &rollbackResponse);
  rollbackStatus = CheckElapsed(rollbackStarted, CapabilityTimeout(plan),
                                rollbackStatus, "定值回滚", true);
  if (!rollbackStatus.ok() ||
      rollbackResponse.items.size() != rollback.items.size()) {
    LOG_ERROR("IEC61850 SGCB定值回滚失败，状态不确定: 数量={}",
              rollback.items.size());
    response->items.clear();
    return Aborted("IEC61850 SGCB定值回滚失败，远端状态不确定");
  }
  for (const auto& item : rollbackResponse.items) {
    if (!item.success) {
      LOG_ERROR("IEC61850 SGCB定值回滚包含失败项，状态不确定");
      response->items.clear();
      return Aborted("IEC61850 SGCB定值回滚包含失败项，远端状态不确定");
    }
  }
  return status;
}

grpc::Status MmsSettingGroupClient::Rollback(
    const MmsSettingGroupPlan& plan, const MmsSettingGroupStatus& previous,
    bool restoreEdit) const {
  auto status = WriteBoolean(plan.confirmEdit, previous.confirmEdit,
                             CapabilityTimeout(plan));
  if (!status.ok()) {
    LOG_ERROR("IEC61850 SGCB回滚CnfEdit失败，状态不确定: 对象={}",
              plan.confirmEdit.identifier);
    return Aborted("IEC61850 SGCB回滚CnfEdit失败，远端状态不确定");
  }
  if (restoreEdit && previous.editGroup != 0) {
    status = WriteInteger(plan.editGroup, previous.editGroup,
                          CapabilityTimeout(plan));
    if (!status.ok()) {
      LOG_ERROR("IEC61850 SGCB回滚EditSG失败，状态不确定: 对象={}",
                plan.editGroup.identifier);
      return Aborted("IEC61850 SGCB回滚EditSG失败，远端状态不确定");
    }
  }
  return grpc::Status::OK;
}

}  // namespace IEC61850
