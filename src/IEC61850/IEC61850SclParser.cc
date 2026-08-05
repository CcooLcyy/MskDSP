#include "IEC61850SclParser.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <format>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <pugixml.hpp>

namespace IEC61850 {
namespace {

using IEC61850Proto::FunctionalConstraint;
using IEC61850Proto::NormalizedSclModel;
using IEC61850Proto::ValidationIssue;

std::string_view LocalName(std::string_view qualifiedName) {
  const auto pos = qualifiedName.find(':');
  return pos == std::string_view::npos ? qualifiedName : qualifiedName.substr(pos + 1);
}

bool IsNode(const pugi::xml_node& node, std::string_view localName) {
  return node.type() == pugi::node_element && LocalName(node.name()) == localName;
}

pugi::xml_node Child(const pugi::xml_node& parent, std::string_view localName) {
  for (const auto& child : parent.children()) {
    if (IsNode(child, localName)) {
      return child;
    }
  }
  return {};
}

std::vector<pugi::xml_node> Children(const pugi::xml_node& parent,
                                     std::string_view localName) {
  std::vector<pugi::xml_node> nodes;
  for (const auto& child : parent.children()) {
    if (IsNode(child, localName)) {
      nodes.emplace_back(child);
    }
  }
  return nodes;
}

std::string Attribute(const pugi::xml_node& node, const char* name) {
  return node.attribute(name).as_string();
}

bool BooleanAttribute(const pugi::xml_node& node, const char* name,
                      bool defaultValue = false) {
  const auto attribute = node.attribute(name);
  if (!attribute) {
    return defaultValue;
  }
  const std::string value = attribute.as_string();
  return value == "true" || value == "1" || value == "TRUE";
}

uint64_t UnsignedAttribute(const pugi::xml_node& node, const char* name,
                           uint64_t defaultValue = 0) {
  const auto attribute = node.attribute(name);
  if (!attribute) {
    return defaultValue;
  }
  uint64_t value = 0;
  const std::string text = attribute.as_string();
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 10);
  return ec == std::errc() && ptr == text.data() + text.size() ? value : defaultValue;
}

void AddIssue(std::vector<ValidationIssue>* issues,
              IEC61850Proto::ValidationSeverity severity,
              std::string code, std::string path, std::string message) {
  if (issues == nullptr) {
    return;
  }
  auto& issue = issues->emplace_back();
  issue.set_severity(severity);
  issue.set_code(std::move(code));
  issue.set_path(std::move(path));
  issue.set_message(std::move(message));
}

grpc::Status Invalid(std::string message) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::move(message));
}

IEC61850Proto::SclDocumentKind DocumentKind(std::string sourceName) {
  std::transform(sourceName.begin(), sourceName.end(), sourceName.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  if (sourceName.ends_with(".scd")) {
    return IEC61850Proto::SCL_DOCUMENT_KIND_SCD;
  }
  if (sourceName.ends_with(".cid")) {
    return IEC61850Proto::SCL_DOCUMENT_KIND_CID;
  }
  if (sourceName.ends_with(".icd")) {
    return IEC61850Proto::SCL_DOCUMENT_KIND_ICD;
  }
  return IEC61850Proto::SCL_DOCUMENT_KIND_UNSPECIFIED;
}

std::string Fnv1aChecksum(std::string_view content) {
  uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char byte : content) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  std::ostringstream stream;
  stream << std::hex << std::setfill('0') << std::setw(16) << hash;
  return stream.str();
}

FunctionalConstraint ParseFunctionalConstraint(std::string_view fc) {
  static const std::unordered_map<std::string_view, FunctionalConstraint> constraints{
      {"ST", IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST},
      {"MX", IEC61850Proto::FUNCTIONAL_CONSTRAINT_MX},
      {"SP", IEC61850Proto::FUNCTIONAL_CONSTRAINT_SP},
      {"SV", IEC61850Proto::FUNCTIONAL_CONSTRAINT_SV},
      {"CF", IEC61850Proto::FUNCTIONAL_CONSTRAINT_CF},
      {"DC", IEC61850Proto::FUNCTIONAL_CONSTRAINT_DC},
      {"SG", IEC61850Proto::FUNCTIONAL_CONSTRAINT_SG},
      {"SE", IEC61850Proto::FUNCTIONAL_CONSTRAINT_SE},
      {"SR", IEC61850Proto::FUNCTIONAL_CONSTRAINT_SR},
      {"OR", IEC61850Proto::FUNCTIONAL_CONSTRAINT_OR},
      {"BL", IEC61850Proto::FUNCTIONAL_CONSTRAINT_BL},
      {"EX", IEC61850Proto::FUNCTIONAL_CONSTRAINT_EX},
      {"CO", IEC61850Proto::FUNCTIONAL_CONSTRAINT_CO},
      {"RP", IEC61850Proto::FUNCTIONAL_CONSTRAINT_RP},
      {"BR", IEC61850Proto::FUNCTIONAL_CONSTRAINT_BR},
      {"LG", IEC61850Proto::FUNCTIONAL_CONSTRAINT_LG},
      {"GO", IEC61850Proto::FUNCTIONAL_CONSTRAINT_GO},
      {"MS", IEC61850Proto::FUNCTIONAL_CONSTRAINT_MS},
      {"US", IEC61850Proto::FUNCTIONAL_CONSTRAINT_US},
  };
  const auto it = constraints.find(fc);
  return it == constraints.end() ? IEC61850Proto::FUNCTIONAL_CONSTRAINT_UNSPECIFIED
                                 : it->second;
}

std::string NodeName(const pugi::xml_node& node) {
  return Attribute(node, "prefix") + Attribute(node, "lnClass") +
         Attribute(node, "inst");
}

std::string NodeReference(std::string_view iedName, std::string_view ldInst,
                          const pugi::xml_node& node) {
  return std::format("{}{}/{}", iedName, ldInst, NodeName(node));
}

std::string DataSetReference(std::string_view ownerNodeRef,
                             std::string_view dataSetName) {
  if (dataSetName.find('/') != std::string_view::npos ||
      dataSetName.find('$') != std::string_view::npos) {
    return std::string(dataSetName);
  }
  return std::format("{}${}", ownerNodeRef, dataSetName);
}

std::string FcdaReference(std::string_view defaultIedName,
                          const pugi::xml_node& fcda) {
  const std::string iedName = fcda.attribute("iedName")
                                  ? Attribute(fcda, "iedName")
                                  : std::string(defaultIedName);
  std::string ref = std::format("{}{}/{}{}{}", iedName,
                                Attribute(fcda, "ldInst"),
                                Attribute(fcda, "prefix"),
                                Attribute(fcda, "lnClass"),
                                Attribute(fcda, "lnInst"));
  const auto doName = Attribute(fcda, "doName");
  if (!doName.empty()) {
    ref += "." + doName;
  }
  const auto daName = Attribute(fcda, "daName");
  if (!daName.empty()) {
    ref += "." + daName;
  }
  return ref;
}

std::string ExtRefSourceReference(const pugi::xml_node& extRef) {
  const auto iedName = Attribute(extRef, "iedName");
  const auto ldInst = Attribute(extRef, "ldInst");
  const auto lnClass = Attribute(extRef, "lnClass");
  const auto doName = Attribute(extRef, "doName");
  if (iedName.empty() || ldInst.empty() || lnClass.empty() || doName.empty()) {
    return {};
  }
  std::string ref = std::format("{}{}/{}{}{}", iedName, ldInst,
                                Attribute(extRef, "prefix"), lnClass,
                                Attribute(extRef, "lnInst"));
  ref += "." + doName;
  const auto daName = Attribute(extRef, "daName");
  if (!daName.empty()) {
    ref += "." + daName;
  }
  return ref;
}

std::optional<uint32_t> ParseHex(std::string_view text) {
  if (text.starts_with("0x") || text.starts_with("0X")) {
    text.remove_prefix(2);
  }
  uint32_t value = 0;
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
  if (ec != std::errc() || ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

void ParseAddressParameters(const pugi::xml_node& address,
                            google::protobuf::RepeatedPtrField<IEC61850Proto::SclAddressParameter>* out,
                            IEC61850Proto::SclMulticastAddress* multicast) {
  for (const auto& parameterNode : Children(address, "P")) {
    const auto type = Attribute(parameterNode, "type");
    const std::string value = parameterNode.child_value();
    auto* parameter = out->Add();
    parameter->set_type(type);
    parameter->set_value(value);
    if (multicast == nullptr) {
      continue;
    }
    if (type == "MAC-Address") {
      multicast->set_mac_address(value);
    } else if (type == "APPID") {
      if (const auto parsed = ParseHex(value)) {
        multicast->set_app_id(*parsed);
      }
    } else if (type == "VLAN-ID") {
      if (const auto parsed = ParseHex(value)) {
        multicast->set_vlan_id(*parsed);
      }
    } else if (type == "VLAN-PRIORITY") {
      if (const auto parsed = ParseHex(value)) {
        multicast->set_vlan_priority(*parsed);
      }
    }
  }
}

void ParseCommunication(const pugi::xml_node& scl, NormalizedSclModel* model) {
  const auto communication = Child(scl, "Communication");
  for (const auto& subnetwork : Children(communication, "SubNetwork")) {
    for (const auto& connectedNode : Children(subnetwork, "ConnectedAP")) {
      auto* connected = model->add_connected_access_points();
      connected->set_subnetwork_name(Attribute(subnetwork, "name"));
      connected->set_network_type(Attribute(subnetwork, "type"));
      connected->set_ied_name(Attribute(connectedNode, "iedName"));
      connected->set_ap_name(Attribute(connectedNode, "apName"));
      ParseAddressParameters(Child(connectedNode, "Address"),
                             connected->mutable_address(), nullptr);

      for (const auto& gseNode : Children(connectedNode, "GSE")) {
        auto* gse = connected->add_gse();
        gse->set_ld_inst(Attribute(gseNode, "ldInst"));
        gse->set_cb_name(Attribute(gseNode, "cbName"));
        ParseAddressParameters(Child(gseNode, "Address"),
                               gse->mutable_parameters(), gse);
      }
      for (const auto& smvNode : Children(connectedNode, "SMV")) {
        auto* smv = connected->add_smv();
        smv->set_ld_inst(Attribute(smvNode, "ldInst"));
        smv->set_cb_name(Attribute(smvNode, "cbName"));
        ParseAddressParameters(Child(smvNode, "Address"),
                               smv->mutable_parameters(), smv);
      }
    }
  }
}

struct TypeTemplates {
  std::unordered_map<std::string, pugi::xml_node> logicalNodes;
  std::unordered_map<std::string, pugi::xml_node> dataObjects;
  std::unordered_map<std::string, pugi::xml_node> dataAttributes;
};

TypeTemplates IndexTypeTemplates(const pugi::xml_node& scl) {
  TypeTemplates types;
  const auto templates = Child(scl, "DataTypeTemplates");
  for (const auto& child : templates.children()) {
    const auto id = Attribute(child, "id");
    if (id.empty()) {
      continue;
    }
    if (IsNode(child, "LNodeType")) {
      types.logicalNodes.emplace(id, child);
    } else if (IsNode(child, "DOType")) {
      types.dataObjects.emplace(id, child);
    } else if (IsNode(child, "DAType")) {
      types.dataAttributes.emplace(id, child);
    }
  }
  return types;
}

bool ExpandDataAttributeType(const TypeTemplates& types,
                             const pugi::xml_node& attributeNode,
                             std::string_view accessPointName,
                             std::string_view parentRef,
                             FunctionalConstraint inheritedFc,
                             IEC61850Proto::SclIed* ied,
                             std::size_t* expandedCount,
                             std::size_t maxExpanded,
                             std::unordered_set<std::string>* activeTypes) {
  if (*expandedCount >= maxExpanded) {
    return true;
  }
  const auto name = Attribute(attributeNode, "name");
  if (name.empty()) {
    return true;
  }
  const std::string dataRef = std::format("{}.{}", parentRef, name);
  const std::string basicType = Attribute(attributeNode, "bType");
  auto fc = ParseFunctionalConstraint(Attribute(attributeNode, "fc"));
  if (fc == IEC61850Proto::FUNCTIONAL_CONSTRAINT_UNSPECIFIED) {
    fc = inheritedFc;
  }

  if (basicType == "Struct") {
    const auto typeRef = Attribute(attributeNode, "type");
    const auto it = types.dataAttributes.find(typeRef);
    if (it != types.dataAttributes.end()) {
      if (!activeTypes->emplace(typeRef).second) {
        return false;
      }
      for (const auto& bda : Children(it->second, "BDA")) {
        if (!ExpandDataAttributeType(types, bda, accessPointName, dataRef, fc,
                                     ied, expandedCount, maxExpanded,
                                     activeTypes)) {
          activeTypes->erase(typeRef);
          return false;
        }
      }
      activeTypes->erase(typeRef);
    }
    return true;
  }

  auto* attribute = ied->add_data_attributes();
  attribute->set_access_point(std::string(accessPointName));
  attribute->set_data_ref(dataRef);
  attribute->set_fc(fc);
  attribute->set_basic_type(basicType);
  attribute->set_type_ref(Attribute(attributeNode, "type"));
  attribute->set_count(static_cast<uint32_t>(UnsignedAttribute(attributeNode, "count")));
  attribute->set_description(Attribute(attributeNode, "desc"));
  ++*expandedCount;
  return true;
}

bool ExpandLogicalNode(const TypeTemplates& types,
                       const pugi::xml_node& logicalNode,
                       std::string_view accessPointName,
                       std::string_view nodeRef,
                       IEC61850Proto::SclIed* ied,
                       std::size_t* expandedCount,
                       std::size_t maxExpanded) {
  const auto lnType = Attribute(logicalNode, "lnType");
  const auto lnTypeIt = types.logicalNodes.find(lnType);
  if (lnTypeIt == types.logicalNodes.end()) {
    return true;
  }
  for (const auto& doNode : Children(lnTypeIt->second, "DO")) {
    if (*expandedCount >= maxExpanded) {
      return true;
    }
    const auto name = Attribute(doNode, "name");
    const auto typeRef = Attribute(doNode, "type");
    const auto doTypeIt = types.dataObjects.find(typeRef);
    if (name.empty() || doTypeIt == types.dataObjects.end()) {
      continue;
    }
    const std::string doRef = std::format("{}.{}", nodeRef, name);
    auto* dataObject = ied->add_data_objects();
    dataObject->set_access_point(std::string(accessPointName));
    dataObject->set_data_ref(doRef);
    dataObject->set_type_ref(typeRef);
    dataObject->set_cdc(Attribute(doTypeIt->second, "cdc"));
    dataObject->set_description(Attribute(doNode, "desc"));
    for (const auto& daNode : Children(doTypeIt->second, "DA")) {
      std::unordered_set<std::string> activeTypes;
      if (!ExpandDataAttributeType(
              types, daNode, accessPointName, doRef,
              IEC61850Proto::FUNCTIONAL_CONSTRAINT_UNSPECIFIED, ied,
              expandedCount, maxExpanded, &activeTypes)) {
        return false;
      }
    }
  }
  return true;
}

void ParseDataSet(const pugi::xml_node& dataSetNode,
                  std::string_view iedName,
                  std::string_view accessPointName,
                  std::string_view ownerNodeRef,
                  IEC61850Proto::SclIed* ied) {
  auto* dataSet = ied->add_data_sets();
  dataSet->set_access_point(std::string(accessPointName));
  dataSet->set_owner_node_ref(ownerNodeRef);
  dataSet->set_name(Attribute(dataSetNode, "name"));
  dataSet->set_data_set_ref(DataSetReference(ownerNodeRef, dataSet->name()));
  for (const auto& fcdaNode : Children(dataSetNode, "FCDA")) {
    auto* fcda = dataSet->add_members();
    fcda->set_ld_inst(Attribute(fcdaNode, "ldInst"));
    fcda->set_prefix(Attribute(fcdaNode, "prefix"));
    fcda->set_ln_class(Attribute(fcdaNode, "lnClass"));
    fcda->set_ln_inst(Attribute(fcdaNode, "lnInst"));
    fcda->set_do_name(Attribute(fcdaNode, "doName"));
    fcda->set_da_name(Attribute(fcdaNode, "daName"));
    fcda->set_fc(ParseFunctionalConstraint(Attribute(fcdaNode, "fc")));
    fcda->set_data_ref(FcdaReference(iedName, fcdaNode));
  }
}

void ParseReportControl(const pugi::xml_node& controlNode,
                        std::string_view accessPointName,
                        std::string_view ownerNodeRef,
                        IEC61850Proto::SclIed* ied) {
  auto* control = ied->add_report_controls();
  control->set_access_point(std::string(accessPointName));
  control->set_owner_node_ref(ownerNodeRef);
  control->set_name(Attribute(controlNode, "name"));
  control->set_buffered(BooleanAttribute(controlNode, "buffered"));
  control->set_rcb_ref(std::format("{}${}${}", ownerNodeRef,
                                   control->buffered() ? "BR" : "RP",
                                   control->name()));
  control->set_data_set_ref(DataSetReference(ownerNodeRef,
                                             Attribute(controlNode, "datSet")));
  control->set_report_id(Attribute(controlNode, "rptID"));
  control->set_config_revision(UnsignedAttribute(controlNode, "confRev"));
  control->set_integrity_period_ms(static_cast<uint32_t>(
      UnsignedAttribute(controlNode, "intgPd")));
  control->set_buffer_time_ms(static_cast<uint32_t>(
      UnsignedAttribute(controlNode, "bufTime")));
  const auto enabled = Child(controlNode, "RptEnabled");
  control->set_max_instances(static_cast<uint32_t>(
      UnsignedAttribute(enabled, "max", 1)));

  const auto trigger = Child(controlNode, "TrgOps");
  auto* triggerOut = control->mutable_trigger_options();
  triggerOut->set_data_change(BooleanAttribute(trigger, "dchg"));
  triggerOut->set_quality_change(BooleanAttribute(trigger, "qchg"));
  triggerOut->set_data_update(BooleanAttribute(trigger, "dupd"));
  triggerOut->set_integrity(BooleanAttribute(trigger, "period"));
  triggerOut->set_general_interrogation(BooleanAttribute(trigger, "gi"));

  const auto options = Child(controlNode, "OptFields");
  auto* optionsOut = control->mutable_optional_fields();
  optionsOut->set_sequence_number(BooleanAttribute(options, "seqNum"));
  optionsOut->set_report_timestamp(BooleanAttribute(options, "timeStamp"));
  optionsOut->set_reason_code(BooleanAttribute(options, "reasonCode"));
  optionsOut->set_data_set(BooleanAttribute(options, "dataSet"));
  optionsOut->set_data_reference(BooleanAttribute(options, "dataRef"));
  optionsOut->set_buffer_overflow(BooleanAttribute(options, "bufOvfl"));
  optionsOut->set_entry_id(BooleanAttribute(options, "entryID"));
  optionsOut->set_config_revision(BooleanAttribute(options, "configRef"));
  optionsOut->set_segmentation(BooleanAttribute(options, "segmentation"));
}

void ParseGseControl(const pugi::xml_node& controlNode,
                     std::string_view accessPointName,
                     std::string_view ownerNodeRef,
                     IEC61850Proto::SclIed* ied) {
  auto* control = ied->add_gse_controls();
  control->set_access_point(std::string(accessPointName));
  control->set_owner_node_ref(ownerNodeRef);
  control->set_name(Attribute(controlNode, "name"));
  control->set_control_ref(std::format("{}$GO${}", ownerNodeRef,
                                       control->name()));
  control->set_go_id(Attribute(controlNode, "appID"));
  control->set_data_set_ref(DataSetReference(ownerNodeRef,
                                             Attribute(controlNode, "datSet")));
  control->set_config_revision(UnsignedAttribute(controlNode, "confRev"));
  control->set_fixed_offsets(BooleanAttribute(controlNode, "fixedOffs"));
}

void ParseSampledValueControl(const pugi::xml_node& controlNode,
                              std::string_view accessPointName,
                              std::string_view ownerNodeRef,
                              IEC61850Proto::SclIed* ied) {
  auto* control = ied->add_sampled_value_controls();
  control->set_access_point(std::string(accessPointName));
  control->set_owner_node_ref(ownerNodeRef);
  control->set_name(Attribute(controlNode, "name"));
  control->set_control_ref(std::format("{}$MS${}", ownerNodeRef,
                                       control->name()));
  control->set_sv_id(Attribute(controlNode, "smvID"));
  control->set_data_set_ref(DataSetReference(ownerNodeRef,
                                             Attribute(controlNode, "datSet")));
  control->set_config_revision(UnsignedAttribute(controlNode, "confRev"));
  control->set_sample_rate(static_cast<uint32_t>(
      UnsignedAttribute(controlNode, "smpRate")));
  control->set_nof_asdu(static_cast<uint32_t>(
      UnsignedAttribute(controlNode, "nofASDU", 1)));
  control->set_multicast(BooleanAttribute(controlNode, "multicast", true));
}

void ParseInputs(const pugi::xml_node& inputsNode,
                 std::string_view accessPointName,
                 std::string_view ownerNodeRef,
                 IEC61850Proto::SclIed* ied) {
  for (const auto& extRefNode : Children(inputsNode, "ExtRef")) {
    auto* extRef = ied->add_ext_refs();
    extRef->set_access_point(std::string(accessPointName));
    extRef->set_owner_node_ref(ownerNodeRef);
    extRef->set_int_addr(Attribute(extRefNode, "intAddr"));
    extRef->set_ied_name(Attribute(extRefNode, "iedName"));
    extRef->set_ld_inst(Attribute(extRefNode, "ldInst"));
    extRef->set_prefix(Attribute(extRefNode, "prefix"));
    extRef->set_ln_class(Attribute(extRefNode, "lnClass"));
    extRef->set_ln_inst(Attribute(extRefNode, "lnInst"));
    extRef->set_do_name(Attribute(extRefNode, "doName"));
    extRef->set_da_name(Attribute(extRefNode, "daName"));
    extRef->set_fc(ParseFunctionalConstraint(Attribute(extRefNode, "fc")));
    extRef->set_service_type(Attribute(extRefNode, "serviceType"));
    extRef->set_src_ld_inst(Attribute(extRefNode, "srcLDInst"));
    extRef->set_src_prefix(Attribute(extRefNode, "srcPrefix"));
    extRef->set_src_ln_class(Attribute(extRefNode, "srcLNClass"));
    extRef->set_src_ln_inst(Attribute(extRefNode, "srcLNInst"));
    extRef->set_src_cb_name(Attribute(extRefNode, "srcCBName"));
    extRef->set_source_data_ref(ExtRefSourceReference(extRefNode));
  }
}

bool ParseIed(const pugi::xml_node& iedNode,
              const TypeTemplates& types,
              IEC61850Proto::SclIed* ied,
              std::size_t* expandedCount,
              std::size_t maxExpanded) {
  const auto iedName = Attribute(iedNode, "name");
  ied->set_name(iedName);
  ied->set_manufacturer(Attribute(iedNode, "manufacturer"));
  ied->set_type(Attribute(iedNode, "type"));
  ied->set_description(Attribute(iedNode, "desc"));

  for (const auto& accessPointNode : Children(iedNode, "AccessPoint")) {
    auto* accessPoint = ied->add_access_points();
    const auto accessPointName = Attribute(accessPointNode, "name");
    accessPoint->set_name(accessPointName);
    const auto server = Child(accessPointNode, "Server");
    accessPoint->set_has_server(static_cast<bool>(server));
    for (const auto& ldevice : Children(server, "LDevice")) {
      const auto ldInst = Attribute(ldevice, "inst");
      for (const auto& logicalNode : ldevice.children()) {
        if (!IsNode(logicalNode, "LN0") && !IsNode(logicalNode, "LN")) {
          continue;
        }
        const auto nodeRef = NodeReference(iedName, ldInst, logicalNode);
        auto* node = ied->add_logical_nodes();
        node->set_access_point(accessPointName);
        node->set_ied_name(iedName);
        node->set_ld_inst(ldInst);
        node->set_prefix(Attribute(logicalNode, "prefix"));
        node->set_ln_class(Attribute(logicalNode, "lnClass"));
        node->set_ln_inst(Attribute(logicalNode, "inst"));
        node->set_ln_type(Attribute(logicalNode, "lnType"));
        node->set_node_ref(nodeRef);

        for (const auto& child : logicalNode.children()) {
          if (IsNode(child, "DataSet")) {
            ParseDataSet(child, iedName, accessPointName, nodeRef, ied);
          } else if (IsNode(child, "ReportControl")) {
            ParseReportControl(child, accessPointName, nodeRef, ied);
          } else if (IsNode(child, "GSEControl")) {
            ParseGseControl(child, accessPointName, nodeRef, ied);
          } else if (IsNode(child, "SampledValueControl")) {
            ParseSampledValueControl(child, accessPointName, nodeRef, ied);
          } else if (IsNode(child, "Inputs")) {
            ParseInputs(child, accessPointName, nodeRef, ied);
          }
        }
        if (!ExpandLogicalNode(types, logicalNode, accessPointName, nodeRef, ied,
                               expandedCount, maxExpanded)) {
          return false;
        }
      }
    }
  }
  return true;
}

grpc::Status ValidateDataSetReferences(const NormalizedSclModel& model,
                                       std::vector<ValidationIssue>* issues) {
  for (const auto& ied : model.ieds()) {
    std::unordered_map<std::string, std::unordered_set<std::string>>
        dataSetReferences;
    for (const auto& dataSet : ied.data_sets()) {
      dataSetReferences[dataSet.access_point()].emplace(
          dataSet.data_set_ref());
    }
    const auto hasDataSet = [&dataSetReferences](std::string_view accessPoint,
                                                 std::string_view reference) {
      const auto accessPointIt =
          dataSetReferences.find(std::string(accessPoint));
      return accessPointIt != dataSetReferences.end() &&
             accessPointIt->second.contains(std::string(reference));
    };
    for (const auto& control : ied.report_controls()) {
      if (!hasDataSet(control.access_point(), control.data_set_ref())) {
        const auto path = std::format(
            "/SCL/IED[@name='{}']/AccessPoint[@name='{}']/ReportControl[@name='{}']",
            ied.name(), control.access_point(), control.name());
        AddIssue(issues, IEC61850Proto::VALIDATION_SEVERITY_ERROR,
                 "SCL_DANGLING_DATASET_REFERENCE", path,
                 std::format("ReportControl引用的数据集不存在: {}",
                             control.data_set_ref()));
        return Invalid("SCL包含悬空的ReportControl数据集引用");
      }
    }
    for (const auto& control : ied.gse_controls()) {
      if (!hasDataSet(control.access_point(), control.data_set_ref())) {
        AddIssue(issues, IEC61850Proto::VALIDATION_SEVERITY_ERROR,
                 "SCL_DANGLING_DATASET_REFERENCE",
                 std::format(
                     "/SCL/IED[@name='{}']/AccessPoint[@name='{}']/GSEControl[@name='{}']",
                     ied.name(), control.access_point(), control.name()),
                 std::format("GSEControl引用的数据集不存在: {}",
                             control.data_set_ref()));
        return Invalid("SCL包含悬空的GSEControl数据集引用");
      }
    }
    for (const auto& control : ied.sampled_value_controls()) {
      if (!hasDataSet(control.access_point(), control.data_set_ref())) {
        AddIssue(issues, IEC61850Proto::VALIDATION_SEVERITY_ERROR,
                 "SCL_DANGLING_DATASET_REFERENCE",
                 std::format(
                     "/SCL/IED[@name='{}']/AccessPoint[@name='{}']/SampledValueControl[@name='{}']",
                     ied.name(), control.access_point(), control.name()),
                 std::format("SampledValueControl引用的数据集不存在: {}",
                             control.data_set_ref()));
        return Invalid("SCL包含悬空的SampledValueControl数据集引用");
      }
    }
  }
  return grpc::Status::OK;
}

}  // namespace

SclParser::SclParser() : SclParser(Limits{}) {}

SclParser::SclParser(Limits limits) : limits_(limits) {}

grpc::Status SclParser::Parse(
    const std::string& modelName, const std::string& sourceName,
    std::string_view content, NormalizedSclModel* out,
    std::vector<ValidationIssue>* issues) const {
  if (out == nullptr || issues == nullptr) {
    return Invalid("输出模型和校验问题列表不能为空");
  }
  out->Clear();
  issues->clear();
  out->set_model_name(modelName);
  out->set_source_name(sourceName);
  out->set_document_kind(DocumentKind(sourceName));

  if (modelName.empty()) {
    return Invalid("model_name不能为空");
  }
  if (sourceName.empty()) {
    return Invalid("source_name不能为空");
  }
  if (content.empty()) {
    return Invalid("SCL内容不能为空");
  }
  if (content.size() > limits_.maxDocumentBytes) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "SCL内容超过大小上限");
  }

  pugi::xml_document document;
  const auto parsed = document.load_buffer(content.data(), content.size(),
                                           pugi::parse_default,
                                           pugi::encoding_utf8);
  if (!parsed) {
    AddIssue(issues, IEC61850Proto::VALIDATION_SEVERITY_ERROR,
             "SCL_XML_PARSE_ERROR", "/",
             std::format("XML解析失败: {}，偏移={}", parsed.description(),
                         parsed.offset));
    return Invalid("SCL XML解析失败");
  }

  pugi::xml_node scl;
  for (const auto& node : document.children()) {
    if (IsNode(node, "SCL")) {
      scl = node;
      break;
    }
  }
  if (!scl) {
    AddIssue(issues, IEC61850Proto::VALIDATION_SEVERITY_ERROR,
             "SCL_ROOT_MISSING", "/", "根元素必须为SCL");
    return Invalid("未找到SCL根元素");
  }

  out->set_namespace_uri(Attribute(scl, "xmlns"));
  out->set_revision(Attribute(scl, "revision"));
  out->set_source_checksum(Fnv1aChecksum(content));
  ParseCommunication(scl, out);
  const auto types = IndexTypeTemplates(scl);

  std::unordered_set<std::string> iedNames;
  std::size_t expandedCount = 0;
  const auto expansionDetectionLimit =
      limits_.maxExpandedDataAttributes ==
              std::numeric_limits<std::size_t>::max()
          ? limits_.maxExpandedDataAttributes
          : limits_.maxExpandedDataAttributes + 1;
  for (const auto& iedNode : Children(scl, "IED")) {
    const auto iedName = Attribute(iedNode, "name");
    if (!iedNames.emplace(iedName).second) {
      AddIssue(issues, IEC61850Proto::VALIDATION_SEVERITY_ERROR,
               "SCL_DUPLICATE_IED",
               std::format("/SCL/IED[@name='{}']", iedName),
               std::format("IED名称重复: {}", iedName));
      return Invalid("SCL包含重复IED名称");
    }
    if (iedNames.size() > limits_.maxIeds) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "SCL中的IED数量超过上限");
    }
    std::unordered_set<std::string> accessPointNames;
    for (const auto& accessPointNode : Children(iedNode, "AccessPoint")) {
      const auto accessPointName = Attribute(accessPointNode, "name");
      if (accessPointName.empty()) {
        AddIssue(
            issues, IEC61850Proto::VALIDATION_SEVERITY_ERROR,
            "SCL_ACCESS_POINT_NAME_EMPTY",
            std::format("/SCL/IED[@name='{}']/AccessPoint", iedName),
            std::format("IED {}中的AccessPoint名称不能为空", iedName));
        return Invalid("SCL包含名称为空的AccessPoint");
      }
      if (!accessPointNames.emplace(accessPointName).second) {
        AddIssue(
            issues, IEC61850Proto::VALIDATION_SEVERITY_ERROR,
            "SCL_DUPLICATE_ACCESS_POINT",
            std::format(
                "/SCL/IED[@name='{}']/AccessPoint[@name='{}']", iedName,
                accessPointName),
            std::format("IED {}中的AccessPoint名称重复: {}", iedName,
                        accessPointName));
        return Invalid("SCL包含重复AccessPoint名称");
      }
    }
    if (!ParseIed(iedNode, types, out->add_ieds(), &expandedCount,
                  expansionDetectionLimit)) {
      AddIssue(issues, IEC61850Proto::VALIDATION_SEVERITY_ERROR,
               "SCL_RECURSIVE_DA_TYPE", "/SCL/DataTypeTemplates/DAType",
               "DAType存在循环引用");
      return Invalid("SCL中的DAType存在循环引用");
    }
    if (expandedCount > limits_.maxExpandedDataAttributes) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "SCL展开后的数据属性数量超过上限");
    }
  }

  return ValidateDataSetReferences(*out, issues);
}

IEC61850Proto::SclModelSummary SclParser::BuildSummary(
    const NormalizedSclModel& model) {
  IEC61850Proto::SclModelSummary summary;
  summary.set_model_name(model.model_name());
  summary.set_source_name(model.source_name());
  summary.set_document_kind(model.document_kind());
  summary.set_source_checksum(model.source_checksum());
  summary.set_ied_count(static_cast<uint32_t>(model.ieds_size()));
  for (const auto& ied : model.ieds()) {
    summary.set_logical_node_count(summary.logical_node_count() +
                                   static_cast<uint32_t>(ied.logical_nodes_size()));
    summary.set_data_attribute_count(summary.data_attribute_count() +
                                     static_cast<uint32_t>(ied.data_attributes_size()));
    summary.set_data_set_count(summary.data_set_count() +
                               static_cast<uint32_t>(ied.data_sets_size()));
    summary.set_report_control_count(summary.report_control_count() +
                                     static_cast<uint32_t>(ied.report_controls_size()));
    summary.set_gse_control_count(summary.gse_control_count() +
                                  static_cast<uint32_t>(ied.gse_controls_size()));
    summary.set_sampled_value_control_count(
        summary.sampled_value_control_count() +
        static_cast<uint32_t>(ied.sampled_value_controls_size()));
    summary.set_external_reference_count(
        summary.external_reference_count() +
        static_cast<uint32_t>(ied.ext_refs_size()));
  }
  return summary;
}

}  // namespace IEC61850
