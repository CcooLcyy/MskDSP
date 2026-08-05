#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "IEC61850MmsBer.h"
#include "IEC61850MmsControl.h"
#include "IEC61850ProtocolStack.h"

namespace {

void Append(std::vector<std::uint8_t>* output,
            const std::vector<std::uint8_t>& value) {
  output->insert(output->end(), value.begin(), value.end());
}

std::vector<std::uint8_t> Tlv(std::uint8_t tag,
                              std::vector<std::uint8_t> value) {
  EXPECT_LT(value.size(), 128u);
  std::vector<std::uint8_t> output;
  output.reserve(value.size() + 2);
  output.push_back(tag);
  output.push_back(static_cast<std::uint8_t>(value.size()));
  Append(&output, value);
  return output;
}

std::vector<std::uint8_t> Bytes(std::string_view value) {
  return std::vector<std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(value.data()),
      reinterpret_cast<const std::uint8_t*>(value.data() + value.size()));
}

std::vector<std::uint8_t> DomainObjectName(std::string_view domain,
                                           std::string_view identifier) {
  std::vector<std::uint8_t> object;
  Append(&object, Tlv(0x1a, Bytes(domain)));
  Append(&object, Tlv(0x1a, Bytes(identifier)));
  return Tlv(0xa1, std::move(object));
}

std::vector<std::uint8_t> VariableSpecification(
    const std::vector<std::uint8_t>& objectName) {
  return Tlv(0x30, Tlv(0xa0, objectName));
}

std::vector<std::uint8_t> VmdVariableSpecification(
    std::string_view identifier) {
  return VariableSpecification(Tlv(0x80, Bytes(identifier)));
}

std::vector<std::uint8_t> LastApplErrorData(std::string_view controlReference,
                                           std::int64_t error,
                                           std::uint8_t ctlNum,
                                           std::int64_t addCause) {
  std::vector<std::uint8_t> origin;
  Append(&origin, Tlv(0x85, {static_cast<std::uint8_t>(2)}));
  Append(&origin, Tlv(0x89, {0x01, 0x02, 0x03, 0x04}));
  std::vector<std::uint8_t> data;
  Append(&data, Tlv(0x8a, Bytes(controlReference)));
  Append(&data, Tlv(0x85, {static_cast<std::uint8_t>(error)}));
  Append(&data, Tlv(0xa2, std::move(origin)));
  Append(&data, Tlv(0x86, {ctlNum}));
  Append(&data, Tlv(0x85, {static_cast<std::uint8_t>(addCause)}));
  return Tlv(0xa2, std::move(data));
}

std::vector<std::uint8_t> CommandTerminationReport(
    std::string_view operationReference, std::uint8_t controlNumber,
    bool includeError = false, std::string_view errorReference = {}) {
  const auto separator = operationReference.find('/');
  EXPECT_NE(separator, std::string_view::npos);
  std::vector<std::uint8_t> variableList;
  if (includeError) {
    Append(&variableList, VmdVariableSpecification("LastApplError"));
  }
  Append(&variableList,
         VariableSpecification(DomainObjectName(
             operationReference.substr(0, separator),
             operationReference.substr(separator + 1))));

  std::vector<std::uint8_t> values;
  if (includeError) {
    Append(&values, LastApplErrorData(errorReference, 1, controlNumber, 10));
  }
  Append(&values, Tlv(0x83, {0xff}));

  std::vector<std::uint8_t> report;
  Append(&report, Tlv(0xa0, std::move(variableList)));
  Append(&report, Tlv(0xa1, std::move(values)));
  return Tlv(0xa3, Tlv(0xa0, std::move(report)));
}

IEC61850::MmsObjectName ControlObject() {
  IEC61850::MmsObjectName object;
  object.type = IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC;
  object.domain = "LD0";
  object.identifier = "CSWI1$Pos";
  return object;
}

IEC61850::MmsControlCommand OperateCommand(
    IEC61850::MmsControlOperation operation) {
  IEC61850::MmsControlCommand command;
  command.operation = operation;
  command.controlObject = ControlObject();
  command.controlValue = {0x83, 0x01, 0xff};
  command.controlNumber = 7;
  command.originCategory = 2;
  command.originIdentifier = {192, 0, 2, 10};
  command.timestampMs = 1123;
  command.test = false;
  command.check = 1;
  return command;
}

IEC61850::MmsObjectName DiscoveredControlObject() {
  auto object = ControlObject();
  object.domain = "IED1LD0";
  return object;
}

IEC61850::ProtocolIedPlan ControlPlan() {
  IEC61850::ProtocolIedPlan plan;
  plan.config.set_ied_name("IED1");
  plan.config.set_access_point("AP1");
  auto* object = plan.ied.add_data_objects();
  object->set_data_ref("IED1LD0/CSWI1.Pos");
  object->set_cdc("DPC");
  object->set_access_point("AP1");
  auto* attribute = plan.ied.add_data_attributes();
  attribute->set_data_ref("IED1LD0/CSWI1.Pos.ctlVal");
  attribute->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_CO);
  attribute->set_basic_type("BOOLEAN");
  attribute->set_access_point("AP1");
  return plan;
}

IEC61850::MmsOnlineDirectory ControlDirectory() {
  IEC61850::MmsOnlineDirectory directory;
  directory.iedName = "IED1";
  directory.accessPoint = "AP1";
  for (const auto identifier : {"CSWI1$Pos$SBO", "CSWI1$Pos$SBOw",
                                "CSWI1$Pos$Cancel", "CSWI1$Pos$Oper"}) {
    IEC61850::MmsObjectName object;
    object.type = IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC;
    object.domain = "IED1LD0";
    object.identifier = identifier;
    directory.namedVariables.emplace_back(std::move(object));
  }
  directory.dataAttributes.push_back(
      {"IED1LD0/CSWI1.Pos.ctlVal",
       IEC61850Proto::FUNCTIONAL_CONSTRAINT_CO,
       IEC61850::MmsTypeSpecification{.kind =
                                          IEC61850::MmsTypeSpecificationKind::BOOLEAN,
                                      .width = 1}});
  return directory;
}

IEC61850::ProtocolIedPlan ControlPlanWithOnlineTiming() {
  auto plan = ControlPlan();
  for (const auto [ref, fc] : {
           std::pair<const char*, IEC61850Proto::FunctionalConstraint>{
               "IED1LD0/CSWI1.Pos.ctlModel",
               IEC61850Proto::FUNCTIONAL_CONSTRAINT_CF},
           {"IED1LD0/CSWI1.Pos.sboTimeout",
            IEC61850Proto::FUNCTIONAL_CONSTRAINT_CF},
           {"IED1LD0/CSWI1.Pos.operTimeout",
            IEC61850Proto::FUNCTIONAL_CONSTRAINT_CF}}) {
    auto* attribute = plan.ied.add_data_attributes();
    attribute->set_data_ref(ref);
    attribute->set_fc(fc);
    attribute->set_basic_type("INT32");
    attribute->set_access_point("AP1");
  }
  return plan;
}

IEC61850::MmsOnlineDirectory ControlDirectoryWithOnlineTiming() {
  auto directory = ControlDirectory();
  std::vector<std::uint8_t> encoded;
  if (!IEC61850::EncodeMmsDataSigned(2, &encoded).ok()) {
    return {};
  }
  directory.dataAttributes.push_back(
      {"IED1LD0/CSWI1.Pos.ctlModel",
       IEC61850Proto::FUNCTIONAL_CONSTRAINT_CF,
       IEC61850::MmsTypeSpecification{.kind =
                                          IEC61850::MmsTypeSpecificationKind::INTEGER,
                                      .width = 32},
       encoded});
  if (!IEC61850::EncodeMmsDataUnsigned(1234, &encoded).ok()) {
    return {};
  }
  directory.dataAttributes.push_back(
      {"IED1LD0/CSWI1.Pos.sboTimeout",
       IEC61850Proto::FUNCTIONAL_CONSTRAINT_CF,
       IEC61850::MmsTypeSpecification{.kind =
                                          IEC61850::MmsTypeSpecificationKind::UNSIGNED,
                                      .width = 32},
       encoded});
  if (!IEC61850::EncodeMmsDataUnsigned(2345, &encoded).ok()) {
    return {};
  }
  directory.dataAttributes.push_back(
      {"IED1LD0/CSWI1.Pos.operTimeout",
       IEC61850Proto::FUNCTIONAL_CONSTRAINT_CF,
       IEC61850::MmsTypeSpecification{.kind =
                                          IEC61850::MmsTypeSpecificationKind::UNSIGNED,
                                      .width = 32},
       encoded});
  return directory;
}

IEC61850::MmsOnlineDirectory ControlDirectoryForCtlModel(
    std::int64_t ctlModel,
    std::initializer_list<std::string_view> memberSuffixes) {
  auto directory = ControlDirectoryWithOnlineTiming();
  directory.namedVariables.clear();
  for (const auto suffix : memberSuffixes) {
    IEC61850::MmsObjectName object;
    object.type = IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC;
    object.domain = "IED1LD0";
    object.identifier = std::string("CSWI1$Pos") + std::string(suffix);
    directory.namedVariables.emplace_back(std::move(object));
  }
  if (!IEC61850::EncodeMmsDataSigned(
           ctlModel, &directory.dataAttributes[1].encodedValue)
           .ok()) {
    return {};
  }
  return directory;
}

std::vector<std::uint8_t> DecodeControlStructureTags(
    std::span<const std::uint8_t> encoded) {
  std::size_t offset = 0;
  IEC61850::BerTlvView structure;
  if (!IEC61850::ReadBerTlv(encoded, &offset, &structure).ok() ||
      offset != encoded.size() || structure.tag != 0xa2) {
    return {};
  }
  std::vector<std::uint8_t> tags;
  offset = 0;
  while (offset < structure.value.size()) {
    IEC61850::BerTlvView field;
    if (!IEC61850::ReadBerTlv(structure.value, &offset, &field).ok()) {
      return {};
    }
    tags.emplace_back(field.tag);
  }
  return tags;
}

// 验证在线控制成员按SCL的FC=CO数据对象编译为能力模型。
TEST(IEC61850MmsControlTest, DiscoversOnlineControlCapabilities) {
  IEC61850::MmsControlModel model;
  ASSERT_TRUE(IEC61850::BuildMmsControlModel(ControlPlan(), ControlDirectory(),
                                             &model)
                  .ok());
  ASSERT_EQ(model.controls.size(), 1u);
  const auto* capability = model.Find(DiscoveredControlObject());
  ASSERT_NE(capability, nullptr);
  EXPECT_TRUE(capability->supportsSbo);
  EXPECT_TRUE(capability->supportsSboWithValue);
  EXPECT_TRUE(capability->supportsOperate);
  EXPECT_TRUE(capability->supportsCancel);
  ASSERT_TRUE(capability->ctlValType.has_value());
  EXPECT_EQ(capability->ctlValType->kind,
            IEC61850::MmsTypeSpecificationKind::BOOLEAN);
}

// 验证在线ctlModel、sboTimeout和operTimeout进入每个控制对象能力模型。
TEST(IEC61850MmsControlTest, DiscoversOnlineControlTiming) {
  IEC61850::MmsControlModel model;
  ASSERT_TRUE(IEC61850::BuildMmsControlModel(
                  ControlPlanWithOnlineTiming(),
                  ControlDirectoryWithOnlineTiming(), &model)
                  .ok());
  const auto* capability = model.Find(DiscoveredControlObject());
  ASSERT_NE(capability, nullptr);
  ASSERT_TRUE(capability->ctlModel.has_value());
  EXPECT_EQ(*capability->ctlModel, 2);
  ASSERT_TRUE(capability->sboTimeoutMs.has_value());
  EXPECT_EQ(*capability->sboTimeoutMs, 1234u);
  ASSERT_TRUE(capability->operTimeoutMs.has_value());
  EXPECT_EQ(*capability->operTimeoutMs, 2345u);
}

// 验证不同控制操作分别使用在线SBO或Oper等待窗口，缺少参数时回退默认值。
TEST(IEC61850MmsControlTest, ResolvesOperationSpecificTimeout) {
  IEC61850::MmsControlCapability capability;
  capability.sboTimeoutMs = 1234;
  capability.operTimeoutMs = 2345;
  const auto fallback = std::chrono::milliseconds(5000);

  EXPECT_EQ(IEC61850::ResolveMmsControlTimeout(
                capability, IEC61850::MmsControlOperation::SELECT_WITH_VALUE,
                fallback),
            std::chrono::milliseconds(1234));
  EXPECT_EQ(IEC61850::ResolveMmsControlTimeout(
                capability, IEC61850::MmsControlOperation::OPERATE, fallback),
            std::chrono::milliseconds(2345));
  EXPECT_EQ(IEC61850::ResolveMmsControlTimeout(
                capability, IEC61850::MmsControlOperation::CANCEL, fallback),
            std::chrono::milliseconds(2345));
  capability.sboTimeoutMs.reset();
  capability.operTimeoutMs.reset();
  EXPECT_EQ(IEC61850::ResolveMmsControlTimeout(
                capability, IEC61850::MmsControlOperation::OPERATE, fallback),
            fallback);
}

// 验证在线控制参数为零时拒绝建立能力模型，不能默认为无限或立即过期。
TEST(IEC61850MmsControlTest, RejectsInvalidOnlineControlTiming) {
  auto directory = ControlDirectoryWithOnlineTiming();
  ASSERT_TRUE(IEC61850::EncodeMmsDataUnsigned(
                   0, &directory.dataAttributes[2].encodedValue)
                   .ok());
  IEC61850::MmsControlModel model;
  const auto status = IEC61850::BuildMmsControlModel(
      ControlPlanWithOnlineTiming(), directory, &model);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_TRUE(model.controls.empty());
}

// 验证在线控制参数TypeSpecification必须与ctlModel和超时参数的标准类型一致。
TEST(IEC61850MmsControlTest, RejectsOnlineControlParameterTypeMismatch) {
  for (const std::size_t attributeIndex : {1u, 2u, 3u}) {
    auto directory = ControlDirectoryWithOnlineTiming();
    ASSERT_TRUE(directory.dataAttributes[attributeIndex]
                    .typeSpecification.has_value());
    directory.dataAttributes[attributeIndex].typeSpecification->kind =
        IEC61850::MmsTypeSpecificationKind::BOOLEAN;
    IEC61850::MmsControlModel model;
    const auto status = IEC61850::BuildMmsControlModel(
        ControlPlanWithOnlineTiming(), directory, &model);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_TRUE(model.controls.empty());
  }
}

// 验证在线控制参数缺少TypeSpecification时拒绝建立能力模型。
TEST(IEC61850MmsControlTest, RejectsMissingOnlineControlParameterType) {
  for (const std::size_t attributeIndex : {1u, 2u, 3u}) {
    auto directory = ControlDirectoryWithOnlineTiming();
    directory.dataAttributes[attributeIndex].typeSpecification.reset();
    IEC61850::MmsControlModel model;
    const auto status = IEC61850::BuildMmsControlModel(
        ControlPlanWithOnlineTiming(), directory, &model);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_TRUE(model.controls.empty());
  }
}

// 验证在线控制参数编码必须与已声明的INTEGER/UNSIGNED类型一致。
TEST(IEC61850MmsControlTest, RejectsOnlineControlParameterEncodedTypeMismatch) {
  auto directory = ControlDirectoryWithOnlineTiming();
  ASSERT_TRUE(IEC61850::EncodeMmsDataUnsigned(
                  2, &directory.dataAttributes[1].encodedValue)
                  .ok());
  IEC61850::MmsControlModel model;
  auto status = IEC61850::BuildMmsControlModel(
      ControlPlanWithOnlineTiming(), directory, &model);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_TRUE(model.controls.empty());

  directory = ControlDirectoryWithOnlineTiming();
  ASSERT_TRUE(IEC61850::EncodeMmsDataSigned(
                  1234, &directory.dataAttributes[2].encodedValue)
                  .ok());
  status = IEC61850::BuildMmsControlModel(
      ControlPlanWithOnlineTiming(), directory, &model);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_TRUE(model.controls.empty());
}

// 验证normal/enhanced direct ctlModel覆盖名称推断，只允许无需选择的Oper。
TEST(IEC61850MmsControlTest, AppliesDirectCtlModelSemantics) {
  for (const std::int64_t ctlModel : {1, 3}) {
    auto directory = ControlDirectoryForCtlModel(
        ctlModel, {"$SBO", "$SBOw", "$Oper", "$Cancel"});
    IEC61850::MmsControlModel model;
    ASSERT_TRUE(IEC61850::BuildMmsControlModel(
                    ControlPlanWithOnlineTiming(), directory, &model)
                    .ok());
    const auto* capability = model.Find(DiscoveredControlObject());
    ASSERT_NE(capability, nullptr);
    EXPECT_FALSE(capability->supportsSbo);
    EXPECT_FALSE(capability->supportsSboWithValue);
    EXPECT_TRUE(capability->supportsOperate);
    EXPECT_FALSE(capability->supportsCancel);
    EXPECT_EQ(capability->ctlModel,
              std::optional<std::int64_t>(ctlModel));

    IEC61850::MmsSboState state;
    EXPECT_TRUE(IEC61850::ValidateMmsControlOperation(
                    model, DiscoveredControlObject(),
                    IEC61850::MmsControlOperation::OPERATE, state, 1000)
                    .ok());
    EXPECT_EQ(IEC61850::ValidateMmsControlOperation(
                  model, DiscoveredControlObject(),
                  IEC61850::MmsControlOperation::SELECT, state, 1000)
                  .error_code(),
              grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_EQ(IEC61850::ValidateMmsControlOperation(
                  model, DiscoveredControlObject(),
                  IEC61850::MmsControlOperation::CANCEL, state, 1000)
                  .error_code(),
              grpc::StatusCode::FAILED_PRECONDITION);
  }
}

// 验证ctlModel=0保留在线对象诊断，但所有专用控制操作都被拒绝。
TEST(IEC61850MmsControlTest, AppliesStatusOnlyCtlModelSemantics) {
  IEC61850::MmsControlModel model;
  ASSERT_TRUE(IEC61850::BuildMmsControlModel(
                  ControlPlanWithOnlineTiming(),
                  ControlDirectoryForCtlModel(0, {}), &model)
                  .ok());
  const auto* capability = model.Find(DiscoveredControlObject());
  ASSERT_NE(capability, nullptr);
  EXPECT_FALSE(capability->supportsSbo);
  EXPECT_FALSE(capability->supportsSboWithValue);
  EXPECT_FALSE(capability->supportsOperate);
  EXPECT_FALSE(capability->supportsCancel);
  IEC61850::MmsSboState state;
  EXPECT_EQ(IEC61850::ValidateMmsControlOperation(
                model, DiscoveredControlObject(),
                IEC61850::MmsControlOperation::OPERATE, state, 1000)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证普通SBO和增强SBO分别只开放$SBO或$SBOw，并都要求Oper和有效选择保持。
TEST(IEC61850MmsControlTest, AppliesSboCtlModelOperationMatrix) {
  for (const auto [ctlModel, selectOperation] : {
           std::pair{std::int64_t{2}, IEC61850::MmsControlOperation::SELECT},
           std::pair{std::int64_t{4},
                     IEC61850::MmsControlOperation::SELECT_WITH_VALUE}}) {
    const auto directory =
        ctlModel == 2
            ? ControlDirectoryForCtlModel(2, {"$SBO", "$Oper", "$Cancel"})
            : ControlDirectoryForCtlModel(4,
                                          {"$SBOw", "$Oper", "$Cancel"});
    IEC61850::MmsControlModel model;
    ASSERT_TRUE(IEC61850::BuildMmsControlModel(
                    ControlPlanWithOnlineTiming(), directory, &model)
                    .ok());
    const auto* capability = model.Find(DiscoveredControlObject());
    ASSERT_NE(capability, nullptr);
    EXPECT_EQ(capability->supportsSbo, ctlModel == 2);
    EXPECT_EQ(capability->supportsSboWithValue, ctlModel == 4);
    EXPECT_TRUE(capability->supportsOperate);
    EXPECT_TRUE(capability->supportsCancel);

    IEC61850::MmsSboState state;
    ASSERT_TRUE(IEC61850::ValidateMmsControlOperation(
                    model, DiscoveredControlObject(), selectOperation, state,
                    1000)
                    .ok());
    EXPECT_EQ(IEC61850::ValidateMmsControlOperation(
                  model, DiscoveredControlObject(),
                  IEC61850::MmsControlOperation::OPERATE, state, 1000)
                  .error_code(),
              grpc::StatusCode::FAILED_PRECONDITION);
    ASSERT_TRUE(state.RecordSelection(DiscoveredControlObject(), 1000).ok());
    EXPECT_TRUE(IEC61850::ValidateMmsControlOperation(
                    model, DiscoveredControlObject(),
                    IEC61850::MmsControlOperation::OPERATE, state, 1001)
                    .ok());
  }
}

// 验证ctlModel要求的普通或增强SBO在线成员缺失时拒绝建立能力模型。
TEST(IEC61850MmsControlTest, RejectsCtlModelMemberMismatch) {
  for (const auto directory : {
           ControlDirectoryForCtlModel(2, {"$SBOw", "$Oper"}),
           ControlDirectoryForCtlModel(4, {"$SBO", "$Oper"}),
           ControlDirectoryForCtlModel(1, {"$SBO"})}) {
    IEC61850::MmsControlModel model;
    const auto status = IEC61850::BuildMmsControlModel(
        ControlPlanWithOnlineTiming(), directory, &model);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_TRUE(model.controls.empty());
  }
}

// 验证控制命令的ctlVal必须匹配在线TypeSpecification，不能按任意Data标签发送。
TEST(IEC61850MmsControlTest, RejectsCtlValueTypeMismatch) {
  IEC61850::MmsControlModel model;
  ASSERT_TRUE(IEC61850::BuildMmsControlModel(ControlPlan(), ControlDirectory(),
                                             &model)
                  .ok());
  const std::vector<std::uint8_t> integerValue{0x85, 0x01, 0x01};
  const auto status = IEC61850::ValidateMmsControlValue(
      model, DiscoveredControlObject(), integerValue);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证计划内控制对象缺少在线操作成员时不会被静默降级为可控对象。
TEST(IEC61850MmsControlTest, RejectsIncompleteOnlineControlCapabilities) {
  auto directory = ControlDirectory();
  directory.namedVariables.pop_back();
  IEC61850::MmsControlModel model;
  const auto status = IEC61850::BuildMmsControlModel(ControlPlan(), directory,
                                                     &model);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_TRUE(model.controls.empty());
}

// 验证SBO选择保持只在有效期限内成立，并在取消后失效。
TEST(IEC61850MmsControlTest, SboSelectionExpiresAndCanBeCleared) {
  IEC61850::MmsSboState state(std::chrono::milliseconds(100));
  const auto object = DiscoveredControlObject();
  EXPECT_FALSE(state.IsSelected(object, 1000));
  ASSERT_TRUE(state.RecordSelection(object, 1000).ok());
  EXPECT_TRUE(state.IsSelected(object, 1099));
  EXPECT_FALSE(state.IsSelected(object, 1100));
  ASSERT_TRUE(state.RecordSelection(object, 2000).ok());
  state.ClearSelection(object);
  EXPECT_FALSE(state.IsSelected(object, 2000));
}

// 验证同一控制对象只能建立一个待完成Oper，并支持发送前回滚而不丢失SBO选择。
TEST(IEC61850MmsControlTest, ReservesOnePendingOperationPerObject) {
  IEC61850::MmsSboState state;
  const auto object = DiscoveredControlObject();
  ASSERT_TRUE(state.RecordSelection(object, 1000).ok());
  ASSERT_TRUE(state.RecordPendingOperation(object).ok());
  EXPECT_TRUE(state.IsOperationPending(object));
  EXPECT_EQ(state.RecordPendingOperation(object).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);

  state.ClearPendingOperation(object);
  EXPECT_FALSE(state.IsOperationPending(object));
  EXPECT_TRUE(state.IsSelected(object, 1000));
}

// 验证明确的远端控制拒绝保留选择并要求Cancel，不能再次执行Oper。
TEST(IEC61850MmsControlTest, RejectedOperationRequiresCancel) {
  IEC61850::MmsControlModel model;
  ASSERT_TRUE(IEC61850::BuildMmsControlModel(ControlPlan(), ControlDirectory(),
                                             &model)
                  .ok());
  const auto object = DiscoveredControlObject();
  IEC61850::MmsSboState state;
  ASSERT_TRUE(state.RecordSelection(object, 1000).ok());
  ASSERT_TRUE(state.RecordPendingOperation(object).ok());
  ASSERT_TRUE(state.MarkOperationRejected(object).ok());
  EXPECT_FALSE(state.IsOperationPending(object));
  EXPECT_TRUE(state.IsCancelRequired(object));
  EXPECT_TRUE(state.IsSelected(object, 1000));
  EXPECT_EQ(IEC61850::ValidateMmsControlOperation(
                model, object, IEC61850::MmsControlOperation::OPERATE, state,
                1000)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_TRUE(IEC61850::ValidateMmsControlOperation(
                  model, object, IEC61850::MmsControlOperation::CANCEL, state,
                  1000)
                  .ok());
  state.ClearSelection(object);
  EXPECT_FALSE(state.IsCancelRequired(object));
}

// 验证不确定结果会锁定对象，且不会被发送前回滚误清除。
TEST(IEC61850MmsControlTest, UncertainOperationBlocksFurtherControl) {
  IEC61850::MmsControlModel model;
  ASSERT_TRUE(IEC61850::BuildMmsControlModel(ControlPlan(), ControlDirectory(),
                                             &model)
                  .ok());
  const auto object = DiscoveredControlObject();
  IEC61850::MmsSboState state;
  ASSERT_TRUE(state.RecordSelection(object, 1000).ok());
  ASSERT_TRUE(state.RecordPendingOperation(object).ok());
  ASSERT_TRUE(state.MarkUncertain(object).ok());
  EXPECT_TRUE(state.IsControlUncertain(object));
  EXPECT_FALSE(state.IsOperationPending(object));
  EXPECT_EQ(IEC61850::ValidateMmsControlOperation(
                model, object, IEC61850::MmsControlOperation::OPERATE, state,
                1000)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(IEC61850::ValidateMmsControlOperation(
                model, object, IEC61850::MmsControlOperation::CANCEL, state,
                1000)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证不确定状态容量耗尽时进入全局保守锁定，不能因记录失败而继续控制。
TEST(IEC61850MmsControlTest, UncertainCapacityOverflowLocksControlState) {
  IEC61850::MmsSboState state;
  auto object = DiscoveredControlObject();
  for (std::size_t index = 0; index < 256; ++index) {
    auto current = object;
    current.identifier += std::to_string(index);
    ASSERT_TRUE(state.RecordPendingOperation(current).ok());
    ASSERT_TRUE(state.MarkUncertain(current).ok());
  }

  auto overflow = object;
  overflow.identifier += "overflow";
  EXPECT_EQ(state.MarkUncertain(overflow).error_code(),
            grpc::StatusCode::RESOURCE_EXHAUSTED);
  EXPECT_TRUE(state.IsControlUncertain(overflow));
  EXPECT_EQ(state.RecordPendingOperation(overflow).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);

  state.Clear();
  EXPECT_FALSE(state.IsControlUncertain(overflow));
  EXPECT_TRUE(state.RecordPendingOperation(overflow).ok());
}

// 验证不同控制对象使用各自的SBO保持时间，避免全局窗口互相覆盖。
TEST(IEC61850MmsControlTest, KeepsIndependentSboSelectionTimeouts) {
  IEC61850::MmsSboState state(std::chrono::milliseconds(5000));
  auto first = DiscoveredControlObject();
  auto second = first;
  second.identifier += "$Other";
  ASSERT_TRUE(state.RecordSelection(first, 1000,
                                    std::chrono::milliseconds(100))
                  .ok());
  ASSERT_TRUE(state.RecordSelection(second, 1000,
                                    std::chrono::milliseconds(200))
                  .ok());
  EXPECT_FALSE(state.IsSelected(first, 1100));
  EXPECT_TRUE(state.IsSelected(second, 1199));
  EXPECT_FALSE(state.IsSelected(second, 1200));
}

// 验证需要SBO的Oper必须有同一对象的有效选择上下文。
TEST(IEC61850MmsControlTest, RequiresSboSelectionBeforeOperate) {
  IEC61850::MmsControlModel model;
  ASSERT_TRUE(IEC61850::BuildMmsControlModel(ControlPlan(), ControlDirectory(),
                                             &model)
                  .ok());
  IEC61850::MmsSboState state(std::chrono::milliseconds(100));
  const auto object = DiscoveredControlObject();
  EXPECT_EQ(IEC61850::ValidateMmsControlOperation(
                model, object, IEC61850::MmsControlOperation::OPERATE, state,
                1000)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  ASSERT_TRUE(state.RecordSelection(object, 1000).ok());
  EXPECT_TRUE(IEC61850::ValidateMmsControlOperation(
                  model, object, IEC61850::MmsControlOperation::OPERATE, state,
                  1050)
                  .ok());
  EXPECT_EQ(IEC61850::ValidateMmsControlOperation(
                model, object, IEC61850::MmsControlOperation::OPERATE, state,
                1100)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证普通SBO选择使用控制对象的$SBO变量读取而不是通用Write。
TEST(IEC61850MmsControlTest, BuildsSboSelectReadRequest) {
  IEC61850::MmsReadRequest request;
  ASSERT_TRUE(IEC61850::BuildMmsControlSelectRequest(ControlObject(), &request)
                  .ok());
  ASSERT_EQ(request.variables.size(), 1u);
  EXPECT_EQ(request.variables.front().type,
            IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC);
  EXPECT_EQ(request.variables.front().domain, "LD0");
  EXPECT_EQ(request.variables.front().identifier, "CSWI1$Pos$SBO");
  EXPECT_FALSE(request.specificationWithResult);
}

// 验证普通SBO只接受单个非空VisibleString结果，其他Data不能建立选择保持。
TEST(IEC61850MmsControlTest, ValidatesSboSelectResponse) {
  IEC61850::MmsReadResponse response;
  auto& item = response.items.emplace_back();
  item.success = true;
  ASSERT_TRUE(IEC61850::EncodeMmsDataVisibleString(
                  "IED1LD0/CSWI1.Pos", &item.encodedData)
                  .ok());
  const auto expectedObject = DiscoveredControlObject();
  EXPECT_TRUE(IEC61850::ValidateMmsControlSelectResponse(
                  response, &expectedObject)
                  .ok());

  ASSERT_TRUE(IEC61850::EncodeMmsDataVisibleString(
                  "IED1LD0/CSWI2.Pos", &item.encodedData)
                  .ok());
  EXPECT_EQ(IEC61850::ValidateMmsControlSelectResponse(
                response, &expectedObject)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);

  ASSERT_TRUE(IEC61850::EncodeMmsDataVisibleString("", &item.encodedData)
                  .ok());
  EXPECT_EQ(IEC61850::ValidateMmsControlSelectResponse(response).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  ASSERT_TRUE(IEC61850::EncodeMmsDataBoolean(true, &item.encodedData).ok());
  EXPECT_EQ(IEC61850::ValidateMmsControlSelectResponse(response).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  item.success = false;
  item.encodedData.clear();
  EXPECT_EQ(IEC61850::ValidateMmsControlSelectResponse(response).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  item.success = true;
  item.encodedData = {0x8a, 0x02, 'A'};
  EXPECT_EQ(IEC61850::ValidateMmsControlSelectResponse(response).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  ASSERT_TRUE(IEC61850::EncodeMmsDataBoolean(true, &item.encodedData).ok());
  response.items.emplace_back(item);
  EXPECT_EQ(IEC61850::ValidateMmsControlSelectResponse(response).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证SBOw、Oper和Cancel分别使用标准控制对象后缀及各自的结构成员。
TEST(IEC61850MmsControlTest, BuildsControlWriteRequests) {
  for (const auto operation : {
           IEC61850::MmsControlOperation::SELECT_WITH_VALUE,
           IEC61850::MmsControlOperation::OPERATE,
           IEC61850::MmsControlOperation::CANCEL}) {
    auto command = OperateCommand(operation);
    if (operation == IEC61850::MmsControlOperation::CANCEL) {
      command.controlValue.clear();
    }
    IEC61850::MmsWriteRequest request;
    ASSERT_TRUE(IEC61850::BuildMmsControlWriteRequest(command, &request)
                    .ok());
    ASSERT_EQ(request.items.size(), 1u);
    const auto& item = request.items.front();
    const auto expectedSuffix =
        operation == IEC61850::MmsControlOperation::SELECT_WITH_VALUE
            ? "$SBOw"
            : operation == IEC61850::MmsControlOperation::OPERATE ? "$Oper"
                                                                   : "$Cancel";
    EXPECT_EQ(item.variable.identifier,
              std::string("CSWI1$Pos") + expectedSuffix);
    const auto tags = DecodeControlStructureTags(item.encodedData);
    if (operation == IEC61850::MmsControlOperation::CANCEL) {
      EXPECT_EQ(tags, (std::vector<std::uint8_t>{0xa2, 0x86, 0x91, 0x83,
                                                 0x84}));
    } else {
      EXPECT_EQ(tags, (std::vector<std::uint8_t>{0x83, 0xa2, 0x86, 0x91,
                                                 0x83, 0x84}));
    }
  }
}

// 验证DataCenter标量控制值按在线ctlVal类型编码，并正确执行整数和浮点工程量反向换算。
TEST(IEC61850MmsControlTest, EncodesDataCenterScalarControlValue) {
  IEC61850::MmsControlCapability capability;
  capability.ctlValType = IEC61850::MmsTypeSpecification{
      .kind = IEC61850::MmsTypeSpecificationKind::INTEGER, .width = 32};
  IEC61850::MmsPointControlCommand command;
  command.valueType = IEC61850Proto::POINT_VALUE_TYPE_INT64;
  command.intValue = 17;
  command.scale = 2.0;
  command.offset = 3.0;

  std::vector<std::uint8_t> encoded;
  ASSERT_TRUE(IEC61850::EncodeMmsPointControlValue(
                  command, capability, &encoded)
                  .ok());
  std::size_t offset = 0;
  IEC61850::BerTlvView value;
  ASSERT_TRUE(IEC61850::ReadBerTlv(encoded, &offset, &value).ok());
  ASSERT_EQ(offset, encoded.size());
  std::int64_t decodedInteger = 0;
  ASSERT_TRUE(IEC61850::ReadBerSigned(value.value, &decodedInteger).ok());
  EXPECT_EQ(decodedInteger, 7);

  capability.ctlValType = IEC61850::MmsTypeSpecification{
      .kind = IEC61850::MmsTypeSpecificationKind::UNSIGNED, .width = 32};
  command.intValue = 5;
  command.scale = 0.0;
  command.offset = 0.0;
  ASSERT_TRUE(IEC61850::EncodeMmsPointControlValue(
                  command, capability, &encoded)
                  .ok());
  offset = 0;
  ASSERT_TRUE(IEC61850::ReadBerTlv(encoded, &offset, &value).ok());
  std::uint64_t decodedUnsigned = 0;
  ASSERT_TRUE(IEC61850::ReadBerUnsigned(value.value, &decodedUnsigned).ok());
  EXPECT_EQ(decodedUnsigned, 5u);

  capability.ctlValType = IEC61850::MmsTypeSpecification{
      .kind = IEC61850::MmsTypeSpecificationKind::FLOATING_POINT, .width = 32};
  command.valueType = IEC61850Proto::POINT_VALUE_TYPE_DOUBLE;
  command.doubleValue = 6.0;
  command.scale = 2.0;
  command.offset = 1.0;
  ASSERT_TRUE(IEC61850::EncodeMmsPointControlValue(
                  command, capability, &encoded)
                  .ok());
  EXPECT_EQ(encoded.front(), 0x87);
  EXPECT_EQ(encoded[2], 0x08);
}

// 验证DataCenter整数控制值反向换算后超出MMS有符号或无符号范围时被拒绝。
TEST(IEC61850MmsControlTest, RejectsDataCenterIntegerControlOverflow) {
  IEC61850::MmsControlCapability capability;
  capability.ctlValType = IEC61850::MmsTypeSpecification{
      .kind = IEC61850::MmsTypeSpecificationKind::INTEGER, .width = 64};
  IEC61850::MmsPointControlCommand command;
  command.valueType = IEC61850Proto::POINT_VALUE_TYPE_INT64;
  command.intValue = std::numeric_limits<std::int64_t>::max();
  command.scale = 0.5;
  std::vector<std::uint8_t> encoded;
  EXPECT_EQ(IEC61850::EncodeMmsPointControlValue(
                command, capability, &encoded)
                .error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  capability.ctlValType->kind = IEC61850::MmsTypeSpecificationKind::UNSIGNED;
  command.offset = 0.0;
  command.scale = 0.0;
  command.intValue = -1;
  EXPECT_EQ(IEC61850::EncodeMmsPointControlValue(
                command, capability, &encoded)
                .error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证Oper结构的可选operTm位于ctlVal之后且不改变其他成员顺序。
TEST(IEC61850MmsControlTest, EncodesOptionalOperateTimestamp) {
  auto command = OperateCommand(IEC61850::MmsControlOperation::OPERATE);
  command.operateTimestampMs = 2000;
  std::vector<std::uint8_t> encoded;
  ASSERT_TRUE(IEC61850::EncodeMmsControlStructure(command, &encoded).ok());

  std::size_t offset = 0;
  IEC61850::BerTlvView structure;
  ASSERT_TRUE(IEC61850::ReadBerTlv(encoded, &offset, &structure).ok());
  ASSERT_EQ(offset, encoded.size());
  std::size_t fieldOffset = 0;
  IEC61850::BerTlvView field;
  ASSERT_TRUE(IEC61850::ReadBerTlv(structure.value, &fieldOffset, &field).ok());
  ASSERT_TRUE(IEC61850::ReadBerTlv(structure.value, &fieldOffset, &field).ok());
  EXPECT_EQ(field.tag, 0x91);
}

// 验证ctlNum按无符号8位值编码，完整覆盖0到255范围。
TEST(IEC61850MmsControlTest, EncodesUnsignedControlNumber) {
  auto command = OperateCommand(IEC61850::MmsControlOperation::OPERATE);
  command.controlNumber = 255;
  std::vector<std::uint8_t> encoded;
  ASSERT_TRUE(IEC61850::EncodeMmsControlStructure(command, &encoded).ok());

  std::size_t offset = 0;
  IEC61850::BerTlvView structure;
  ASSERT_TRUE(IEC61850::ReadBerTlv(encoded, &offset, &structure).ok());
  offset = 0;
  IEC61850::BerTlvView field;
  ASSERT_TRUE(IEC61850::ReadBerTlv(structure.value, &offset, &field).ok());
  ASSERT_TRUE(IEC61850::ReadBerTlv(structure.value, &offset, &field).ok());
  ASSERT_TRUE(IEC61850::ReadBerTlv(structure.value, &offset, &field).ok());
  ASSERT_EQ(field.tag, 0x86);
  std::uint64_t value = 0;
  ASSERT_TRUE(IEC61850::ReadBerUnsigned(field.value, &value).ok());
  EXPECT_EQ(value, 255u);
}

// 验证控制编解码拒绝非法对象、错误操作成员、越界origin和非法Check。
TEST(IEC61850MmsControlTest, RejectsInvalidControlCommands) {
  auto command = OperateCommand(IEC61850::MmsControlOperation::OPERATE);
  std::vector<std::uint8_t> encoded{0xff};
  EXPECT_FALSE(IEC61850::EncodeMmsControlStructure(command, nullptr).ok());

  command.controlValue.clear();
  EXPECT_FALSE(IEC61850::EncodeMmsControlStructure(command, &encoded).ok());
  EXPECT_TRUE(encoded.empty());

  command = OperateCommand(IEC61850::MmsControlOperation::OPERATE);
  command.controlValue.push_back(0x00);
  EXPECT_FALSE(IEC61850::EncodeMmsControlStructure(command, &encoded).ok());

  command = OperateCommand(IEC61850::MmsControlOperation::OPERATE);
  command.originCategory = 9;
  EXPECT_FALSE(IEC61850::EncodeMmsControlStructure(command, &encoded).ok());

  command = OperateCommand(IEC61850::MmsControlOperation::OPERATE);
  command.originIdentifier.assign(65, 0x01);
  EXPECT_FALSE(IEC61850::EncodeMmsControlStructure(command, &encoded).ok());

  command = OperateCommand(IEC61850::MmsControlOperation::OPERATE);
  command.check = 4;
  EXPECT_FALSE(IEC61850::EncodeMmsControlStructure(command, &encoded).ok());

  command = OperateCommand(IEC61850::MmsControlOperation::CANCEL);
  EXPECT_FALSE(IEC61850::EncodeMmsControlStructure(command, &encoded).ok());
  command.controlValue.clear();
  EXPECT_TRUE(IEC61850::EncodeMmsControlStructure(command, &encoded).ok());

  command = OperateCommand(IEC61850::MmsControlOperation::SELECT_WITH_VALUE);
  command.operateTimestampMs = 2000;
  EXPECT_FALSE(IEC61850::EncodeMmsControlStructure(command, &encoded).ok());

  command = OperateCommand(IEC61850::MmsControlOperation::SELECT);
  IEC61850::MmsWriteRequest request;
  EXPECT_FALSE(
      IEC61850::BuildMmsControlWriteRequest(command, &request).ok());
}

// 验证控制对象必须是有效的Domain-specific对象，且输出失败时保持清空。
TEST(IEC61850MmsControlTest, RejectsInvalidControlObject) {
  auto object = ControlObject();
  object.type = IEC61850::MmsObjectNameType::VMD_SPECIFIC;
  IEC61850::MmsReadRequest readRequest;
  EXPECT_FALSE(
      IEC61850::BuildMmsControlSelectRequest(object, &readRequest).ok());
  EXPECT_TRUE(readRequest.variables.empty());

  object = ControlObject();
  object.identifier.clear();
  EXPECT_FALSE(
      IEC61850::BuildMmsControlSelectRequest(object, &readRequest).ok());
  EXPECT_TRUE(readRequest.variables.empty());
}

// 验证CommandTermination成功报告只包含目标Oper并正确核对对象引用。
TEST(IEC61850MmsControlTest, DecodesSuccessfulCommandTermination) {
  const auto expectedOper =
      IEC61850::MmsObjectName{IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC,
                              "LD0", "CSWI1$Pos$Oper"};
  const auto encoded =
      CommandTerminationReport("LD0/CSWI1$Pos$Oper", 7);
  IEC61850::MmsCommandTermination termination;
  ASSERT_TRUE(IEC61850::DecodeMmsCommandTermination(
                  encoded, expectedOper, 7, &termination)
                  .ok());
  EXPECT_TRUE(termination.success);
  EXPECT_FALSE(termination.lastApplError.has_value());
  EXPECT_EQ(termination.operationObject, expectedOper);
}

// 验证CommandTermination失败报告能解析LastApplError、Origin、ctlNum和AddCause。
TEST(IEC61850MmsControlTest, DecodesCommandTerminationWithLastApplError) {
  const auto expectedOper =
      IEC61850::MmsObjectName{IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC,
                              "LD0", "CSWI1$Pos$Oper"};
  const auto encoded = CommandTerminationReport(
      "LD0/CSWI1$Pos$Oper", 7, true, "LD0/CSWI1$Pos$Oper");
  IEC61850::MmsCommandTermination termination;
  ASSERT_TRUE(IEC61850::DecodeMmsCommandTermination(
                  encoded, expectedOper, 7, &termination)
                  .ok());
  EXPECT_FALSE(termination.success);
  ASSERT_TRUE(termination.lastApplError.has_value());
  const auto& error = *termination.lastApplError;
  EXPECT_EQ(error.controlObject, expectedOper);
  EXPECT_EQ(error.error, 1);
  EXPECT_EQ(error.originCategory, 2);
  EXPECT_EQ(error.originIdentifier,
            (std::vector<std::uint8_t>{0x01, 0x02, 0x03, 0x04}));
  EXPECT_EQ(error.controlNumber, 7);
  EXPECT_EQ(error.addCause, 10);
}

// 验证CommandTermination拒绝错误对象、ctlNum不匹配和截断报文，且不交付半成品。
TEST(IEC61850MmsControlTest, RejectsInvalidCommandTermination) {
  const auto expectedOper =
      IEC61850::MmsObjectName{IEC61850::MmsObjectNameType::DOMAIN_SPECIFIC,
                              "LD0", "CSWI1$Pos$Oper"};
  IEC61850::MmsCommandTermination termination;
  auto encoded = CommandTerminationReport(
      "LD0/CSWI1$Pos$Oper", 7, true, "LD0/CSWI1$Other$Oper");
  EXPECT_FALSE(IEC61850::DecodeMmsCommandTermination(
                   encoded, expectedOper, 7, &termination)
                   .ok());
  EXPECT_TRUE(termination.operationObject.identifier.empty());

  encoded = CommandTerminationReport(
      "LD0/CSWI1$Pos$Oper", 7, true, "LD0/CSWI1$Pos$Oper");
  EXPECT_FALSE(IEC61850::DecodeMmsCommandTermination(
                   encoded, expectedOper, 8, &termination)
                   .ok());
  ASSERT_FALSE(encoded.empty());
  encoded.pop_back();
  EXPECT_FALSE(IEC61850::DecodeMmsCommandTermination(
                   encoded, expectedOper, 7, &termination)
                   .ok());
  EXPECT_FALSE(termination.success);
  EXPECT_FALSE(termination.lastApplError.has_value());
}

}  // namespace
