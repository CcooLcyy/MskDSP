#include "IEC61850RealtimePlan.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "IEC61850SvRuntime.h"

namespace IEC61850 {
namespace {

std::string ReferenceKey(std::string_view dataRef,
                         IEC61850Proto::FunctionalConstraint fc) {
  return std::format("{}#{}", dataRef, static_cast<int>(fc));
}

std::string Upper(std::string_view value) {
  std::string result(value);
  std::ranges::transform(result, result.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return result;
}

std::optional<IEC61850Proto::PointValueType> PointTypeFromBasicType(
    std::string_view basicType) {
  const auto normalized = Upper(basicType);
  if (normalized == "BOOLEAN") {
    return IEC61850Proto::POINT_VALUE_TYPE_BOOL;
  }
  if (normalized.starts_with("INT") || normalized == "ENUM" ||
      normalized == "CODEDENUM" || normalized == "QUALITY" ||
      normalized == "CHECK") {
    return IEC61850Proto::POINT_VALUE_TYPE_INT64;
  }
  if (normalized.starts_with("FLOAT")) {
    return IEC61850Proto::POINT_VALUE_TYPE_DOUBLE;
  }
  if (normalized.find("STRING") != std::string::npos ||
      normalized == "OBJREF" || normalized == "TIMESTAMP") {
    return IEC61850Proto::POINT_VALUE_TYPE_STRING;
  }
  if (normalized.find("OCTET") != std::string::npos ||
      normalized.find("BINARY") != std::string::npos) {
    return IEC61850Proto::POINT_VALUE_TYPE_BYTES;
  }
  return std::nullopt;
}

std::optional<std::pair<ProtocolSvMemberEncoding, std::uint8_t>>
SvEncodingFromBasicType(std::string_view basicType) {
  const auto normalized = Upper(basicType);
  if (normalized == "BOOLEAN") {
    return std::pair{ProtocolSvMemberEncoding::BOOLEAN, std::uint8_t{1}};
  }
  if (normalized == "INT8") {
    return std::pair{ProtocolSvMemberEncoding::SIGNED_INTEGER, std::uint8_t{1}};
  }
  if (normalized == "INT16") {
    return std::pair{ProtocolSvMemberEncoding::SIGNED_INTEGER, std::uint8_t{2}};
  }
  if (normalized == "INT32") {
    return std::pair{ProtocolSvMemberEncoding::SIGNED_INTEGER, std::uint8_t{4}};
  }
  if (normalized == "INT64") {
    return std::pair{ProtocolSvMemberEncoding::SIGNED_INTEGER, std::uint8_t{8}};
  }
  if (normalized == "INT8U") {
    return std::pair{ProtocolSvMemberEncoding::UNSIGNED_INTEGER, std::uint8_t{1}};
  }
  if (normalized == "INT16U") {
    return std::pair{ProtocolSvMemberEncoding::UNSIGNED_INTEGER, std::uint8_t{2}};
  }
  if (normalized == "INT32U") {
    return std::pair{ProtocolSvMemberEncoding::UNSIGNED_INTEGER, std::uint8_t{4}};
  }
  if (normalized == "FLOAT32") {
    return std::pair{ProtocolSvMemberEncoding::FLOATING_POINT, std::uint8_t{4}};
  }
  if (normalized == "FLOAT64") {
    return std::pair{ProtocolSvMemberEncoding::FLOATING_POINT, std::uint8_t{8}};
  }
  if (normalized == "QUALITY") {
    return std::pair{ProtocolSvMemberEncoding::UNSIGNED_INTEGER, std::uint8_t{4}};
  }
  return std::nullopt;
}

const IEC61850Proto::SclIed* FindIed(
    const IEC61850Proto::NormalizedSclModel& model,
    std::string_view iedName) {
  const auto it = std::ranges::find_if(
      model.ieds(),
      [&](const auto& candidate) { return candidate.name() == iedName; });
  return it == model.ieds().end() ? nullptr : &*it;
}

std::string SourceControlRef(const IEC61850Proto::SclExtRef& extRef,
                             std::string_view serviceSuffix) {
  if (extRef.ied_name().empty() || extRef.src_ld_inst().empty() ||
      extRef.src_ln_class().empty() || extRef.src_cb_name().empty()) {
    return {};
  }
  return std::format("{}{}/{}{}{}${}${}", extRef.ied_name(),
                     extRef.src_ld_inst(), extRef.src_prefix(),
                     extRef.src_ln_class(), extRef.src_ln_inst(), serviceSuffix,
                     extRef.src_cb_name());
}

std::string GooseControlRef(const IEC61850Proto::SclExtRef& extRef) {
  return SourceControlRef(extRef, "GO");
}

std::string SvControlRef(const IEC61850Proto::SclExtRef& extRef) {
  return SourceControlRef(extRef, "MS");
}

const IEC61850Proto::SclGseControl* FindUniqueGseControl(
    const IEC61850Proto::SclIed& publisher, std::string_view controlRef,
    bool* duplicate) {
  const IEC61850Proto::SclGseControl* found = nullptr;
  *duplicate = false;
  for (const auto& control : publisher.gse_controls()) {
    if (control.control_ref() != controlRef) {
      continue;
    }
    if (found != nullptr) {
      *duplicate = true;
      return nullptr;
    }
    found = &control;
  }
  return found;
}

const IEC61850Proto::SclSampledValueControl* FindUniqueSvControl(
    const IEC61850Proto::SclIed& publisher, std::string_view controlRef,
    bool* duplicate) {
  const IEC61850Proto::SclSampledValueControl* found = nullptr;
  *duplicate = false;
  for (const auto& control : publisher.sampled_value_controls()) {
    if (control.control_ref() != controlRef) {
      continue;
    }
    if (found != nullptr) {
      *duplicate = true;
      return nullptr;
    }
    found = &control;
  }
  return found;
}

const IEC61850Proto::SclDataSet* FindDataSet(
    const IEC61850Proto::SclIed& publisher,
    const IEC61850Proto::SclGseControl& control) {
  const auto it = std::ranges::find_if(
      publisher.data_sets(), [&](const auto& dataSet) {
        return dataSet.data_set_ref() == control.data_set_ref() &&
               dataSet.access_point() == control.access_point();
      });
  return it == publisher.data_sets().end() ? nullptr : &*it;
}

const IEC61850Proto::SclDataSet* FindDataSet(
    const IEC61850Proto::SclIed& publisher,
    const IEC61850Proto::SclSampledValueControl& control) {
  const auto it = std::ranges::find_if(
      publisher.data_sets(), [&](const auto& dataSet) {
        return dataSet.data_set_ref() == control.data_set_ref() &&
               dataSet.access_point() == control.access_point();
      });
  return it == publisher.data_sets().end() ? nullptr : &*it;
}

const IEC61850Proto::SclDataAttribute* FindDataAttribute(
    const IEC61850Proto::SclIed& publisher, std::string_view accessPoint,
    const IEC61850Proto::SclFcda& member) {
  const auto it = std::ranges::find_if(
      publisher.data_attributes(), [&](const auto& attribute) {
        return attribute.access_point() == accessPoint &&
               attribute.data_ref() == member.data_ref() &&
               attribute.fc() == member.fc();
      });
  return it == publisher.data_attributes().end() ? nullptr : &*it;
}

grpc::Status BuildEndpoint(
    const IEC61850Proto::NormalizedSclModel& model,
    const ProtocolNetworkBinding& localBinding,
    const IEC61850Proto::SclExtRef& extRef,
    ProtocolGooseEndpointPlan* endpoint) {
  const IEC61850Proto::SclMulticastAddress* address = nullptr;
  for (const auto& connectedAp : model.connected_access_points()) {
    if (connectedAp.ied_name() != extRef.ied_name() ||
        connectedAp.subnetwork_name() !=
            localBinding.connectedAccessPoint.subnetwork_name()) {
      continue;
    }
    for (const auto& candidate : connectedAp.gse()) {
      if (candidate.ld_inst() != extRef.src_ld_inst() ||
          candidate.cb_name() != extRef.src_cb_name()) {
        continue;
      }
      if (address != nullptr) {
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            std::format(
                "GOOSE发布端通信地址重复: 发布IED={}, 网段={}, 控制块={}",
                extRef.ied_name(), connectedAp.subnetwork_name(),
                extRef.src_cb_name()));
      }
      address = &candidate;
    }
  }
  if (address == nullptr || !address->has_app_id() ||
      address->app_id() == 0 || address->app_id() > 0xffff ||
      address->mac_address().empty()) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        std::format(
            "GOOSE发布端缺少有效MAC或数值APPID: 发布IED={}, 网段={}, 控制块={}",
            extRef.ied_name(),
            localBinding.connectedAccessPoint.subnetwork_name(),
            extRef.src_cb_name()));
  }
  if ((address->has_vlan_id() && address->vlan_id() > 4095) ||
      (address->has_vlan_priority() && address->vlan_priority() > 7)) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        std::format("GOOSE发布端VLAN参数超出范围: 发布IED={}, 控制块={}",
                    extRef.ied_name(), extRef.src_cb_name()));
  }
  endpoint->channel = localBinding.channel.channel();
  endpoint->interfaceName = localBinding.channel.interface_name();
  endpoint->subnetworkName =
      localBinding.connectedAccessPoint.subnetwork_name();
  endpoint->destinationMac = address->mac_address();
  endpoint->appId = static_cast<std::uint16_t>(address->app_id());
  endpoint->vlanTagged = address->has_vlan_id();
  endpoint->vlanId = address->has_vlan_id()
                         ? static_cast<std::uint16_t>(address->vlan_id())
                         : 0;
  endpoint->vlanPriority =
      address->has_vlan_priority()
          ? static_cast<std::uint8_t>(address->vlan_priority())
          : 0;
  return grpc::Status::OK;
}

std::string LogicalDeviceFromOwnerNode(std::string_view ownerNodeRef,
                                       std::string_view iedName) {
  const auto slash = ownerNodeRef.find('/');
  if (slash == std::string_view::npos || slash == 0) {
    return {};
  }
  auto logicalDevice = ownerNodeRef.substr(0, slash);
  if (!iedName.empty() && logicalDevice.starts_with(iedName)) {
    logicalDevice.remove_prefix(iedName.size());
  }
  return std::string(logicalDevice);
}

grpc::Status BuildLocalGooseEndpoint(
    const IEC61850Proto::NormalizedSclModel& model,
    const ProtocolNetworkBinding& localBinding,
    const IEC61850Proto::SclIed& localIed,
    const IEC61850Proto::SclGseControl& control,
    ProtocolGooseEndpointPlan* endpoint) {
  if (endpoint == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "GOOSE本地发布端点输出参数为空");
  }
  const auto logicalDevice =
      LogicalDeviceFromOwnerNode(control.owner_node_ref(), localIed.name());
  if (logicalDevice.empty()) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        std::format("GOOSE本地发布控制块缺少有效逻辑设备: {}",
                    control.control_ref()));
  }
  const IEC61850Proto::SclMulticastAddress* address = nullptr;
  for (const auto& connectedAp : model.connected_access_points()) {
    if (connectedAp.ied_name() != localIed.name() ||
        connectedAp.subnetwork_name() !=
            localBinding.connectedAccessPoint.subnetwork_name() ||
        (!control.access_point().empty() &&
         connectedAp.ap_name() != control.access_point())) {
      continue;
    }
    for (const auto& candidate : connectedAp.gse()) {
      if (candidate.ld_inst() != logicalDevice ||
          candidate.cb_name() != control.name()) {
        continue;
      }
      if (address != nullptr) {
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            std::format("GOOSE本地发布端通信地址重复: 发布IED={}, 网段={}, 控制块={}",
                        localIed.name(), connectedAp.subnetwork_name(),
                        control.name()));
      }
      address = &candidate;
    }
  }
  if (address == nullptr || !address->has_app_id() ||
      address->app_id() == 0 || address->app_id() > 0xffff ||
      address->mac_address().empty()) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        std::format("GOOSE本地发布端缺少有效MAC或数值APPID: 发布IED={}, 控制块={}",
                    localIed.name(), control.control_ref()));
  }
  if ((address->has_vlan_id() && address->vlan_id() > 4095) ||
      (address->has_vlan_priority() && address->vlan_priority() > 7)) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        std::format("GOOSE本地发布端VLAN参数超出范围: 发布IED={}, 控制块={}",
                    localIed.name(), control.control_ref()));
  }
  endpoint->channel = localBinding.channel.channel();
  endpoint->interfaceName = localBinding.channel.interface_name();
  endpoint->subnetworkName =
      localBinding.connectedAccessPoint.subnetwork_name();
  endpoint->destinationMac = address->mac_address();
  endpoint->appId = static_cast<std::uint16_t>(address->app_id());
  endpoint->vlanTagged = address->has_vlan_id();
  endpoint->vlanId = address->has_vlan_id()
                         ? static_cast<std::uint16_t>(address->vlan_id())
                         : 0;
  endpoint->vlanPriority =
      address->has_vlan_priority()
          ? static_cast<std::uint8_t>(address->vlan_priority())
          : 0;
  return grpc::Status::OK;
}

grpc::Status BuildSvEndpoint(
    const IEC61850Proto::NormalizedSclModel& model,
    const ProtocolNetworkBinding& localBinding,
    const IEC61850Proto::SclExtRef& extRef,
    ProtocolSvEndpointPlan* endpoint) {
  const IEC61850Proto::SclMulticastAddress* address = nullptr;
  for (const auto& connectedAp : model.connected_access_points()) {
    if (connectedAp.ied_name() != extRef.ied_name() ||
        connectedAp.subnetwork_name() !=
            localBinding.connectedAccessPoint.subnetwork_name()) {
      continue;
    }
    for (const auto& candidate : connectedAp.smv()) {
      if (candidate.ld_inst() != extRef.src_ld_inst() ||
          candidate.cb_name() != extRef.src_cb_name()) {
        continue;
      }
      if (address != nullptr) {
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            std::format("SV发布端通信地址重复: 发布IED={}, 网段={}, 控制块={}",
                        extRef.ied_name(), connectedAp.subnetwork_name(),
                        extRef.src_cb_name()));
      }
      address = &candidate;
    }
  }
  if (address == nullptr || !address->has_app_id() ||
      address->app_id() == 0 || address->app_id() > 0xffff ||
      address->mac_address().empty()) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        std::format("SV发布端缺少有效MAC或数值APPID: 发布IED={}, 网段={}, 控制块={}",
                    extRef.ied_name(),
                    localBinding.connectedAccessPoint.subnetwork_name(),
                    extRef.src_cb_name()));
  }
  if ((address->has_vlan_id() && address->vlan_id() > 4095) ||
      (address->has_vlan_priority() && address->vlan_priority() > 7)) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        std::format("SV发布端VLAN参数超出范围: 发布IED={}, 控制块={}",
                    extRef.ied_name(), extRef.src_cb_name()));
  }
  endpoint->channel = localBinding.channel.channel();
  endpoint->interfaceName = localBinding.channel.interface_name();
  endpoint->subnetworkName =
      localBinding.connectedAccessPoint.subnetwork_name();
  endpoint->destinationMac = address->mac_address();
  endpoint->appId = static_cast<std::uint16_t>(address->app_id());
  endpoint->vlanTagged = address->has_vlan_id();
  endpoint->vlanId = address->has_vlan_id()
                         ? static_cast<std::uint16_t>(address->vlan_id())
                         : 0;
  endpoint->vlanPriority =
      address->has_vlan_priority()
          ? static_cast<std::uint8_t>(address->vlan_priority())
          : 0;
  return grpc::Status::OK;
}

bool IsRealtimeScalar(IEC61850Proto::PointValueType type) {
  return type == IEC61850Proto::POINT_VALUE_TYPE_BOOL ||
         type == IEC61850Proto::POINT_VALUE_TYPE_INT64 ||
         type == IEC61850Proto::POINT_VALUE_TYPE_DOUBLE;
}

}  // namespace

grpc::Status BuildRealtimeProtocolPlan(
    const IEC61850Proto::NormalizedSclModel& model,
    const IEC61850Proto::PointMappings& mappings,
    ProtocolIedPlan* plan) {
  if (plan == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "IEC61850实时启动计划不能为空");
  }
  plan->realtimeSignals.clear();
  plan->gooseSubscriptions.clear();
  plan->goosePublishers.clear();
  plan->svStreams.clear();

  std::unordered_map<std::string, std::uint32_t> gooseSignalIds;
  std::uint32_t nextSignalId = 1;
  for (const auto& mapping : mappings.points()) {
    if (mapping.source() != IEC61850Proto::POINT_SOURCE_GOOSE ||
        !IsRealtimeScalar(mapping.value_type())) {
      continue;
    }
    auto& signal = plan->realtimeSignals.emplace_back();
    signal.signalId = nextSignalId++;
    signal.tag = mapping.tag();
    signal.dataRef = mapping.data_ref();
    signal.fc = mapping.fc();
    signal.source = mapping.source();
    signal.valueType = mapping.value_type();
    gooseSignalIds.emplace(ReferenceKey(mapping.data_ref(), mapping.fc()),
                           signal.signalId);
  }

  if (!plan->config.enable_goose() && !plan->config.enable_sv()) {
    return grpc::Status::OK;
  }

  std::map<std::string, const IEC61850Proto::SclExtRef*>
      firstExtRefByControl;
  std::unordered_set<std::string> extRefReferences;
  if (plan->config.enable_goose()) {
    for (const auto& extRef : plan->ied.ext_refs()) {
      if (Upper(extRef.service_type()) != "GOOSE") {
        continue;
      }
      const auto controlRef = GooseControlRef(extRef);
      if (controlRef.empty() || extRef.source_data_ref().empty() ||
          extRef.fc() == IEC61850Proto::FUNCTIONAL_CONSTRAINT_UNSPECIFIED) {
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            std::format("GOOSE ExtRef源控制块或数据引用不完整: IED={}, intAddr={}",
                        plan->config.conn_name(), extRef.int_addr()));
      }
      firstExtRefByControl.try_emplace(controlRef, &extRef);
      extRefReferences.emplace(
          ReferenceKey(extRef.source_data_ref(), extRef.fc()));
    }
  }

  if (plan->config.enable_goose()) {

  for (const auto& [reference, signalId] : gooseSignalIds) {
    if (!extRefReferences.contains(reference)) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          std::format("GOOSE点映射未匹配当前AP的ExtRef: IED={}, 引用={}",
                      plan->config.conn_name(), reference));
    }
  }

  std::uint32_t nextSubscriptionId = 1;
  for (const auto& [controlRef, firstExtRef] : firstExtRefByControl) {
    const auto* publisher = FindIed(model, firstExtRef->ied_name());
    if (publisher == nullptr) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          std::format("GOOSE ExtRef引用的发布IED不存在: {}",
                      firstExtRef->ied_name()));
    }
    bool duplicateControl = false;
    const auto* control =
        FindUniqueGseControl(*publisher, controlRef, &duplicateControl);
    if (control == nullptr) {
      const auto reason = duplicateControl
                              ? std::format("GOOSE发布控制块引用不唯一: {}",
                                            controlRef)
                              : std::format("GOOSE发布控制块不存在: {}",
                                            controlRef);
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          reason);
    }
    const auto* dataSet = FindDataSet(*publisher, *control);
    if (dataSet == nullptr) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          std::format("GOOSE发布控制块的数据集不存在: {}",
                      control->data_set_ref()));
    }

    auto& subscription = plan->gooseSubscriptions.emplace_back();
    subscription.subscriptionId = nextSubscriptionId++;
    subscription.publisherIed = publisher->name();
    subscription.controlRef = control->control_ref();
    subscription.dataSetRef = control->data_set_ref();
    subscription.goId = control->go_id();
    subscription.configRevision = control->config_revision();
    if (subscription.goId.empty()) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          std::format("GOOSE发布控制块缺少goID: {}", controlRef));
    }

    for (const auto& member : dataSet->members()) {
      const auto* attribute = FindDataAttribute(
          *publisher, control->access_point(), member);
      if (attribute == nullptr) {
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            std::format("GOOSE DataSet成员不是可解码的数据属性: {}#{}",
                        member.data_ref(), static_cast<int>(member.fc())));
      }
      const auto valueType = PointTypeFromBasicType(attribute->basic_type());
      if (!valueType.has_value() || !IsRealtimeScalar(*valueType)) {
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            std::format("GOOSE DataSet成员类型暂不支持: 引用={}, 类型={}",
                        member.data_ref(), attribute->basic_type()));
      }
      auto& memberPlan = subscription.members.emplace_back();
      memberPlan.dataRef = member.data_ref();
      memberPlan.fc = member.fc();
      memberPlan.valueType = *valueType;
      if (memberPlan.valueType == IEC61850Proto::POINT_VALUE_TYPE_DOUBLE) {
        const auto normalizedType = Upper(attribute->basic_type());
        memberPlan.encodedSize =
            normalizedType == "FLOAT32" ? 4 : normalizedType == "FLOAT64" ? 8 : 0;
        if (memberPlan.encodedSize == 0) {
          return grpc::Status(
              grpc::StatusCode::FAILED_PRECONDITION,
              std::format("GOOSE浮点成员缺少FLOAT32/FLOAT64线宽度: {}",
                          member.data_ref()));
        }
      }
      memberPlan.qualityValue = Upper(attribute->basic_type()) == "QUALITY";
      const auto signalIt = gooseSignalIds.find(
          ReferenceKey(member.data_ref(), member.fc()));
      if (signalIt != gooseSignalIds.end()) {
        memberPlan.signalId = signalIt->second;
        const auto& signal = plan->realtimeSignals[signalIt->second - 1];
        if (signal.valueType != memberPlan.valueType) {
          return grpc::Status(
              grpc::StatusCode::FAILED_PRECONDITION,
              std::format("GOOSE点映射类型与DataSet成员不一致: {}",
                          member.data_ref()));
        }
      }
    }

    for (const auto& binding : plan->networkBindings) {
      auto& endpoint = subscription.endpoints.emplace_back();
      const auto status = BuildEndpoint(model, binding, *firstExtRef,
                                        &endpoint);
      if (!status.ok()) {
        plan->gooseSubscriptions.clear();
        return status;
      }
    }
  }
  }

  if (plan->config.enable_goose()) {
    std::uint32_t nextPublisherId = 1;
    for (const auto& control : plan->ied.gse_controls()) {
      bool duplicateControl = false;
      const auto* uniqueControl = FindUniqueGseControl(
          plan->ied, control.control_ref(), &duplicateControl);
      if (uniqueControl == nullptr) {
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            duplicateControl
                ? std::format("GOOSE本地发布控制块引用不唯一: {}",
                              control.control_ref())
                : std::format("GOOSE本地发布控制块不存在: {}",
                              control.control_ref()));
      }
      const auto* dataSet = FindDataSet(plan->ied, control);
      if (dataSet == nullptr) {
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            std::format("GOOSE本地发布控制块的数据集不存在: {}",
                        control.data_set_ref()));
      }
      if (control.go_id().empty() || control.config_revision() == 0) {
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            std::format("GOOSE本地发布控制块缺少goID或ConfRev: {}",
                        control.control_ref()));
      }

      auto& publisher = plan->goosePublishers.emplace_back();
      publisher.publisherId = nextPublisherId++;
      publisher.publisherIed = plan->ied.name();
      publisher.controlRef = control.control_ref();
      publisher.dataSetRef = control.data_set_ref();
      publisher.goId = control.go_id();
      publisher.configRevision = control.config_revision();
      for (const auto& member : dataSet->members()) {
        const auto* attribute = FindDataAttribute(
            plan->ied, control.access_point(), member);
        if (attribute == nullptr) {
          return grpc::Status(
              grpc::StatusCode::FAILED_PRECONDITION,
              std::format("GOOSE本地发布DataSet成员不是可编码数据属性: {}#{}",
                          member.data_ref(), static_cast<int>(member.fc())));
        }
        const auto valueType = PointTypeFromBasicType(attribute->basic_type());
        if (!valueType.has_value() || !IsRealtimeScalar(*valueType)) {
          return grpc::Status(
              grpc::StatusCode::FAILED_PRECONDITION,
              std::format("GOOSE本地发布DataSet成员类型暂不支持: 引用={}, 类型={}",
                          member.data_ref(), attribute->basic_type()));
        }
        auto& memberPlan = publisher.members.emplace_back();
        memberPlan.dataRef = member.data_ref();
        memberPlan.fc = member.fc();
        memberPlan.valueType = *valueType;
        memberPlan.qualityValue = Upper(attribute->basic_type()) == "QUALITY";
        if (memberPlan.valueType == IEC61850Proto::POINT_VALUE_TYPE_DOUBLE) {
          const auto normalizedType = Upper(attribute->basic_type());
          memberPlan.encodedSize = normalizedType == "FLOAT32"
                                       ? 4
                                       : normalizedType == "FLOAT64" ? 8 : 0;
          if (memberPlan.encodedSize == 0) {
            return grpc::Status(
                grpc::StatusCode::FAILED_PRECONDITION,
                std::format("GOOSE本地发布浮点成员缺少FLOAT32/FLOAT64线宽度: {}",
                            member.data_ref()));
          }
        }
      }
      for (const auto& binding : plan->networkBindings) {
        auto& endpoint = publisher.endpoints.emplace_back();
        const auto status = BuildLocalGooseEndpoint(
            model, binding, plan->ied, control, &endpoint);
        if (!status.ok()) {
          plan->goosePublishers.clear();
          return status;
        }
      }
    }
  }

  if (!plan->config.enable_sv()) {
    return grpc::Status::OK;
  }

  std::map<std::string, const IEC61850Proto::SclExtRef*>
      firstSvExtRefByControl;
  for (const auto& extRef : plan->ied.ext_refs()) {
    const auto serviceType = Upper(extRef.service_type());
    if (serviceType != "SMV" && serviceType != "SV") {
      continue;
    }
    const auto controlRef = SvControlRef(extRef);
    if (controlRef.empty()) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          std::format("SV ExtRef源控制块引用不完整: IED={}, intAddr={}",
                      plan->config.conn_name(), extRef.int_addr()));
    }
    firstSvExtRefByControl.try_emplace(controlRef, &extRef);
  }

  std::uint32_t nextStreamId = 1;
  for (const auto& [controlRef, firstExtRef] : firstSvExtRefByControl) {
    const auto* publisher = FindIed(model, firstExtRef->ied_name());
    if (publisher == nullptr) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          std::format("SV ExtRef引用的发布IED不存在: {}",
                      firstExtRef->ied_name()));
    }
    bool duplicateControl = false;
    const auto* control =
        FindUniqueSvControl(*publisher, controlRef, &duplicateControl);
    if (control == nullptr) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          duplicateControl
              ? std::format("SV发布控制块引用不唯一: {}", controlRef)
              : std::format("SV发布控制块不存在: {}", controlRef));
    }
    const auto* dataSet = FindDataSet(*publisher, *control);
    if (dataSet == nullptr) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          std::format("SV发布控制块的数据集不存在: {}",
                      control->data_set_ref()));
    }
    if (control->sv_id().empty() || control->config_revision() == 0 ||
        control->nof_asdu() == 0 || control->sample_rate() == 0) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          std::format("SV发布控制块参数不完整: {}", controlRef));
    }

    auto& stream = plan->svStreams.emplace_back();
    stream.streamId = nextStreamId++;
    stream.publisherIed = publisher->name();
    stream.controlRef = control->control_ref();
    stream.dataSetRef = control->data_set_ref();
    stream.svId = control->sv_id();
    stream.configRevision = control->config_revision();
    stream.sampleRate = control->sample_rate();
    stream.nominalFrequencyHz =
        ResolveSvNominalFrequencyHz(plan->config.nominal_frequency_hz());
    stream.nofAsdu = control->nof_asdu();
    for (const auto& member : dataSet->members()) {
      const auto* attribute =
          FindDataAttribute(*publisher, control->access_point(), member);
      if (attribute == nullptr) {
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            std::format("SV DataSet成员不是可解码的数据属性: {}#{}",
                        member.data_ref(), static_cast<int>(member.fc())));
      }
      const auto valueType = PointTypeFromBasicType(attribute->basic_type());
      const auto encoding = SvEncodingFromBasicType(attribute->basic_type());
      if (!valueType.has_value() || !encoding.has_value() ||
          !IsRealtimeScalar(*valueType)) {
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            std::format("SV DataSet成员类型暂不支持: 引用={}, 类型={}",
                        member.data_ref(), attribute->basic_type()));
      }
      auto& memberPlan = stream.members.emplace_back();
      memberPlan.dataRef = member.data_ref();
      memberPlan.fc = member.fc();
      memberPlan.valueType = *valueType;
      memberPlan.encoding = encoding->first;
      memberPlan.encodedSize = encoding->second;
      memberPlan.signalId = nextSignalId++;
      auto& signal = plan->realtimeSignals.emplace_back();
      signal.signalId = memberPlan.signalId;
      signal.dataRef = memberPlan.dataRef;
      signal.fc = memberPlan.fc;
      signal.source = IEC61850Proto::POINT_SOURCE_SV_DERIVED;
      signal.valueType = memberPlan.valueType;

      // 首期只为单ASDU数值成员生成单周期RMS派生点；多ASDU的lane语义
      // 需要现场模型确认后再进入数学链路。
      if (control->nof_asdu() == 1 &&
          (memberPlan.valueType == IEC61850Proto::POINT_VALUE_TYPE_INT64 ||
           memberPlan.valueType == IEC61850Proto::POINT_VALUE_TYPE_DOUBLE)) {
        auto& derived = stream.derivedMembers.emplace_back();
        derived.inputSignalId = memberPlan.signalId;
        derived.rmsSignalId = nextSignalId++;
        derived.rmsDataRef = std::format(
            "SV_DERIVED/{}/RMS/{}", stream.streamId, memberPlan.dataRef);

        auto& derivedSignal = plan->realtimeSignals.emplace_back();
        derivedSignal.signalId = derived.rmsSignalId;
        derivedSignal.dataRef = derived.rmsDataRef;
        derivedSignal.fc = memberPlan.fc;
        derivedSignal.source = IEC61850Proto::POINT_SOURCE_SV_DERIVED;
        derivedSignal.valueType = IEC61850Proto::POINT_VALUE_TYPE_DOUBLE;
      }
    }
    for (const auto& binding : plan->networkBindings) {
      auto& endpoint = stream.endpoints.emplace_back();
      const auto status =
          BuildSvEndpoint(model, binding, *firstExtRef, &endpoint);
      if (!status.ok()) {
        plan->svStreams.clear();
        return status;
      }
    }
  }

  for (const auto& mapping : mappings.points()) {
    if (mapping.source() != IEC61850Proto::POINT_SOURCE_SV_DERIVED) {
      continue;
    }
    const auto signalIt = std::ranges::find_if(
        plan->realtimeSignals, [&mapping](const auto& signal) {
          return signal.source == IEC61850Proto::POINT_SOURCE_SV_DERIVED &&
                 signal.dataRef == mapping.data_ref() &&
                 signal.fc == mapping.fc();
        });
    if (signalIt == plan->realtimeSignals.end()) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          std::format("SV派生点映射未匹配启动计划: 引用={}#{}",
                      mapping.data_ref(), static_cast<int>(mapping.fc())));
    }
    if (signalIt->valueType != mapping.value_type()) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          std::format("SV派生点映射类型不匹配: 引用={}", mapping.data_ref()));
    }
    if (!mapping.tag().empty()) {
      signalIt->tag = mapping.tag();
    }
  }
  return grpc::Status::OK;
}

}  // namespace IEC61850
