#include "IEC61850MmsControl.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "IEC61850MmsBer.h"
#include "IEC61850MmsSession.h"
#include "IEC61850ProtocolStack.h"

namespace IEC61850 {
namespace {

constexpr std::size_t kMaxMmsControlBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxOriginIdentifierBytes = 64;
constexpr std::uint32_t kMaxControlTimeoutMs = 10 * 60 * 1000;
constexpr std::size_t kMaxSboSelections = 256;

grpc::Status ArgumentError(std::string_view reason) {
  return grpc::Status(
      grpc::StatusCode::INVALID_ARGUMENT,
      std::format("IEC61850 MMS控制参数无效: {}", reason));
}

grpc::Status OutputError(std::string_view reason) {
  return grpc::Status(
      grpc::StatusCode::RESOURCE_EXHAUSTED,
      std::format("IEC61850 MMS控制输出超过下位机上限: {}", reason));
}

grpc::Status PreconditionError(std::string_view reason) {
  return grpc::Status(
      grpc::StatusCode::FAILED_PRECONDITION,
      std::format("IEC61850 MMS控制前置条件不满足: {}", reason));
}

bool SameObject(const MmsObjectName& left,
                const MmsObjectName& right) noexcept {
  return left.type == right.type && left.domain == right.domain &&
         left.identifier == right.identifier;
}

std::string ObjectKey(const MmsObjectName& object) {
  return std::format("{}\x1f{}\x1f{}", static_cast<int>(object.type),
                     object.domain, object.identifier);
}

bool IsMmsDataTag(std::uint8_t tag) noexcept {
  switch (tag) {
    case 0xa1:  // array
    case 0xa2:  // structure
    case 0x83:  // boolean
    case 0x84:  // bit-string
    case 0x85:  // integer
    case 0x86:  // unsigned
    case 0x87:  // floating-point
    case 0x89:  // octet-string
    case 0x8a:  // visible-string
    case 0x8b:  // generalized-time
    case 0x8c:  // binary-time
    case 0x8d:  // bcd
    case 0x8e:  // object-identifier
    case 0x8f:  // MMS string
    case 0x90:  // UTF-8 string
    case 0x91:  // UTC time
      return true;
    default:
      return false;
  }
}

bool MatchesMmsDataType(std::uint8_t tag,
                        const MmsTypeSpecification& type) noexcept {
  switch (type.kind) {
    case MmsTypeSpecificationKind::BOOLEAN:
      return tag == 0x83;
    case MmsTypeSpecificationKind::BIT_STRING:
      return tag == 0x84;
    case MmsTypeSpecificationKind::INTEGER:
      return tag == 0x85;
    case MmsTypeSpecificationKind::UNSIGNED:
      return tag == 0x86;
    case MmsTypeSpecificationKind::FLOATING_POINT:
      return tag == 0x87;
    case MmsTypeSpecificationKind::OCTET_STRING:
      return tag == 0x89;
    case MmsTypeSpecificationKind::VISIBLE_STRING:
      return tag == 0x8a;
    case MmsTypeSpecificationKind::BINARY_TIME:
      return tag == 0x8c;
    case MmsTypeSpecificationKind::UTF8_STRING:
      return tag == 0x90;
    case MmsTypeSpecificationKind::UTC_TIME:
      return tag == 0x91;
    case MmsTypeSpecificationKind::ARRAY:
      return tag == 0xa1;
    case MmsTypeSpecificationKind::STRUCTURE:
      return tag == 0xa2;
    case MmsTypeSpecificationKind::NAMED_TYPE:
    case MmsTypeSpecificationKind::GENERAL_TIME:
    case MmsTypeSpecificationKind::OBJECT_IDENTIFIER:
    case MmsTypeSpecificationKind::MMS_STRING:
    case MmsTypeSpecificationKind::BCD:
      return false;
  }
  return false;
}

grpc::Status DecodeOnlineInteger(
    std::span<const std::uint8_t> encoded,
    MmsTypeSpecificationKind expectedKind, std::int64_t* value) {
  if (value == nullptr || encoded.empty()) {
    return grpc::Status(grpc::StatusCode::DATA_LOSS,
                        "IEC61850在线控制参数缺少MMS Data值");
  }
  std::size_t offset = 0;
  BerTlvView data;
  auto status = ReadBerTlv(encoded, &offset, &data);
  if (!status.ok() || offset != encoded.size()) {
    return grpc::Status(grpc::StatusCode::DATA_LOSS,
                        "IEC61850在线控制参数MMS Data结构无效");
  }
  const auto expectedTag = expectedKind == MmsTypeSpecificationKind::INTEGER
                               ? std::uint8_t{0x85}
                               : std::uint8_t{0x86};
  if (data.tag != expectedTag) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        "IEC61850在线控制参数编码标签与TypeSpecification不一致");
  }
  if (data.tag == 0x85) {
    status = ReadBerSigned(data.value, value);
  } else if (data.tag == 0x86) {
    std::uint64_t unsignedValue = 0;
    status = ReadBerUnsigned(data.value, &unsignedValue);
    if (status.ok() &&
        unsignedValue > static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max())) {
      status = grpc::Status(grpc::StatusCode::OUT_OF_RANGE,
                            "IEC61850在线控制参数无符号值溢出");
    }
    if (status.ok()) {
      *value = static_cast<std::int64_t>(unsignedValue);
    }
  } else {
    status = grpc::Status(grpc::StatusCode::DATA_LOSS,
                          "IEC61850在线控制参数不是整数Data");
  }
  if (!status.ok()) {
    return grpc::Status(status.error_code(),
                        std::format("IEC61850在线控制参数整数解码失败: {}",
                                    status.error_message()));
  }
  return grpc::Status::OK;
}

bool IsControlParameter(std::string_view dataRef,
                        std::string_view suffix) noexcept {
  return dataRef.size() > suffix.size() && dataRef.ends_with(suffix);
}

grpc::Status AppendTlv(std::uint8_t tag,
                       std::span<const std::uint8_t> value,
                       std::vector<std::uint8_t>* output) {
  if (output == nullptr) {
    return ArgumentError("BER字段输出参数为空");
  }
  output->clear();
  if (value.size() > kMaxMmsControlBytes) {
    return OutputError("BER字段长度过大");
  }
  output->assign(value.size() + sizeof(std::size_t) + 2, 0);
  BerWriter writer(*output);
  if (!writer.Tlv(tag, value)) {
    output->clear();
    return OutputError("BER字段编码失败");
  }
  output->resize(writer.size());
  return grpc::Status::OK;
}

grpc::Status ValidateControlObject(const MmsObjectName& object) {
  if (object.type != MmsObjectNameType::DOMAIN_SPECIFIC ||
      object.domain.empty() || object.identifier.empty() ||
      object.domain.size() > 1024 || object.identifier.size() > 1024 ||
      object.identifier.back() == '$' ||
      object.identifier.find('/') != std::string::npos) {
    return ArgumentError(
        "控制对象必须是有效的Domain-specific对象引用");
  }
  return grpc::Status::OK;
}

grpc::Status BuildMemberObject(const MmsObjectName& base,
                               std::string_view suffix,
                               MmsObjectName* result) {
  if (result == nullptr) {
    return ArgumentError("控制对象输出参数为空");
  }
  auto status = ValidateControlObject(base);
  if (!status.ok()) {
    return status;
  }
  if (suffix.empty() || suffix.front() != '$') {
    return ArgumentError("控制对象成员后缀无效");
  }
  *result = base;
  result->identifier.append(suffix);
  return grpc::Status::OK;
}

grpc::Status ValidateControlValue(
    std::span<const std::uint8_t> encodedValue) {
  if (encodedValue.empty()) {
    return ArgumentError("控制命令缺少ctlVal");
  }
  if (encodedValue.size() > kMaxMmsControlBytes) {
    return OutputError("ctlVal长度过大");
  }
  std::size_t offset = 0;
  BerTlvView tlv;
  auto status = ReadBerTlv(encodedValue, &offset, &tlv);
  if (!status.ok() || offset != encodedValue.size() ||
      !IsMmsDataTag(tlv.tag)) {
    return ArgumentError("ctlVal必须是完整单个MMS Data选择");
  }
  return grpc::Status::OK;
}

grpc::Status EncodeOrigin(const MmsControlCommand& command,
                          std::vector<std::uint8_t>* encodedOrigin) {
  if (encodedOrigin == nullptr) {
    return ArgumentError("origin输出参数为空");
  }
  encodedOrigin->clear();
  if (command.originCategory > 8) {
    return ArgumentError("origin类别超出IEC 61850范围");
  }
  if (command.originIdentifier.size() > kMaxOriginIdentifierBytes) {
    return ArgumentError("origin标识超过64字节");
  }

  std::vector<std::uint8_t> category;
  auto status = EncodeMmsDataSigned(
      static_cast<std::int64_t>(command.originCategory), &category);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> identifier;
  status = AppendTlv(
      0x89,
      std::span<const std::uint8_t>(command.originIdentifier.data(),
                                    command.originIdentifier.size()),
      &identifier);
  if (!status.ok()) {
    return status;
  }

  std::vector<std::uint8_t> content;
  content.reserve(category.size() + identifier.size());
  content.insert(content.end(), category.begin(), category.end());
  content.insert(content.end(), identifier.begin(), identifier.end());
  return AppendTlv(0xa2, content, encodedOrigin);
}

std::string_view OperationSuffix(MmsControlOperation operation) noexcept {
  switch (operation) {
    case MmsControlOperation::SELECT_WITH_VALUE:
      return "$SBOw";
    case MmsControlOperation::OPERATE:
      return "$Oper";
    case MmsControlOperation::CANCEL:
      return "$Cancel";
    case MmsControlOperation::SELECT:
      return {};
  }
  return {};
}

grpc::Status ValidateControlCommand(const MmsControlCommand& command) {
  if (command.operation == MmsControlOperation::SELECT) {
    return ArgumentError("普通SBO选择必须使用Read请求");
  }
  auto status = ValidateControlObject(command.controlObject);
  if (!status.ok()) {
    return status;
  }
  switch (command.operation) {
    case MmsControlOperation::SELECT_WITH_VALUE:
    case MmsControlOperation::OPERATE:
      status = ValidateControlValue(command.controlValue);
      if (!status.ok()) {
        return status;
      }
      break;
    case MmsControlOperation::CANCEL:
      if (!command.controlValue.empty()) {
        return ArgumentError("Cancel结构不能携带ctlVal");
      }
      break;
    case MmsControlOperation::SELECT:
      break;
  }
  if (command.timestampMs < 0 ||
      (command.operateTimestampMs.has_value() &&
       *command.operateTimestampMs < 0)) {
    return ArgumentError("控制时间戳不能为负数");
  }
  if (command.operateTimestampMs.has_value() &&
      command.operation != MmsControlOperation::OPERATE) {
    return ArgumentError("只有Oper结构可以携带operTm");
  }
  if (command.check > 3) {
    return ArgumentError("Check只能使用2位有效值");
  }
  if (OperationSuffix(command.operation).empty()) {
    return ArgumentError("控制操作类型未知");
  }
  return grpc::Status::OK;
}

}  // namespace

const MmsControlCapability* MmsControlModel::Find(
    const MmsObjectName& object) const noexcept {
  const auto it = std::ranges::find_if(
      controls, [&](const auto& capability) {
        return SameObject(capability.object, object);
      });
  return it == controls.end() ? nullptr : &*it;
}

grpc::Status BuildMmsControlModel(const ProtocolIedPlan& plan,
                                   const MmsOnlineDirectory& directory,
                                   MmsControlModel* model) {
  if (model == nullptr) {
    return ArgumentError("控制能力模型输出参数为空");
  }
  model->controls.clear();
  if (directory.iedName != plan.config.ied_name() ||
      directory.accessPoint != plan.config.access_point()) {
    return PreconditionError("控制目录的IED或AccessPoint不匹配");
  }

  std::unordered_set<std::string> controlObjectKeys;
  std::vector<MmsObjectName> expectedObjects;
  for (const auto& dataObject : plan.ied.data_objects()) {
    const auto prefix = dataObject.data_ref() + ".";
    const bool hasControlAttribute = std::ranges::any_of(
        plan.ied.data_attributes(), [&](const auto& attribute) {
          return attribute.fc() == IEC61850Proto::FUNCTIONAL_CONSTRAINT_CO &&
                 attribute.data_ref().starts_with(prefix);
        });
    if (!hasControlAttribute) {
      continue;
    }
    MmsObjectName object;
    auto status = ParseMmsDomainObjectReference(dataObject.data_ref(), &object);
    if (!status.ok()) {
      return status;
    }
    if (controlObjectKeys.emplace(ObjectKey(object)).second) {
      expectedObjects.emplace_back(std::move(object));
    }
  }

  // 没有FC=CO对象表示该IED没有协议级控制能力，不阻止普通采集会话。
  if (expectedObjects.empty()) {
    return grpc::Status::OK;
  }

  for (const auto& expectedAttribute : plan.ied.data_attributes()) {
    if (expectedAttribute.fc() !=
        IEC61850Proto::FUNCTIONAL_CONSTRAINT_CO) {
      continue;
    }
    const auto onlineAttribute = std::ranges::find_if(
        directory.dataAttributes, [&](const auto& actualAttribute) {
          return actualAttribute.dataRef == expectedAttribute.data_ref() &&
                 actualAttribute.fc == expectedAttribute.fc();
        });
    if (onlineAttribute == directory.dataAttributes.end()) {
      model->controls.clear();
      return PreconditionError(std::format(
          "在线目录缺少控制数据属性: {}", expectedAttribute.data_ref()));
    }
  }

  std::unordered_map<std::string, std::uint8_t> onlineMembers;
  constexpr std::uint8_t kSbo = 1u << 0;
  constexpr std::uint8_t kSboWithValue = 1u << 1;
  constexpr std::uint8_t kOperate = 1u << 2;
  constexpr std::uint8_t kCancel = 1u << 3;
  for (const auto& variable : directory.namedVariables) {
    if (variable.type != MmsObjectNameType::DOMAIN_SPECIFIC) {
      continue;
    }
    constexpr std::array<std::pair<std::string_view, std::uint8_t>, 4>
        suffixes{{{"$SBO", kSbo},
                  {"$SBOw", kSboWithValue},
                  {"$Oper", kOperate},
                  {"$Cancel", kCancel}}};
    for (const auto& [suffix, bit] : suffixes) {
      if (variable.identifier.size() > suffix.size() &&
          variable.identifier.ends_with(suffix)) {
        MmsObjectName base = variable;
        base.identifier.resize(base.identifier.size() - suffix.size());
        onlineMembers[ObjectKey(base)] = static_cast<std::uint8_t>(
            onlineMembers[ObjectKey(base)] | bit);
        break;
      }
    }
  }

  for (const auto& object : expectedObjects) {
    const auto online = onlineMembers.find(ObjectKey(object));
    const auto flags = online == onlineMembers.end() ? std::uint8_t{0}
                                                     : online->second;
    MmsControlCapability capability;
    capability.object = object;
    capability.supportsSbo = (flags & kSbo) != 0;
    capability.supportsSboWithValue = (flags & kSboWithValue) != 0;
    capability.supportsOperate = (flags & kOperate) != 0;
    capability.supportsCancel = (flags & kCancel) != 0;

    const auto readParameter = [&](
                                   std::string_view suffix,
                                   MmsTypeSpecificationKind expectedKind,
                                   std::optional<std::int64_t>* output)
        -> grpc::Status {
      if (output == nullptr) {
        return ArgumentError("在线控制参数输出为空");
      }
      output->reset();
      bool found = false;
      for (const auto& attribute : directory.dataAttributes) {
        if (attribute.fc != IEC61850Proto::FUNCTIONAL_CONSTRAINT_CF ||
            !IsControlParameter(attribute.dataRef, suffix)) {
          continue;
        }
        const auto baseReference = attribute.dataRef.substr(
            0, attribute.dataRef.size() - suffix.size());
        MmsObjectName baseObject;
        auto parameterStatus =
            ParseMmsDomainObjectReference(baseReference, &baseObject);
        if (!parameterStatus.ok()) {
          return parameterStatus;
        }
        if (!SameObject(baseObject, object)) {
          continue;
        }
        if (found) {
          return grpc::Status(
              grpc::StatusCode::DATA_LOSS,
              std::format("在线控制参数重复: {}/{}", object.domain,
                          object.identifier));
        }
        if (!attribute.typeSpecification.has_value() ||
            attribute.typeSpecification->kind != expectedKind) {
          return PreconditionError(std::format(
              "在线控制参数{}的TypeSpecification类型不符合要求: {}/{}",
              suffix, object.domain, object.identifier));
        }
        if (attribute.encodedValue.empty()) {
          return grpc::Status(
              grpc::StatusCode::DATA_LOSS,
              std::format("在线控制参数{}缺少已读取的MMS Data值: {}/{}",
                          suffix, object.domain, object.identifier));
        }
        std::int64_t value = 0;
        parameterStatus =
            DecodeOnlineInteger(attribute.encodedValue, expectedKind, &value);
        if (!parameterStatus.ok()) {
          return grpc::Status(
              parameterStatus.error_code(),
              std::format("在线控制参数{}解码失败: {}", suffix,
                          parameterStatus.error_message()));
        }
        *output = value;
        found = true;
      }
      return grpc::Status::OK;
    };

    std::optional<std::int64_t> ctlModel;
    std::optional<std::int64_t> sboTimeout;
    std::optional<std::int64_t> operTimeout;
    auto parameterStatus = readParameter(
        ".ctlModel", MmsTypeSpecificationKind::INTEGER, &ctlModel);
    if (!parameterStatus.ok()) {
      model->controls.clear();
      return parameterStatus;
    }
    parameterStatus = readParameter(
        ".sboTimeout", MmsTypeSpecificationKind::UNSIGNED, &sboTimeout);
    if (!parameterStatus.ok()) {
      model->controls.clear();
      return parameterStatus;
    }
    parameterStatus = readParameter(
        ".operTimeout", MmsTypeSpecificationKind::UNSIGNED, &operTimeout);
    if (!parameterStatus.ok()) {
      model->controls.clear();
      return parameterStatus;
    }
    if (ctlModel.has_value()) {
      if (*ctlModel < 0 || *ctlModel > 4) {
        model->controls.clear();
        return PreconditionError(std::format(
            "控制对象ctlModel超出IEC 61850范围: {}/{}", object.domain,
            object.identifier));
      }
      capability.ctlModel = ctlModel;
      switch (*ctlModel) {
        case 0:
          capability.supportsSbo = false;
          capability.supportsSboWithValue = false;
          capability.supportsOperate = false;
          capability.supportsCancel = false;
          break;
        case 1:
        case 3:
          if (!capability.supportsOperate) {
            model->controls.clear();
            return PreconditionError(std::format(
                "direct控制对象在线目录缺少$Oper: {}/{}", object.domain,
                object.identifier));
          }
          capability.supportsSbo = false;
          capability.supportsSboWithValue = false;
          capability.supportsCancel = false;
          break;
        case 2:
          if (!capability.supportsSbo) {
            model->controls.clear();
            return PreconditionError(std::format(
                "普通SBO控制对象在线目录缺少$SBO: {}/{}",
                object.domain, object.identifier));
          }
          if (!capability.supportsOperate) {
            model->controls.clear();
            return PreconditionError(std::format(
                "普通SBO控制对象在线目录缺少$Oper: {}/{}", object.domain,
                object.identifier));
          }
          capability.supportsSboWithValue = false;
          break;
        case 4:
          if (!capability.supportsSboWithValue) {
            model->controls.clear();
            return PreconditionError(std::format(
                "增强SBO控制对象在线目录缺少$SBOw: {}/{}", object.domain,
                object.identifier));
          }
          if (!capability.supportsOperate) {
            model->controls.clear();
            return PreconditionError(std::format(
                "增强SBO控制对象在线目录缺少$Oper: {}/{}", object.domain,
                object.identifier));
          }
          capability.supportsSbo = false;
          break;
      }
    } else {
      if (online == onlineMembers.end()) {
        model->controls.clear();
        return PreconditionError(std::format(
            "在线NameList缺少控制对象成员: {}/{}", object.domain,
            object.identifier));
      }
      if (!capability.supportsOperate) {
        model->controls.clear();
        return PreconditionError(std::format(
            "在线NameList缺少控制对象Oper成员: {}/{}", object.domain,
            object.identifier));
      }
    }
    const auto assignTimeout = [&](std::optional<std::int64_t> value,
                                   std::optional<std::uint32_t>* target,
                                   std::string_view name) -> grpc::Status {
      if (!value.has_value()) {
        return grpc::Status::OK;
      }
      if (*value <= 0 ||
          static_cast<std::uint64_t>(*value) > kMaxControlTimeoutMs) {
        return PreconditionError(std::format(
            "控制对象{}超出下位机时间范围: {}/{}", name, object.domain,
            object.identifier));
      }
      *target = static_cast<std::uint32_t>(*value);
      return grpc::Status::OK;
    };
    parameterStatus = assignTimeout(sboTimeout, &capability.sboTimeoutMs,
                                    "sboTimeout");
    if (!parameterStatus.ok()) {
      model->controls.clear();
      return parameterStatus;
    }
    parameterStatus = assignTimeout(operTimeout, &capability.operTimeoutMs,
                                    "operTimeout");
    if (!parameterStatus.ok()) {
      model->controls.clear();
      return parameterStatus;
    }
    for (const auto& attribute : plan.ied.data_attributes()) {
      if (attribute.fc() != IEC61850Proto::FUNCTIONAL_CONSTRAINT_CO ||
          !attribute.data_ref().ends_with(".ctlVal")) {
        continue;
      }
      MmsObjectName ctlValObject;
      auto status = ParseMmsDomainObjectReference(
          attribute.data_ref().substr(
              0, attribute.data_ref().size() - std::string_view(".ctlVal").size()),
          &ctlValObject);
      if (!status.ok() || !SameObject(ctlValObject, object)) {
        continue;
      }
      const auto onlineAttribute = std::ranges::find_if(
          directory.dataAttributes, [&](const auto& actualAttribute) {
            return actualAttribute.dataRef == attribute.data_ref() &&
                   actualAttribute.fc == attribute.fc();
          });
      if (onlineAttribute != directory.dataAttributes.end() &&
          onlineAttribute->typeSpecification.has_value()) {
        capability.ctlValType = onlineAttribute->typeSpecification;
      }
      break;
    }
    model->controls.emplace_back(std::move(capability));
  }
  return grpc::Status::OK;
}

MmsSboState::MmsSboState(std::chrono::milliseconds holdTime)
    : holdTime_(holdTime) {}

grpc::Status MmsSboState::RecordSelection(const MmsObjectName& object,
                                          std::int64_t nowMs,
                                          std::optional<std::chrono::milliseconds>
                                              holdTime) {
  if (object.type != MmsObjectNameType::DOMAIN_SPECIFIC ||
      object.domain.empty() || object.identifier.empty()) {
    return ArgumentError("SBO选择对象必须是有效的Domain-specific对象");
  }
  const auto holdMs = holdTime.value_or(holdTime_).count();
  if (holdMs <= 0 || nowMs > std::numeric_limits<std::int64_t>::max() - holdMs) {
    return ArgumentError("SBO保持时间或当前时间无效");
  }
  std::lock_guard lock(mutex_);
  if (uncertaintyOverflow_) {
    return PreconditionError("控制状态容量已耗尽，需重建MMS会话");
  }
  auto selection = std::ranges::find_if(
      selections_, [&](const auto& current) {
        return SameObject(current.object, object);
      });
  if (selection == selections_.end()) {
    if (selections_.size() >= kMaxSboSelections) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "IEC61850 MMS SBO选择保持数量超过下位机上限");
    }
    selection = selections_.emplace(selections_.end());
    selection->object = object;
  }
  selection->expiresAtMs = nowMs + holdMs;
  return grpc::Status::OK;
}

bool MmsSboState::IsSelected(const MmsObjectName& object,
                             std::int64_t nowMs) const {
  std::lock_guard lock(mutex_);
  const auto selection = std::ranges::find_if(
      selections_, [&](const auto& current) {
        return SameObject(current.object, object);
      });
  return selection != selections_.end() && nowMs < selection->expiresAtMs;
}

bool MmsSboState::IsControlUncertain(const MmsObjectName& object) const {
  std::lock_guard lock(mutex_);
  if (uncertaintyOverflow_) {
    return true;
  }
  const auto execution = std::ranges::find_if(
      executions_, [&](const auto& current) {
        return SameObject(current.object, object);
      });
  return execution != executions_.end() && execution->uncertain;
}

bool MmsSboState::IsOperationPending(const MmsObjectName& object) const {
  std::lock_guard lock(mutex_);
  const auto execution = std::ranges::find_if(
      executions_, [&](const auto& current) {
        return SameObject(current.object, object);
      });
  return execution != executions_.end() && execution->pending;
}

bool MmsSboState::IsCancelRequired(const MmsObjectName& object) const {
  std::lock_guard lock(mutex_);
  const auto execution = std::ranges::find_if(
      executions_, [&](const auto& current) {
        return SameObject(current.object, object);
      });
  return execution != executions_.end() && execution->cancelRequired;
}

grpc::Status MmsSboState::RecordPendingOperation(
    const MmsObjectName& object) {
  if (object.type != MmsObjectNameType::DOMAIN_SPECIFIC ||
      object.domain.empty() || object.identifier.empty()) {
    return ArgumentError("待定时Oper对象必须是有效的Domain-specific对象");
  }
  std::lock_guard lock(mutex_);
  if (uncertaintyOverflow_) {
    return PreconditionError("控制状态容量已耗尽，需重建MMS会话");
  }
  auto execution = std::ranges::find_if(
      executions_, [&](const auto& current) {
        return SameObject(current.object, object);
      });
  if (execution == executions_.end()) {
    if (executions_.size() >= kMaxSboSelections) {
      return grpc::Status(
          grpc::StatusCode::RESOURCE_EXHAUSTED,
          "IEC61850 MMS待完成控制数量超过下位机上限");
    }
    execution = executions_.emplace(executions_.end());
    execution->object = object;
  }
  if (execution->uncertain) {
    return PreconditionError("控制对象结果不确定，不能建立执行占用");
  }
  if (execution->pending || execution->cancelRequired) {
    return PreconditionError("控制对象已有执行中或待Cancel的Oper");
  }
  execution->pending = true;
  return grpc::Status::OK;
}

grpc::Status MmsSboState::MarkOperationRejected(
    const MmsObjectName& object) {
  if (object.type != MmsObjectNameType::DOMAIN_SPECIFIC ||
      object.domain.empty() || object.identifier.empty()) {
    return ArgumentError("远端拒绝对象必须是有效的Domain-specific对象");
  }
  std::lock_guard lock(mutex_);
  const auto execution = std::ranges::find_if(
      executions_, [&](const auto& current) {
        return SameObject(current.object, object);
      });
  if (execution == executions_.end() || !execution->pending) {
    return PreconditionError("控制对象没有可转换为待Cancel的执行占用");
  }
  execution->pending = false;
  execution->cancelRequired = true;
  return grpc::Status::OK;
}

void MmsSboState::ClearPendingOperation(const MmsObjectName& object) {
  std::lock_guard lock(mutex_);
  std::erase_if(executions_, [&](const auto& current) {
    return SameObject(current.object, object);
  });
}

grpc::Status MmsSboState::MarkUncertain(const MmsObjectName& object) {
  if (object.type != MmsObjectNameType::DOMAIN_SPECIFIC ||
      object.domain.empty() || object.identifier.empty()) {
    return ArgumentError("结果不确定对象必须是有效的Domain-specific对象");
  }
  std::lock_guard lock(mutex_);
  auto execution = std::ranges::find_if(
      executions_, [&](const auto& current) {
        return SameObject(current.object, object);
      });
  if (execution == executions_.end()) {
    if (executions_.size() >= kMaxSboSelections) {
      uncertaintyOverflow_ = true;
      return grpc::Status(
          grpc::StatusCode::RESOURCE_EXHAUSTED,
          "IEC61850 MMS结果不确定控制数量超过下位机上限");
    }
    execution = executions_.emplace(executions_.end());
    execution->object = object;
  }
  execution->pending = false;
  execution->cancelRequired = false;
  execution->uncertain = true;
  return grpc::Status::OK;
}

void MmsSboState::ClearSelection(const MmsObjectName& object) {
  std::lock_guard lock(mutex_);
  std::erase_if(selections_, [&](const auto& current) {
    return SameObject(current.object, object);
  });
  std::erase_if(executions_, [&](const auto& current) {
    return SameObject(current.object, object);
  });
}

void MmsSboState::Clear() {
  std::lock_guard lock(mutex_);
  selections_.clear();
  executions_.clear();
  uncertaintyOverflow_ = false;
}

grpc::Status ValidateMmsControlOperation(
    const MmsControlModel& model, const MmsObjectName& object,
    MmsControlOperation operation, const MmsSboState& sboState,
    std::int64_t nowMs) {
  const auto* capability = model.Find(object);
  if (capability == nullptr) {
    return PreconditionError("控制对象不在在线能力模型中");
  }
  if (sboState.IsControlUncertain(object)) {
    return PreconditionError("控制对象上一次请求结果不确定，需重建MMS会话");
  }
  if (sboState.IsCancelRequired(object) &&
      operation != MmsControlOperation::CANCEL) {
    return PreconditionError("控制对象上一次Oper已被远端拒绝，必须先Cancel");
  }
  if (sboState.IsOperationPending(object) &&
      operation != MmsControlOperation::CANCEL) {
    return PreconditionError("控制对象存在尚未完成的Oper");
  }
  const bool requiresSelection =
      capability->supportsSbo || capability->supportsSboWithValue;
  switch (operation) {
    case MmsControlOperation::SELECT:
      if (!capability->supportsSbo) {
        return PreconditionError("控制对象不支持普通SBO选择");
      }
      return grpc::Status::OK;
    case MmsControlOperation::SELECT_WITH_VALUE:
      if (!capability->supportsSboWithValue) {
        return PreconditionError("控制对象不支持带值SBO选择");
      }
      return grpc::Status::OK;
    case MmsControlOperation::OPERATE:
      if (!capability->supportsOperate) {
        return PreconditionError("控制对象不支持Oper操作");
      }
      if (requiresSelection && !sboState.IsSelected(object, nowMs)) {
        return PreconditionError("控制对象没有有效的SBO选择保持");
      }
      return grpc::Status::OK;
    case MmsControlOperation::CANCEL:
      if (!capability->supportsCancel) {
        return PreconditionError("控制对象不支持Cancel操作");
      }
      if (requiresSelection && !sboState.IsSelected(object, nowMs)) {
        return PreconditionError("控制对象没有可取消的SBO选择保持");
      }
      return grpc::Status::OK;
  }
  return ArgumentError("未知MMS控制操作");
}

std::chrono::milliseconds ResolveMmsControlTimeout(
    const MmsControlCapability& capability, MmsControlOperation operation,
    std::chrono::milliseconds fallback) noexcept {
  const auto choose = [](const std::optional<std::uint32_t>& value,
                         std::chrono::milliseconds defaultValue) noexcept {
    if (!value.has_value() || *value == 0) {
      return defaultValue;
    }
    return std::chrono::milliseconds(*value);
  };
  switch (operation) {
    case MmsControlOperation::SELECT_WITH_VALUE:
      return choose(capability.sboTimeoutMs, fallback);
    case MmsControlOperation::OPERATE:
    case MmsControlOperation::CANCEL:
      return choose(capability.operTimeoutMs, fallback);
    case MmsControlOperation::SELECT:
      return fallback;
  }
  return fallback;
}

grpc::Status ValidateMmsControlValue(
    const MmsControlModel& model, const MmsObjectName& object,
    std::span<const std::uint8_t> encodedValue) {
  const auto* capability = model.Find(object);
  if (capability == nullptr) {
    return PreconditionError("控制对象不在在线能力模型中");
  }
  if (!capability->ctlValType.has_value()) {
    return PreconditionError("在线控制对象缺少ctlVal类型描述");
  }
  if (encodedValue.empty()) {
    return ArgumentError("控制命令缺少ctlVal");
  }
  std::size_t offset = 0;
  BerTlvView data;
  auto status = ReadBerTlv(encodedValue, &offset, &data);
  if (!status.ok() || offset != encodedValue.size() ||
      !MatchesMmsDataType(data.tag, *capability->ctlValType)) {
    return PreconditionError("控制命令ctlVal与在线TypeSpecification不匹配");
  }
  return grpc::Status::OK;
}

grpc::Status EncodeMmsPointControlValue(
    const MmsPointControlCommand& command,
    const MmsControlCapability& capability,
    std::vector<std::uint8_t>* encodedValue) {
  if (encodedValue == nullptr) {
    return ArgumentError("DataCenter控制值输出参数为空");
  }
  encodedValue->clear();
  if (!capability.ctlValType.has_value()) {
    return PreconditionError("在线控制对象缺少ctlVal类型描述");
  }
  if (!std::isfinite(command.scale) || !std::isfinite(command.offset)) {
    return ArgumentError("控制点scale或offset不是有限数");
  }
  const auto& type = *capability.ctlValType;
  switch (command.valueType) {
    case IEC61850Proto::POINT_VALUE_TYPE_BOOL:
      if (type.kind != MmsTypeSpecificationKind::BOOLEAN) {
        return PreconditionError("DataCenter控制值类型与在线ctlVal不匹配");
      }
      return EncodeMmsDataBoolean(command.boolValue, encodedValue);

    case IEC61850Proto::POINT_VALUE_TYPE_INT64: {
      const long double scale = command.scale == 0.0 ? 1.0 : command.scale;
      const long double raw =
          (static_cast<long double>(command.intValue) -
           static_cast<long double>(command.offset)) /
          scale;
      if (!std::isfinite(raw) ||
          std::round(raw) != raw) {
        return ArgumentError("整数控制值反向换算后不是有限整数");
      }
      if (type.kind == MmsTypeSpecificationKind::INTEGER) {
        if (raw < -0x1p63L || raw >= 0x1p63L) {
          return ArgumentError("整数控制值超出MMS范围");
        }
        return EncodeMmsDataSigned(static_cast<std::int64_t>(raw),
                                   encodedValue);
      }
      if (type.kind == MmsTypeSpecificationKind::UNSIGNED) {
        if (raw < 0.0L || raw >= 0x1p64L) {
          return ArgumentError("无符号控制值超出MMS范围");
        }
        return EncodeMmsDataUnsigned(static_cast<std::uint64_t>(raw),
                                     encodedValue);
      }
      return PreconditionError("DataCenter整数控制值与在线ctlVal不匹配");
    }

    case IEC61850Proto::POINT_VALUE_TYPE_DOUBLE: {
      if (type.kind != MmsTypeSpecificationKind::FLOATING_POINT) {
        return PreconditionError("DataCenter浮点控制值与在线ctlVal不匹配");
      }
      const long double scale = command.scale == 0.0 ? 1.0 : command.scale;
      const long double raw =
          (static_cast<long double>(command.doubleValue) -
           static_cast<long double>(command.offset)) /
          scale;
      const double converted = static_cast<double>(raw);
      if (!std::isfinite(raw) || !std::isfinite(converted)) {
        return ArgumentError("浮点控制值反向换算后不是有限数");
      }
      std::uint8_t formatWidth = 0;
      if (type.width == 32 || type.width == 0x08) {
        formatWidth = 0x08;
      } else if (type.width == 64 || type.width == 0x0b) {
        formatWidth = 0x0b;
      } else {
        return PreconditionError("在线FLOATING-POINT格式宽度不受支持");
      }
      return EncodeMmsDataFloatingPoint(converted, formatWidth,
                                        encodedValue);
    }

    default:
      return grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT,
          "DataCenter控制命令只支持BOOL、INTEGER和FLOATING-POINT标量");
  }
}

grpc::Status ValidateMmsControlSelectResponse(
    const MmsReadResponse& response, const MmsObjectName* expectedObject) {
  if (response.items.size() != 1 || !response.items.front().success) {
    return PreconditionError("普通SBO选择未返回单个成功结果");
  }
  const auto& encoded = response.items.front().encodedData;
  if (encoded.empty()) {
    return PreconditionError("普通SBO选择返回的VisibleString为空");
  }
  std::size_t offset = 0;
  BerTlvView data;
  auto status = ReadBerTlv(encoded, &offset, &data);
  if (!status.ok() || offset != encoded.size() || data.tag != 0x8a ||
      data.value.empty()) {
    return PreconditionError(
        "普通SBO选择返回值不是单个非空VisibleString");
  }
  for (const auto character : data.value) {
    if (character < 0x20 || character > 0x7e) {
      return PreconditionError("普通SBO选择返回的VisibleString包含非法字符");
    }
  }
  if (expectedObject == nullptr) {
    return grpc::Status::OK;
  }
  status = ValidateControlObject(*expectedObject);
  if (!status.ok()) {
    return status;
  }
  std::string expected = expectedObject->domain;
  expected.push_back('/');
  expected.append(expectedObject->identifier);
  std::replace(expected.begin(), expected.end(), '$', '.');
  const std::string actual(
      reinterpret_cast<const char*>(data.value.data()), data.value.size());
  if (actual != expected) {
    return PreconditionError(std::format(
        "普通SBO选择返回对象与请求对象不一致: 返回={}, 期望={}", actual,
        expected));
  }
  return grpc::Status::OK;
}

namespace {

grpc::Status ReadControlVisibleString(const BerTlvView& tlv,
                                      std::string* value) {
  if (value == nullptr || tlv.tag != 0x8a || tlv.value.empty() ||
      tlv.value.size() > 1024) {
    return PreconditionError("CommandTermination VisibleString无效");
  }
  for (const auto character : tlv.value) {
    if (character < 0x20 || character > 0x7e) {
      return PreconditionError("CommandTermination VisibleString包含非法字符");
    }
  }
  value->assign(reinterpret_cast<const char*>(tlv.value.data()),
                tlv.value.size());
  return grpc::Status::OK;
}

grpc::Status ReadControlSignedByte(const BerTlvView& tlv,
                                   std::int64_t* value,
                                   std::string_view fieldName) {
  if (value == nullptr || tlv.tag != 0x85) {
    return PreconditionError(
        std::format("CommandTermination {}类型错误", fieldName));
  }
  auto status = ReadBerSigned(tlv.value, value);
  if (!status.ok() || *value < 0 || *value > 127) {
    return PreconditionError(
        std::format("CommandTermination {}超出Byte范围", fieldName));
  }
  return grpc::Status::OK;
}

grpc::Status ReadControlUnsignedByte(const BerTlvView& tlv,
                                     std::uint8_t* value,
                                     std::string_view fieldName) {
  if (value == nullptr || tlv.tag != 0x86) {
    return PreconditionError(
        std::format("CommandTermination {}类型错误", fieldName));
  }
  std::uint64_t decoded = 0;
  auto status = ReadBerUnsigned(tlv.value, &decoded);
  if (!status.ok() || decoded > std::numeric_limits<std::uint8_t>::max()) {
    return PreconditionError(
        std::format("CommandTermination {}超出无符号Byte范围", fieldName));
  }
  *value = static_cast<std::uint8_t>(decoded);
  return grpc::Status::OK;
}

struct CommandTerminationVariable {
  MmsObjectName object;
  bool lastApplError = false;
};

grpc::Status DecodeLastApplError(
    const BerTlvView& encoded, const MmsObjectName& expectedOper,
    std::uint8_t expectedControlNumber, MmsLastApplError* result) {
  if (result == nullptr || encoded.tag != 0xa2) {
    return PreconditionError("CommandTermination缺少LastApplError结构");
  }
  MmsLastApplError decoded;
  std::size_t offset = 0;
  std::array<BerTlvView, 5> fields{};
  for (auto& field : fields) {
    auto status = ReadBerTlv(encoded.value, &offset, &field);
    if (!status.ok()) {
      return status;
    }
  }
  if (offset != encoded.value.size()) {
    return PreconditionError("LastApplError包含未知字段");
  }

  std::string controlReference;
  auto status = ReadControlVisibleString(fields[0], &controlReference);
  if (!status.ok()) {
    return status;
  }
  status = ParseMmsDomainObjectReference(controlReference,
                                         &decoded.controlObject);
  if (!status.ok() || !SameObject(decoded.controlObject, expectedOper)) {
    return PreconditionError(
        std::format("LastApplError控制对象与当前Oper不一致: 返回={}, 期望={}",
                    controlReference,
                    expectedOper.domain + "/" + expectedOper.identifier));
  }

  status = ReadControlSignedByte(fields[1], &decoded.error, "Error");
  if (!status.ok()) {
    return status;
  }
  if (fields[2].tag != 0xa2) {
    return PreconditionError("LastApplError Origin结构错误");
  }
  std::size_t originOffset = 0;
  BerTlvView originCategory;
  BerTlvView originIdentifier;
  status = ReadBerTlv(fields[2].value, &originOffset, &originCategory);
  if (!status.ok()) {
    return status;
  }
  status = ReadBerTlv(fields[2].value, &originOffset, &originIdentifier);
  if (!status.ok() || originOffset != fields[2].value.size()) {
    return PreconditionError("LastApplError Origin成员数量错误");
  }
  std::int64_t originCategoryValue = 0;
  status = ReadControlSignedByte(originCategory, &originCategoryValue,
                                 "Origin.orCat");
  if (!status.ok()) {
    return status;
  }
  decoded.originCategory = static_cast<std::uint8_t>(originCategoryValue);
  if (originIdentifier.tag != 0x89 || originIdentifier.value.size() > 64) {
    return PreconditionError("LastApplError Origin.orIdent类型或长度错误");
  }
  decoded.originIdentifier.assign(originIdentifier.value.begin(),
                                  originIdentifier.value.end());

  status = ReadControlUnsignedByte(fields[3], &decoded.controlNumber,
                                   "ctlNum");
  if (!status.ok()) {
    return status;
  }
  if (decoded.controlNumber != expectedControlNumber) {
    return PreconditionError("LastApplError ctlNum与当前Oper不一致");
  }
  status = ReadControlSignedByte(fields[4], &decoded.addCause, "AddCause");
  if (!status.ok()) {
    return status;
  }
  *result = std::move(decoded);
  return grpc::Status::OK;
}

}  // namespace

grpc::Status DecodeMmsCommandTermination(
    std::span<const std::uint8_t> input, const MmsObjectName& expectedOper,
    std::uint8_t expectedControlNumber, MmsCommandTermination* result) {
  if (result == nullptr) {
    return ArgumentError("CommandTermination输出参数为空");
  }
  *result = {};
  MmsCommandTermination decoded;
  if (input.empty() || input.size() > kMaxMmsControlBytes) {
    return PreconditionError("CommandTermination报文为空或超过长度上限");
  }
  std::size_t offset = 0;
  BerTlvView unconfirmed;
  auto status = ReadBerTlv(input, &offset, &unconfirmed);
  if (!status.ok() || offset != input.size() || unconfirmed.tag != 0xa3) {
    return PreconditionError("CommandTermination缺少Unconfirmed-PDU[3]");
  }
  std::size_t unconfirmedOffset = 0;
  BerTlvView report;
  status = ReadBerTlv(unconfirmed.value, &unconfirmedOffset, &report);
  if (!status.ok() || unconfirmedOffset != unconfirmed.value.size() ||
      report.tag != 0xa0) {
    return PreconditionError("CommandTermination缺少InformationReport[0]");
  }
  std::size_t reportOffset = 0;
  BerTlvView variableAccess;
  BerTlvView accessList;
  status = ReadBerTlv(report.value, &reportOffset, &variableAccess);
  if (!status.ok() || variableAccess.tag != 0xa0) {
    return PreconditionError("CommandTermination缺少listOfVariable[0]");
  }
  status = ReadBerTlv(report.value, &reportOffset, &accessList);
  if (!status.ok() || reportOffset != report.value.size() ||
      accessList.tag != 0xa1) {
    return PreconditionError("CommandTermination缺少listOfAccessResult[1]");
  }

  std::vector<CommandTerminationVariable> variables;
  std::size_t variableOffset = 0;
  while (variableOffset < variableAccess.value.size()) {
    if (variables.size() >= 2) {
      return PreconditionError("CommandTermination变量数量超过两个");
    }
    BerTlvView variable;
    status = ReadBerTlv(variableAccess.value, &variableOffset, &variable);
    if (!status.ok() || variable.tag != 0x30) {
      return PreconditionError("CommandTermination变量项不是SEQUENCE");
    }
    std::size_t itemOffset = 0;
    BerTlvView specification;
    status = ReadBerTlv(variable.value, &itemOffset, &specification);
    if (!status.ok() || itemOffset != variable.value.size() ||
        specification.tag != 0xa0) {
      return PreconditionError(
          "CommandTermination变量项缺少variableSpecification");
    }
    CommandTerminationVariable decodedVariable;
    status = DecodeMmsObjectName(specification.value, &decodedVariable.object);
    if (!status.ok()) {
      return status;
    }
    decodedVariable.lastApplError =
        decodedVariable.object.type == MmsObjectNameType::VMD_SPECIFIC &&
        decodedVariable.object.identifier == "LastApplError";
    variables.emplace_back(std::move(decodedVariable));
  }
  if (variables.empty()) {
    return PreconditionError("CommandTermination变量列表为空");
  }

  std::vector<BerTlvView> values;
  std::size_t valueOffset = 0;
  while (valueOffset < accessList.value.size()) {
    if (values.size() >= variables.size()) {
      return PreconditionError("CommandTermination结果数量多于变量数量");
    }
    BerTlvView value;
    status = ReadBerTlv(accessList.value, &valueOffset, &value);
    if (!status.ok() || !IsMmsDataTag(value.tag)) {
      return PreconditionError("CommandTermination包含非法AccessResult");
    }
    values.emplace_back(value);
  }
  if (values.size() != variables.size()) {
    return PreconditionError("CommandTermination变量和结果数量不一致");
  }

  std::optional<std::size_t> operationIndex;
  std::optional<std::size_t> errorIndex;
  for (std::size_t index = 0; index < variables.size(); ++index) {
    if (variables[index].lastApplError) {
      if (errorIndex.has_value()) {
        return PreconditionError("CommandTermination重复LastApplError变量");
      }
      errorIndex = index;
    } else if (SameObject(variables[index].object, expectedOper)) {
      if (operationIndex.has_value()) {
        return PreconditionError("CommandTermination重复目标Oper变量");
      }
      operationIndex = index;
    } else {
      return PreconditionError("CommandTermination包含非目标控制变量");
    }
  }
  if (!operationIndex.has_value() ||
      (errorIndex.has_value() && variables.size() != 2) ||
      (!errorIndex.has_value() && variables.size() != 1)) {
    return PreconditionError("CommandTermination目标变量数量或身份错误");
  }
  decoded.operationObject = expectedOper;
  decoded.success = !errorIndex.has_value();
  if (errorIndex.has_value()) {
    MmsLastApplError error;
    status = DecodeLastApplError(values[*errorIndex], expectedOper,
                                 expectedControlNumber, &error);
    if (!status.ok()) {
      return status;
    }
    decoded.lastApplError = std::move(error);
  }
  *result = std::move(decoded);
  return grpc::Status::OK;
}

grpc::Status EncodeMmsControlStructure(
    const MmsControlCommand& command,
    std::vector<std::uint8_t>* encodedData) {
  if (encodedData == nullptr) {
    return ArgumentError("控制结构输出参数为空");
  }
  encodedData->clear();
  auto status = ValidateControlCommand(command);
  if (!status.ok()) {
    return status;
  }

  std::vector<std::uint8_t> origin;
  status = EncodeOrigin(command, &origin);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> controlNumber;
  status = EncodeMmsDataUnsigned(command.controlNumber, &controlNumber);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> timestamp;
  status = EncodeMmsDataUtcTime(command.timestampMs, true, &timestamp);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::uint8_t> test;
  status = EncodeMmsDataBoolean(command.test, &test);
  if (!status.ok()) {
    return status;
  }
  const std::array<std::uint8_t, 1> checkPayload{
      static_cast<std::uint8_t>((command.check & 0x03u) << 6)};
  std::vector<std::uint8_t> check;
  status = EncodeMmsDataBitString(6, checkPayload, &check);
  if (!status.ok()) {
    return status;
  }

  std::vector<std::uint8_t> content;
  content.reserve(command.controlValue.size() + origin.size() +
                  controlNumber.size() + timestamp.size() + test.size() +
                  check.size() + 32);
  if (command.operation != MmsControlOperation::CANCEL) {
    content.insert(content.end(), command.controlValue.begin(),
                   command.controlValue.end());
  }
  if (command.operation == MmsControlOperation::OPERATE &&
      command.operateTimestampMs.has_value()) {
    std::vector<std::uint8_t> operateTimestamp;
    status = EncodeMmsDataUtcTime(*command.operateTimestampMs, true,
                                  &operateTimestamp);
    if (!status.ok()) {
      return status;
    }
    content.insert(content.end(), operateTimestamp.begin(),
                   operateTimestamp.end());
  }
  content.insert(content.end(), origin.begin(), origin.end());
  content.insert(content.end(), controlNumber.begin(), controlNumber.end());
  content.insert(content.end(), timestamp.begin(), timestamp.end());
  content.insert(content.end(), test.begin(), test.end());
  content.insert(content.end(), check.begin(), check.end());
  return AppendTlv(0xa2, content, encodedData);
}

grpc::Status BuildMmsControlSelectRequest(
    const MmsObjectName& controlObject, MmsReadRequest* request) {
  if (request == nullptr) {
    return ArgumentError("SBO选择请求输出参数为空");
  }
  request->variables.clear();
  request->specificationWithResult = false;
  MmsObjectName selectedObject;
  auto status = BuildMemberObject(controlObject, "$SBO", &selectedObject);
  if (!status.ok()) {
    return status;
  }
  request->variables.emplace_back(std::move(selectedObject));
  return grpc::Status::OK;
}

grpc::Status BuildMmsControlWriteRequest(
    const MmsControlCommand& command, MmsWriteRequest* request) {
  if (request == nullptr) {
    return ArgumentError("控制Write请求输出参数为空");
  }
  request->items.clear();
  auto status = ValidateControlCommand(command);
  if (!status.ok()) {
    return status;
  }
  const auto suffix = OperationSuffix(command.operation);
  MmsObjectName memberObject;
  status = BuildMemberObject(command.controlObject, suffix, &memberObject);
  if (!status.ok()) {
    return status;
  }
  MmsWriteRequestItem& item = request->items.emplace_back();
  item.variable = std::move(memberObject);
  status = EncodeMmsControlStructure(command, &item.encodedData);
  if (!status.ok()) {
    request->items.clear();
    return status;
  }
  return grpc::Status::OK;
}

}  // namespace IEC61850
