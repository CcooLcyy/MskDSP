#include "IEC61850MmsSession.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <limits>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>

#include "mskdsp/IEC61850Limits.hpp"

namespace IEC61850 {
namespace {

bool ContainsLogicalNode(const MmsOnlineDirectory& directory,
                         std::string_view nodeRef) {
  return std::ranges::find(directory.logicalNodeRefs, nodeRef) !=
         directory.logicalNodeRefs.end();
}

const MmsDirectoryDataAttribute* FindDataAttribute(
    const MmsOnlineDirectory& directory, std::string_view dataRef,
    IEC61850Proto::FunctionalConstraint fc) {
  const auto it = std::ranges::find_if(
      directory.dataAttributes, [&](const auto& attribute) {
        return attribute.dataRef == dataRef && attribute.fc == fc;
      });
  return it == directory.dataAttributes.end() ? nullptr : &*it;
}

std::string Upper(std::string_view value) {
  std::string result(value);
  for (auto& character : result) {
    character = static_cast<char>(
        std::toupper(static_cast<unsigned char>(character)));
  }
  return result;
}

std::optional<std::uint32_t> TrailingWidth(std::string_view value,
                                           std::size_t begin) {
  if (begin >= value.size()) {
    return std::nullopt;
  }
  std::uint32_t width = 0;
  const auto parsed = std::from_chars(value.data() + begin,
                                      value.data() + value.size(), width);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
      width == 0) {
    return std::nullopt;
  }
  return width;
}

bool MatchesScalarType(std::string_view basicType,
                       const MmsTypeSpecification& type) {
  const auto basic = Upper(basicType);
  if (basic == "BOOLEAN") {
    return type.kind == MmsTypeSpecificationKind::BOOLEAN;
  }
  if (basic.starts_with("INT")) {
    auto widthEnd = basic.size();
    bool unsignedType = false;
    if (widthEnd != 0 && basic.back() == 'U') {
      unsignedType = true;
      --widthEnd;
    }
    const auto width = TrailingWidth(basic.substr(0, widthEnd), 3);
    if (!width.has_value()) {
      return false;
    }
    const auto expectedKind = unsignedType
                                  ? MmsTypeSpecificationKind::UNSIGNED
                                  : MmsTypeSpecificationKind::INTEGER;
    return type.kind == expectedKind && type.width == *width;
  }
  if (basic.starts_with("FLOAT")) {
    const auto width = TrailingWidth(basic, 5);
    return width.has_value() && type.kind == MmsTypeSpecificationKind::FLOATING_POINT &&
           type.width == *width;
  }
  if (basic == "ENUM") {
    return type.kind == MmsTypeSpecificationKind::INTEGER ||
           type.kind == MmsTypeSpecificationKind::UNSIGNED;
  }
  if (basic.starts_with("VISSTRING")) {
    const auto width = TrailingWidth(basic, 9);
    return type.kind == MmsTypeSpecificationKind::VISIBLE_STRING &&
           (!width.has_value() || type.width == *width);
  }
  if (basic.starts_with("UNICODE") || basic.starts_with("UTF8")) {
    const auto prefix = basic.starts_with("UNICODE") ? 7u : 4u;
    const auto width = TrailingWidth(basic, prefix);
    return type.kind == MmsTypeSpecificationKind::UTF8_STRING &&
           (!width.has_value() || type.width == *width);
  }
  if (basic.starts_with("OCTET")) {
    const auto width = TrailingWidth(basic, 5);
    return type.kind == MmsTypeSpecificationKind::OCTET_STRING &&
           (!width.has_value() || type.width == *width);
  }
  if (basic == "QUALITY") {
    return type.kind == MmsTypeSpecificationKind::BIT_STRING &&
           type.width == 13;
  }
  if (basic == "TIMESTAMP") {
    return type.kind == MmsTypeSpecificationKind::BINARY_TIME ||
           type.kind == MmsTypeSpecificationKind::UTC_TIME;
  }
  if (basic == "DBPOS" || basic == "TCMD" || basic == "CHECK" ||
      basic == "CODEDEN" || basic == "CODECTL") {
    return type.kind == MmsTypeSpecificationKind::BIT_STRING;
  }
  if (basic == "OBJREF") {
    return type.kind == MmsTypeSpecificationKind::OBJECT_IDENTIFIER;
  }
  if (basic == "ENTRYID") {
    return type.kind == MmsTypeSpecificationKind::OCTET_STRING;
  }
  return false;
}

bool MatchesSclType(const IEC61850Proto::SclDataAttribute& attribute,
                    const MmsTypeSpecification& type) {
  if (attribute.count() > 1) {
    return type.kind == MmsTypeSpecificationKind::ARRAY &&
           type.elementCount == attribute.count() && type.elementType != nullptr &&
           MatchesScalarType(attribute.basic_type(), *type.elementType);
  }
  if (type.kind == MmsTypeSpecificationKind::ARRAY) {
    return attribute.count() == type.elementCount && type.elementType != nullptr &&
           MatchesScalarType(attribute.basic_type(), *type.elementType);
  }
  return MatchesScalarType(attribute.basic_type(), type);
}

const MmsDirectoryDataSet* FindDataSet(const MmsOnlineDirectory& directory,
                                       std::string_view dataSetRef) {
  const auto it = std::ranges::find(directory.dataSets, dataSetRef,
                                    &MmsDirectoryDataSet::dataSetRef);
  return it == directory.dataSets.end() ? nullptr : &*it;
}

const MmsDirectoryReportControl* FindReportControl(
    const MmsOnlineDirectory& directory, std::string_view rcbRef) {
  const auto it = std::ranges::find_if(
      directory.reportControls,
      [&](const auto& report) { return report.rcbRef == rcbRef; });
  return it == directory.reportControls.end() ? nullptr : &*it;
}

std::string SegmentKey(const MmsReportSegment& segment) {
  return std::format("{}\x1f{}", segment.reportRef, segment.sequenceNumber);
}

std::size_t SaturatingAdd(std::size_t left, std::size_t right) noexcept {
  if (right > std::numeric_limits<std::size_t>::max() - left) {
    return std::numeric_limits<std::size_t>::max();
  }
  return left + right;
}

std::size_t MmsValueBytes(const MmsValue& value) noexcept {
  if (const auto* text = std::get_if<std::string>(&value)) {
    return text->size();
  }
  if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&value)) {
    return bytes->size();
  }
  if (const auto* composite =
          std::get_if<std::shared_ptr<MmsCompositeValue>>(&value)) {
    if (*composite == nullptr) {
      return 0;
    }
    // encodedContent保存结构值内部的原始TLV内容；未填充时递归统计成员。
    if (!(*composite)->encodedContent.empty()) {
      return (*composite)->encodedContent.size();
    }
    std::size_t total = 0;
    for (const auto& element : (*composite)->elements) {
      total = SaturatingAdd(total, MmsValueBytes(element));
    }
    return total;
  }
  return 0;
}

std::size_t VariableValueBytes(const MmsDataValue& value) noexcept {
  return MmsValueBytes(value.value);
}

std::size_t SegmentValueBytes(const MmsReportSegment& segment) noexcept {
  constexpr std::size_t kValueFixedBytes = 128;
  std::size_t retained = 0;
  for (const auto& value : segment.values) {
    retained = SaturatingAdd(retained, kValueFixedBytes);
    retained = SaturatingAdd(retained, value.dataRef.size());
    retained = SaturatingAdd(retained, VariableValueBytes(value));
  }
  return retained;
}

std::size_t InitialReportBytes(const MmsReportSegment& segment) noexcept {
  constexpr std::size_t kReportFixedBytes = 128;
  return SaturatingAdd(
      SaturatingAdd(kReportFixedBytes, segment.reportRef.size()),
      segment.dataSetRef.size());
}

bool HasOversizedValue(const MmsReportSegment& segment) noexcept {
  return std::ranges::any_of(segment.values, [](const auto& value) {
    return VariableValueBytes(value) >
           mskdsp::kIec61850MaxMmsVariableValueBytes;
  });
}

std::int64_t SaturatingDeadline(std::int64_t now,
                               std::int64_t timeout) noexcept {
  if (now > std::numeric_limits<std::int64_t>::max() - timeout) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return now + timeout;
}

}  // namespace

MmsSessionContract::MmsSessionContract(const ProtocolIedPlan& plan) :
  plan_(plan) {}

grpc::Status MmsSessionContract::ValidateOnlineDirectory(
    const MmsOnlineDirectory& directory,
    MmsDirectoryValidationStage stage) {
  ResetReadiness();
  if (directory.iedName != plan_.config.ied_name() ||
      directory.accessPoint != plan_.config.access_point()) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        std::format("MMS在线目录IED或AccessPoint不匹配: 期望={}/{}，实际={}/{}",
                    plan_.config.ied_name(), plan_.config.access_point(),
                    directory.iedName, directory.accessPoint));
  }
  std::unordered_set<std::string> logicalNodes;
  for (const auto& nodeRef : directory.logicalNodeRefs) {
    if (!logicalNodes.emplace(nodeRef).second) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          std::format("MMS在线目录逻辑节点引用重复: {}",
                                      nodeRef));
    }
  }
  std::unordered_set<std::string> dataAttributes;
  for (const auto& attribute : directory.dataAttributes) {
    const auto key = std::format("{}#{}", attribute.dataRef,
                                 static_cast<int>(attribute.fc));
    if (!dataAttributes.emplace(key).second) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          std::format("MMS在线目录数据属性引用重复: {}", key));
    }
  }
  std::unordered_set<std::string> dataSets;
  for (const auto& dataSet : directory.dataSets) {
    if (!dataSets.emplace(dataSet.dataSetRef).second) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          std::format("MMS在线目录DataSet引用重复: {}",
                                      dataSet.dataSetRef));
    }
  }
  std::unordered_set<std::string> reportControls;
  for (const auto& report : directory.reportControls) {
    if (!reportControls.emplace(report.rcbRef).second) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          std::format("MMS在线目录ReportControl引用重复: {}",
                                      report.rcbRef));
    }
  }
  for (const auto& node : plan_.ied.logical_nodes()) {
    if (!ContainsLogicalNode(directory, node.node_ref())) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          std::format("MMS在线目录缺少逻辑节点: {}", node.node_ref()));
    }
  }
  for (const auto& attribute : plan_.ied.data_attributes()) {
    const auto* actual = FindDataAttribute(directory, attribute.data_ref(),
                                           attribute.fc());
    if (actual == nullptr) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          std::format("MMS在线目录缺少数据属性: {}#{}", attribute.data_ref(),
                      static_cast<int>(attribute.fc())));
    }
    if (actual->typeSpecification.has_value() &&
        !MatchesSclType(attribute, *actual->typeSpecification)) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          std::format("MMS在线数据属性类型不匹配: {}#{}，SCL类型={}",
                      attribute.data_ref(), static_cast<int>(attribute.fc()),
                      attribute.basic_type()));
    }
  }
  for (const auto& expected : plan_.ied.data_sets()) {
    const auto* actual = FindDataSet(directory, expected.data_set_ref());
    if (actual == nullptr) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          std::format("MMS在线目录缺少DataSet: {}", expected.data_set_ref()));
    }
    if (stage != MmsDirectoryValidationStage::BASE) {
      if (actual->members.size() !=
          static_cast<std::size_t>(expected.members_size())) {
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            std::format("MMS在线DataSet成员数量不一致: {}",
                        expected.data_set_ref()));
      }
      for (int index = 0; index < expected.members_size(); ++index) {
        const auto& expectedMember = expected.members(index);
        const auto& actualMember = actual->members[index];
        if (actualMember.dataRef != expectedMember.data_ref() ||
            actualMember.fc != expectedMember.fc()) {
          return grpc::Status(
              grpc::StatusCode::FAILED_PRECONDITION,
              std::format("MMS在线DataSet成员不一致: {}[{}]",
                          expected.data_set_ref(), index));
        }
      }
    }
  }
  if (stage == MmsDirectoryValidationStage::COMPLETE) {
    for (const auto& expected : plan_.ied.report_controls()) {
      if (expected.trigger_options().general_interrogation() &&
          !expected.optional_fields().reason_code()) {
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            std::format("MMS启用GI时必须在OptFlds启用ReasonCode: {}",
                        expected.rcb_ref()));
      }
      const auto* actual = FindReportControl(directory, expected.rcb_ref());
      if (actual == nullptr) {
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            std::format("MMS在线目录缺少ReportControl: {}",
                        expected.rcb_ref()));
      }
      if (actual->dataSetRef != expected.data_set_ref() ||
          actual->reportId != expected.report_id() ||
          actual->buffered != expected.buffered() ||
          actual->configRevision != expected.config_revision() ||
          actual->maxInstances != expected.max_instances() ||
          actual->integrityPeriodMs != expected.integrity_period_ms() ||
          actual->bufferTimeMs != expected.buffer_time_ms() ||
          actual->triggerOptions.SerializeAsString() !=
              expected.trigger_options().SerializeAsString() ||
          actual->optionalFields.SerializeAsString() !=
              expected.optional_fields().SerializeAsString()) {
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            std::format("MMS ReportControl参数或ConfRev不一致: {}",
                        expected.rcb_ref()));
      }
    }
  }
  directoryValidated_ = true;
  return grpc::Status::OK;
}

grpc::Status ParseMmsDomainObjectReference(std::string_view reference,
                                           MmsObjectName* objectName) {
  if (objectName == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "MMS Domain对象名输出参数为空");
  }
  *objectName = {};
  const auto separator = reference.find('/');
  if (separator == std::string_view::npos || separator == 0 ||
      separator + 1 >= reference.size() ||
      reference.find('/', separator + 1) != std::string_view::npos) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "SCL引用必须是唯一的Domain/Item结构");
  }
  objectName->type = MmsObjectNameType::DOMAIN_SPECIFIC;
  objectName->domain = std::string(reference.substr(0, separator));
  objectName->identifier = std::string(reference.substr(separator + 1));
  for (auto& character : objectName->identifier) {
    if (character == '.') {
      character = '$';
    }
  }
  if (objectName->identifier.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "SCL引用的MMS Item不能为空");
  }
  return grpc::Status::OK;
}

std::vector<MmsRcbActivationRequest>
MmsSessionContract::BuildRcbActivationRequests() const {
  std::vector<MmsRcbActivationRequest> requests;
  requests.reserve(plan_.ied.report_controls_size());
  for (const auto& control : plan_.ied.report_controls()) {
    auto& request = requests.emplace_back();
    request.rcbRef = control.rcb_ref();
    request.dataSetRef = control.data_set_ref();
    request.reportId = control.report_id();
    request.buffered = control.buffered();
    request.configRevision = control.config_revision();
    request.maxInstances = control.max_instances();
    request.integrityPeriodMs = control.integrity_period_ms();
    request.bufferTimeMs = control.buffer_time_ms();
    request.triggerOptions = control.trigger_options();
    request.optionalFields = control.optional_fields();
    request.generalInterrogation =
        control.trigger_options().general_interrogation();
  }
  return requests;
}

grpc::Status MmsSessionContract::MarkRcbEnabled(
    std::string_view rcbRef, std::uint64_t configRevision) {
  const auto it = std::ranges::find_if(
      plan_.ied.report_controls(),
      [&](const auto& report) { return report.rcb_ref() == rcbRef; });
  if (!directoryValidated_) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "MMS在线目录尚未核对通过，不能确认RCB启用");
  }
  if (it == plan_.ied.report_controls().end()) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        std::format("MMS启用确认中的RCB不在启动计划中: {}",
                                    rcbRef));
  }
  if (it->config_revision() != configRevision) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        std::format("MMS RCB启用确认的ConfRev不一致: {}", rcbRef));
  }
  enabledRcbs_.emplace(rcbRef);
  return grpc::Status::OK;
}

grpc::Status MmsSessionContract::MarkGeneralInterrogationRequested(
    std::string_view rcbRef) {
  const auto it = std::ranges::find_if(
      plan_.ied.report_controls(),
      [&](const auto& report) { return report.rcb_ref() == rcbRef; });
  if (!directoryValidated_) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "MMS在线目录尚未核对通过，不能记录GI请求");
  }
  if (it == plan_.ied.report_controls().end()) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        std::format("MMS GI请求中的RCB不在启动计划中: {}", rcbRef));
  }
  if (!it->trigger_options().general_interrogation()) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        std::format("MMS当前RCB未声明GI触发条件: {}", rcbRef));
  }
  if (!it->optional_fields().reason_code()) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        std::format("MMS启用GI时必须在OptFlds启用ReasonCode: {}", rcbRef));
  }
  if (!enabledRcbs_.contains(std::string(rcbRef))) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        std::format("MMS RCB尚未启用，不能记录GI请求: {}", rcbRef));
  }
  if (requestedGiRcbs_.contains(std::string(rcbRef))) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        std::format("MMS当前RCB已有待完成的GI请求: {}", rcbRef));
  }
  requestedGiRcbs_.emplace(rcbRef);
  return grpc::Status::OK;
}

grpc::Status MmsSessionContract::MarkGeneralInterrogationComplete(
    const MmsReportEvent& report) {
  const auto& rcbRef = report.reportRef;
  const auto it = std::ranges::find_if(
      plan_.ied.report_controls(),
      [&](const auto& report) { return report.rcb_ref() == rcbRef; });
  if (!directoryValidated_) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "MMS在线目录尚未核对通过，不能确认GI完成");
  }
  if (it == plan_.ied.report_controls().end()) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        std::format("MMS完成GI确认中的RCB不在启动计划中: {}", rcbRef));
  }
  if (!it->trigger_options().general_interrogation()) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        std::format("MMS当前RCB未声明GI触发条件: {}", rcbRef));
  }
  if (!enabledRcbs_.contains(std::string(rcbRef))) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        std::format("MMS RCB尚未启用，不能确认GI完成: {}", rcbRef));
  }
  if (!requestedGiRcbs_.contains(std::string(rcbRef))) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        std::format("MMS当前RCB没有待完成的GI请求: {}", rcbRef));
  }
  if (!it->optional_fields().reason_code() || !report.generalInterrogation) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        std::format("MMS GI报告缺少明确ReasonCode: {}", rcbRef));
  }
  if (report.dataSetRef != it->data_set_ref() ||
      report.confRev != it->config_revision()) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        std::format("MMS GI报告DataSet或ConfRev不一致: {}", rcbRef));
  }
  const auto dataSetIt = std::ranges::find_if(
      plan_.ied.data_sets(), [&](const auto& dataSet) {
        return dataSet.data_set_ref() == it->data_set_ref();
      });
  if (dataSetIt == plan_.ied.data_sets().end() ||
      report.values.size() !=
          static_cast<std::size_t>(dataSetIt->members_size())) {
    return grpc::Status(
        grpc::StatusCode::FAILED_PRECONDITION,
        std::format("MMS GI报告未覆盖完整DataSet: {}", rcbRef));
  }
  for (int index = 0; index < dataSetIt->members_size(); ++index) {
    const auto& expectedMember = dataSetIt->members(index);
    const auto& receivedMember = report.values[static_cast<std::size_t>(index)];
    if (receivedMember.dataRef != expectedMember.data_ref() ||
        receivedMember.fc != expectedMember.fc()) {
      return grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          std::format("MMS GI报告DataSet成员顺序不一致: {}[{}]", rcbRef,
                      index));
    }
  }
  requestedGiRcbs_.erase(std::string(rcbRef));
  completedGiRcbs_.emplace(rcbRef);
  return grpc::Status::OK;
}

bool MmsSessionContract::GeneralInterrogationPending(
    std::string_view rcbRef) const {
  return requestedGiRcbs_.contains(std::string(rcbRef));
}

bool MmsSessionContract::Ready() const {
  if (!directoryValidated_) {
    return false;
  }
  for (const auto& control : plan_.ied.report_controls()) {
    if (!enabledRcbs_.contains(control.rcb_ref())) {
      return false;
    }
    if (control.trigger_options().general_interrogation()) {
      if (requestedGiRcbs_.contains(control.rcb_ref()) ||
          !completedGiRcbs_.contains(control.rcb_ref())) {
        return false;
      }
    }
  }
  return true;
}

void MmsSessionContract::ResetReadiness() {
  enabledRcbs_.clear();
  requestedGiRcbs_.clear();
  completedGiRcbs_.clear();
  directoryValidated_ = false;
}

struct MmsReportAssembler::PendingReport {
  std::string key;
  std::string reportRef;
  std::string dataSetRef;
  uint64_t confRev = 0;
  uint64_t sequenceNumber = 0;
  uint32_t nextSegment = 0;
  int64_t receiveTimestampMs = 0;
  int64_t expiresAtMs = 0;
  bool generalInterrogation = false;
  std::size_t retainedBytes = 0;
  std::vector<MmsDataValue> values;
};

MmsReportAssembler::MmsReportAssembler(std::int64_t segmentTimeoutMs) :
  segmentTimeoutMs_(std::max<std::int64_t>(1, segmentTimeoutMs)) {}

MmsReportAssembler::~MmsReportAssembler() = default;

std::optional<MmsReportEvent> MmsReportAssembler::Push(
    MmsReportSegment segment, std::int64_t nowMs) {
  if (segment.reportRef.empty() || segment.dataSetRef.empty()) {
    return std::nullopt;
  }
  Expire(nowMs);
  const auto segmentBytes = SegmentValueBytes(segment);
  if (HasOversizedValue(segment) ||
      segmentBytes > mskdsp::kIec61850MaxMmsReportRetainedBytes) {
    return std::nullopt;
  }
  const auto key = SegmentKey(segment);
  auto it = std::ranges::find(pending_, key, &PendingReport::key);
  if (it == pending_.end()) {
    if (segment.segmentNumber != 0) {
      return std::nullopt;
    }
    if (pending_.size() >= mskdsp::kIec61850MaxMmsPendingReportGroups) {
      pending_.erase(pending_.begin());
    }
    PendingReport pending;
    pending.key = key;
    pending.reportRef = segment.reportRef;
    pending.dataSetRef = segment.dataSetRef;
    pending.confRev = segment.confRev;
    pending.sequenceNumber = segment.sequenceNumber;
    pending.nextSegment = 0;
    pending.receiveTimestampMs = segment.receiveTimestampMs;
    pending.generalInterrogation = segment.generalInterrogation;
    pending.expiresAtMs = SaturatingDeadline(nowMs, segmentTimeoutMs_);
    pending.retainedBytes = InitialReportBytes(segment);
    if (pending.retainedBytes > mskdsp::kIec61850MaxMmsReportRetainedBytes ||
        segmentBytes >
            mskdsp::kIec61850MaxMmsReportRetainedBytes -
                pending.retainedBytes) {
      return std::nullopt;
    }
    it = pending_.insert(pending_.end(), std::move(pending));
  }
  if (it->confRev != segment.confRev ||
      it->dataSetRef != segment.dataSetRef ||
      it->nextSegment != segment.segmentNumber) {
    pending_.erase(it);
    return std::nullopt;
  }
  if (segmentBytes >
      mskdsp::kIec61850MaxMmsReportRetainedBytes - it->retainedBytes) {
    pending_.erase(it);
    return std::nullopt;
  }
  it->values.insert(it->values.end(), segment.values.begin(),
                    segment.values.end());
  it->expiresAtMs = SaturatingDeadline(nowMs, segmentTimeoutMs_);
  it->generalInterrogation =
      it->generalInterrogation || segment.generalInterrogation;
  it->retainedBytes += segmentBytes;
  ++it->nextSegment;
  if (segment.moreSegmentsFollow) {
    return std::nullopt;
  }
  MmsReportEvent report;
  report.reportRef = std::move(it->reportRef);
  report.dataSetRef = std::move(it->dataSetRef);
  report.confRev = it->confRev;
  report.sequenceNumber = it->sequenceNumber;
  report.receiveTimestampMs = it->receiveTimestampMs;
  report.generalInterrogation = it->generalInterrogation;
  report.values = std::move(it->values);
  pending_.erase(it);
  return report;
}

void MmsReportAssembler::Expire(std::int64_t nowMs) {
  std::erase_if(pending_, [&](const PendingReport& pending) {
    return nowMs >= pending.expiresAtMs;
  });
}

void MmsReportAssembler::Clear() { pending_.clear(); }

}  // namespace IEC61850
