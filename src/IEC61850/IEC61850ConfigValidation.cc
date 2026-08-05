#include "IEC61850ConfigValidation.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "mskdsp/IEC61850Limits.hpp"
#include "IEC61850ModelSelection.h"
#include "IEC61850SvRuntime.h"
#include "IEC61850ThreadRuntimePolicy.h"

namespace IEC61850 {
namespace {

void AddError(std::vector<IEC61850Proto::ValidationIssue>* issues,
              std::string code, std::string path, std::string message) {
  if (issues == nullptr) {
    return;
  }
  auto& issue = issues->emplace_back();
  issue.set_severity(IEC61850Proto::VALIDATION_SEVERITY_ERROR);
  issue.set_code(std::move(code));
  issue.set_path(std::move(path));
  issue.set_message(std::move(message));
}

bool HasErrors(const std::vector<IEC61850Proto::ValidationIssue>* issues,
               bool localError) {
  return localError || (issues != nullptr && !issues->empty());
}

grpc::Status Invalid(const std::vector<IEC61850Proto::ValidationIssue>* issues,
                     std::string fallback) {
  if (issues != nullptr && !issues->empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        issues->front().message());
  }
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                      std::move(fallback));
}

std::string ScopedReference(std::string_view accessPoint,
                            std::string_view reference) {
  std::string key;
  key.reserve(accessPoint.size() + reference.size() + 1);
  key.append(accessPoint);
  key.push_back('\0');
  key.append(reference);
  return key;
}

std::string Upper(std::string_view value) {
  std::string result(value);
  std::ranges::transform(result, result.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return result;
}

bool IsProtectionScalarType(IEC61850Proto::PointValueType type) {
  return type == IEC61850Proto::POINT_VALUE_TYPE_BOOL ||
         type == IEC61850Proto::POINT_VALUE_TYPE_INT64 ||
         type == IEC61850Proto::POINT_VALUE_TYPE_DOUBLE;
}

bool IsProtectionComparator(IEC61850Proto::ProtectionComparator comparator) {
  switch (comparator) {
    case IEC61850Proto::PROTECTION_COMPARATOR_BOOL_TRUE:
    case IEC61850Proto::PROTECTION_COMPARATOR_BOOL_FALSE:
    case IEC61850Proto::PROTECTION_COMPARATOR_EQUAL:
    case IEC61850Proto::PROTECTION_COMPARATOR_NOT_EQUAL:
    case IEC61850Proto::PROTECTION_COMPARATOR_GREATER_THAN:
    case IEC61850Proto::PROTECTION_COMPARATOR_GREATER_OR_EQUAL:
    case IEC61850Proto::PROTECTION_COMPARATOR_LESS_THAN:
    case IEC61850Proto::PROTECTION_COMPARATOR_LESS_OR_EQUAL:
      return true;
    default:
      return false;
  }
}

bool IsProtectionNumericComparator(
    IEC61850Proto::ProtectionComparator comparator) {
  return comparator == IEC61850Proto::PROTECTION_COMPARATOR_GREATER_THAN ||
         comparator == IEC61850Proto::PROTECTION_COMPARATOR_GREATER_OR_EQUAL ||
         comparator == IEC61850Proto::PROTECTION_COMPARATOR_LESS_THAN ||
         comparator == IEC61850Proto::PROTECTION_COMPARATOR_LESS_OR_EQUAL;
}

}  // namespace

grpc::Status ValidatePersistedConfig(
    const IEC61850Proto::PersistedConfig& config,
    std::vector<IEC61850Proto::ValidationIssue>* issues) {
  if (issues != nullptr) {
    issues->clear();
  }
  bool hasError = false;
  if (config.schema_version() != 1) {
    AddError(issues, "CONFIG_SCHEMA_VERSION_UNSUPPORTED", "/schema_version",
             std::format("不支持的IEC61850配置版本: {}", config.schema_version()));
    hasError = true;
  }

  std::unordered_map<std::string, const IEC61850Proto::NormalizedSclModel*> models;
  for (int index = 0; index < config.models_size(); ++index) {
    const auto& model = config.models(index);
    const auto path = std::format("/models/{}", index);
    if (model.model_name().empty()) {
      AddError(issues, "CONFIG_MODEL_NAME_EMPTY", path + "/model_name",
               "模型名称不能为空");
      hasError = true;
      continue;
    }
    if (!models.emplace(model.model_name(), &model).second) {
      AddError(issues, "CONFIG_DUPLICATE_MODEL_NAME", path + "/model_name",
               std::format("模型名称重复: {}", model.model_name()));
      hasError = true;
    }
    for (int iedIndex = 0; iedIndex < model.ieds_size(); ++iedIndex) {
      const auto& modelIed = model.ieds(iedIndex);
      const auto iedPath = std::format("{}/ieds/{}", path, iedIndex);
      std::unordered_map<std::string, bool> accessPoints;
      for (int accessPointIndex = 0;
           accessPointIndex < modelIed.access_points_size();
           ++accessPointIndex) {
        const auto& accessPoint =
            modelIed.access_points(accessPointIndex);
        if (accessPoint.name().empty()) {
          AddError(
              issues, "CONFIG_ACCESS_POINT_NAME_EMPTY",
              std::format("{}/access_points/{}/name", iedPath,
                          accessPointIndex),
              std::format("模型IED {}中的AccessPoint名称不能为空",
                          modelIed.name()));
          hasError = true;
        }
        if (!accessPoints.emplace(accessPoint.name(),
                                  accessPoint.has_server()).second) {
          AddError(
              issues, "CONFIG_DUPLICATE_ACCESS_POINT",
              std::format("{}/access_points/{}/name", iedPath,
                          accessPointIndex),
              std::format("模型IED {}中的AccessPoint名称重复: {}",
                          modelIed.name(), accessPoint.name()));
          hasError = true;
        }
      }
      const auto serverAccessPointCount =
          CountServerAccessPoints(modelIed);
      const auto validateOwnership =
          [&](const auto& objects, std::string_view fieldName) {
            for (int objectIndex = 0; objectIndex < objects.size();
                 ++objectIndex) {
              const auto& object = objects.Get(objectIndex);
              const auto objectPath =
                  std::format("{}/{}/{}/access_point", iedPath, fieldName,
                              objectIndex);
              if (object.access_point().empty()) {
                if (serverAccessPointCount != 1) {
                  AddError(
                      issues, "CONFIG_ACCESS_POINT_MODEL_UNSCOPED",
                      objectPath,
                      std::format(
                          "模型IED {}的对象缺少AccessPoint归属且无法唯一推断，请重新导入SCL",
                          modelIed.name()));
                  hasError = true;
                }
                continue;
              }
              const auto accessPointIt =
                  accessPoints.find(object.access_point());
              if (accessPointIt == accessPoints.end() ||
                  !accessPointIt->second) {
                AddError(
                    issues, "CONFIG_ACCESS_POINT_MODEL_INVALID", objectPath,
                    std::format(
                        "模型IED {}的对象归属AccessPoint不存在或未包含Server: {}",
                        modelIed.name(), object.access_point()));
                hasError = true;
              }
            }
          };
      validateOwnership(modelIed.logical_nodes(), "logical_nodes");
      validateOwnership(modelIed.data_objects(), "data_objects");
      validateOwnership(modelIed.data_attributes(), "data_attributes");
      validateOwnership(modelIed.data_sets(), "data_sets");
      validateOwnership(modelIed.report_controls(), "report_controls");
      validateOwnership(modelIed.gse_controls(), "gse_controls");
      validateOwnership(modelIed.sampled_value_controls(),
                        "sampled_value_controls");
      validateOwnership(modelIed.ext_refs(), "ext_refs");

      std::string soleServerAccessPoint;
      if (serverAccessPointCount == 1) {
        for (const auto& accessPoint : modelIed.access_points()) {
          if (accessPoint.has_server()) {
            soleServerAccessPoint = accessPoint.name();
            break;
          }
        }
      }
      const auto effectiveAccessPoint =
          [&soleServerAccessPoint](std::string_view objectAccessPoint) {
            return objectAccessPoint.empty()
                       ? std::string_view(soleServerAccessPoint)
                       : objectAccessPoint;
          };
      std::unordered_set<std::string> dataSetReferences;
      for (const auto& dataSet : modelIed.data_sets()) {
        dataSetReferences.emplace(
            ScopedReference(effectiveAccessPoint(dataSet.access_point()),
                            dataSet.data_set_ref()));
      }
      const auto validateControlDataSets =
          [&](const auto& controls, std::string_view fieldName,
              std::string_view controlType) {
            for (int controlIndex = 0; controlIndex < controls.size();
                 ++controlIndex) {
              const auto& control = controls.Get(controlIndex);
              if (dataSetReferences.contains(ScopedReference(
                      effectiveAccessPoint(control.access_point()),
                      control.data_set_ref()))) {
                continue;
              }
              AddError(
                  issues, "CONFIG_CONTROL_DATASET_NOT_FOUND",
                  std::format("{}/{}/{}/data_set_ref", iedPath, fieldName,
                              controlIndex),
                  std::format(
                      "模型IED {}的{}引用的数据集不在同一AccessPoint中: {}",
                      modelIed.name(), controlType,
                      control.data_set_ref()));
              hasError = true;
            }
          };
      validateControlDataSets(modelIed.report_controls(), "report_controls",
                              "ReportControl");
      for (int reportIndex = 0;
           reportIndex < modelIed.report_controls_size(); ++reportIndex) {
        const auto& report = modelIed.report_controls(reportIndex);
        if (report.trigger_options().general_interrogation() &&
            !report.optional_fields().reason_code()) {
          AddError(
              issues, "CONFIG_REPORT_GI_REQUIRES_REASON_CODE",
              std::format("{}/report_controls/{}/optional_fields/reason_code",
                          iedPath, reportIndex),
              std::format("ReportControl {}启用GI时必须启用ReasonCode",
                          report.rcb_ref()));
          hasError = true;
        }
      }
      validateControlDataSets(modelIed.gse_controls(), "gse_controls",
                              "GSEControl");
      validateControlDataSets(modelIed.sampled_value_controls(),
                              "sampled_value_controls",
                              "SampledValueControl");
    }
  }

  std::unordered_map<std::string, const IEC61850Proto::PersistedIed*> ieds;
  for (int index = 0; index < config.ieds_size(); ++index) {
    const auto& persisted = config.ieds(index);
    const auto& ied = persisted.config();
    const auto path = std::format("/ieds/{}", index);
    if (ied.conn_name().empty()) {
      AddError(issues, "CONFIG_CONN_NAME_EMPTY", path + "/config/conn_name",
               "IED连接名不能为空");
      hasError = true;
      continue;
    }
    if (!ieds.emplace(ied.conn_name(), &persisted).second) {
      AddError(issues, "CONFIG_DUPLICATE_CONN_NAME", path + "/config/conn_name",
               std::format("IED连接名重复: {}", ied.conn_name()));
      hasError = true;
    }
    if (ied.mms_event_queue_capacity() >
        mskdsp::kIec61850MaxMmsEventQueueCapacity) {
      AddError(issues, "CONFIG_MMS_EVENT_QUEUE_CAPACITY_EXCEEDED",
               path + "/config/mms_event_queue_capacity",
               std::format("MMS待处理点值容量不得超过{}",
                           mskdsp::kIec61850MaxMmsEventQueueCapacity));
      hasError = true;
    }
    if (ied.publish_batch_size() >
        mskdsp::kIec61850MaxPublishBatchSize) {
      AddError(issues, "CONFIG_PUBLISH_BATCH_SIZE_EXCEEDED",
               path + "/config/publish_batch_size",
               std::format("DataCenter单批发布点数不得超过{}",
                           mskdsp::kIec61850MaxPublishBatchSize));
      hasError = true;
    }
    if (ied.publish_batch_window_ms() >
        mskdsp::kIec61850MaxPublishBatchWindowMs) {
      AddError(issues, "CONFIG_PUBLISH_BATCH_WINDOW_EXCEEDED",
               path + "/config/publish_batch_window_ms",
               std::format("DataCenter合批窗口不得超过{}毫秒",
                           mskdsp::kIec61850MaxPublishBatchWindowMs));
      hasError = true;
    }
    if (!IsSupportedSvNominalFrequencyHz(ied.nominal_frequency_hz())) {
      AddError(issues, "CONFIG_SV_NOMINAL_FREQUENCY_INVALID",
               path + "/config/nominal_frequency_hz",
               "SV额定频率只能为0、50Hz或60Hz，且必须为有限数值");
      hasError = true;
    }
    ThreadRuntimePolicy runtimePolicy;
    const auto runtimePolicyStatus =
        BuildThreadRuntimePolicy(ied, &runtimePolicy);
    if (!runtimePolicyStatus.ok()) {
      AddError(issues, "CONFIG_REALTIME_POLICY_INVALID",
               path + "/config/realtime_scheduling",
               runtimePolicyStatus.error_message());
      hasError = true;
    }
    std::unordered_set<std::string> protectionRuleIds;
    for (int ruleIndex = 0; ruleIndex < ied.protection_rules_size();
         ++ruleIndex) {
      const auto& rule = ied.protection_rules(ruleIndex);
      const auto rulePath = std::format("{}/config/protection_rules/{}", path,
                                        ruleIndex);
      if (rule.rule_id().empty() ||
          !protectionRuleIds.emplace(rule.rule_id()).second) {
        AddError(issues, "CONFIG_PROTECTION_RULE_ID_INVALID",
                 rulePath + "/rule_id", "保护规则编号不能为空且不能重复");
        hasError = true;
      }
      if (!ied.enable_goose()) {
        AddError(issues, "CONFIG_PROTECTION_REQUIRES_GOOSE",
                 rulePath, "配置保护规则时必须启用GOOSE");
        hasError = true;
      }
      if (rule.conditions_size() == 0 || rule.output_control_ref().empty() ||
          rule.assert_values_size() == 0 ||
          rule.assert_values_size() != rule.release_values_size()) {
        AddError(issues, "CONFIG_PROTECTION_RULE_INCOMPLETE", rulePath,
                 "保护规则必须包含输入条件、GOOSE订阅以及成对的动作和释放值");
        hasError = true;
      }
      std::unordered_set<std::string> conditionReferences;
      for (int conditionIndex = 0;
           conditionIndex < rule.conditions_size(); ++conditionIndex) {
        const auto& condition = rule.conditions(conditionIndex);
        const auto conditionPath = std::format("{}/conditions/{}", rulePath,
                                               conditionIndex);
        if (condition.data_ref().empty() ||
            condition.fc() ==
                IEC61850Proto::FUNCTIONAL_CONSTRAINT_UNSPECIFIED ||
            !conditionReferences
                 .emplace(std::format("{}#{}", condition.data_ref(),
                                      static_cast<int>(condition.fc())))
                 .second ||
            !IsProtectionScalarType(condition.value_type()) ||
            !IsProtectionComparator(condition.comparator())) {
          AddError(issues, "CONFIG_PROTECTION_CONDITION_INVALID",
                   conditionPath, "保护输入条件的信号、类型或比较器无效");
          hasError = true;
          continue;
        }
        if ((condition.comparator() ==
                 IEC61850Proto::PROTECTION_COMPARATOR_BOOL_TRUE ||
             condition.comparator() ==
                 IEC61850Proto::PROTECTION_COMPARATOR_BOOL_FALSE) &&
            condition.value_type() != IEC61850Proto::POINT_VALUE_TYPE_BOOL) {
          AddError(issues, "CONFIG_PROTECTION_BOOL_TYPE_MISMATCH",
                   conditionPath + "/value_type",
                   "保护布尔比较器必须使用BOOL类型");
          hasError = true;
        }
        if (IsProtectionNumericComparator(condition.comparator()) &&
            condition.value_type() == IEC61850Proto::POINT_VALUE_TYPE_BOOL) {
          AddError(issues, "CONFIG_PROTECTION_NUMERIC_TYPE_MISMATCH",
                   conditionPath + "/value_type",
                   "保护大小比较器不能使用BOOL类型");
          hasError = true;
        }
      }
      if (rule.interlock_signal_ids_size() != 0) {
        AddError(issues, "CONFIG_PROTECTION_INTERLOCK_REFERENCE_INVALID",
                 rulePath + "/interlock_signal_ids",
                 "生产保护规则必须使用interlock_signals稳定引用");
        hasError = true;
      }
      std::unordered_set<std::string> interlockReferences;
      for (int interlockIndex = 0;
           interlockIndex < rule.interlock_signals_size(); ++interlockIndex) {
        const auto& reference = rule.interlock_signals(interlockIndex);
        const auto referenceKey = std::format(
            "{}#{}", reference.data_ref(), static_cast<int>(reference.fc()));
        if (reference.data_ref().empty() ||
            reference.fc() ==
                IEC61850Proto::FUNCTIONAL_CONSTRAINT_UNSPECIFIED ||
            !interlockReferences.emplace(referenceKey).second ||
            conditionReferences.contains(referenceKey)) {
          AddError(issues, "CONFIG_PROTECTION_INTERLOCK_REFERENCE_INVALID",
                   std::format("{}/interlock_signals/{}", rulePath,
                               interlockIndex),
                   "保护联锁必须使用非空data_ref和fc");
          hasError = true;
        }
      }
      for (int valueIndex = 0; valueIndex < rule.assert_values_size();
           ++valueIndex) {
        const auto& assertValue = rule.assert_values(valueIndex);
        const auto& releaseValue = rule.release_values(valueIndex);
        if (!IsProtectionScalarType(assertValue.value_type()) ||
            !IsProtectionScalarType(releaseValue.value_type()) ||
            assertValue.value_type() != releaseValue.value_type()) {
          AddError(issues, "CONFIG_PROTECTION_OUTPUT_INVALID",
                   std::format("{}/assert_values/{}", rulePath, valueIndex),
                   "保护动作值和释放值必须使用相同的基础标量类型");
          hasError = true;
        }
      }
    }

    const auto modelIt = models.find(ied.model_name());
    if (modelIt == models.end()) {
      AddError(issues, "CONFIG_MODEL_NOT_FOUND", path + "/config/model_name",
               std::format("IED引用的模型不存在: {}", ied.model_name()));
      hasError = true;
    } else {
      const IEC61850Proto::SclIed* foundModelIed = nullptr;
      for (const auto& modelIed : modelIt->second->ieds()) {
        if (modelIed.name() == ied.ied_name()) {
          foundModelIed = &modelIed;
          break;
        }
      }
      if (foundModelIed == nullptr) {
        AddError(issues, "CONFIG_IED_NOT_FOUND_IN_MODEL",
                 path + "/config/ied_name",
                 std::format("模型{}中不存在IED: {}", ied.model_name(),
                             ied.ied_name()));
        hasError = true;
      } else {
        const IEC61850Proto::SclAccessPoint* foundAccessPoint = nullptr;
        for (const auto& accessPoint : foundModelIed->access_points()) {
          if (accessPoint.name() == ied.access_point()) {
            foundAccessPoint = &accessPoint;
            break;
          }
        }
        if (foundAccessPoint == nullptr) {
          AddError(issues, "CONFIG_ACCESS_POINT_NOT_FOUND",
                   path + "/config/access_point",
                   std::format("模型IED {}中不存在AccessPoint: {}",
                               ied.ied_name(), ied.access_point()));
          hasError = true;
        } else if ((ied.enable_mms() || ied.enable_goose() ||
                    ied.enable_sv()) &&
                   !foundAccessPoint->has_server()) {
          AddError(
              issues, "CONFIG_ACCESS_POINT_SERVER_MISSING",
              path + "/config/access_point",
              std::format("模型IED {}的AccessPoint未包含Server: {}",
                          ied.ied_name(), ied.access_point()));
          hasError = true;
        }
      }
    }

    std::unordered_set<int> channelIds;
    bool hasEnabledChannel = false;
    for (int channelIndex = 0; channelIndex < ied.channels_size(); ++channelIndex) {
      const auto& channel = ied.channels(channelIndex);
      const auto channelPath = std::format("{}/config/channels/{}", path,
                                           channelIndex);
      if (channel.channel() == IEC61850Proto::NETWORK_CHANNEL_UNSPECIFIED) {
        AddError(issues, "CONFIG_CHANNEL_UNSPECIFIED", channelPath + "/channel",
                 "网络通道必须指定为A网或B网");
        hasError = true;
      }
      if (!channelIds.emplace(static_cast<int>(channel.channel())).second) {
        AddError(issues, "CONFIG_DUPLICATE_CHANNEL", channelPath + "/channel",
                 "同一IED的A/B网络通道不能重复");
        hasError = true;
      }
      if (!channel.enabled()) {
        continue;
      }
      hasEnabledChannel = true;
      if (channel.interface_name().empty()) {
        AddError(issues, "CONFIG_INTERFACE_NAME_EMPTY",
                 channelPath + "/interface_name", "启用的网络通道必须配置网卡名");
        hasError = true;
      }
      if (ied.enable_mms() &&
          (channel.remote_ip().empty() || channel.remote_port() == 0 ||
           channel.remote_port() > 65535)) {
        AddError(issues, "CONFIG_MMS_ENDPOINT_INCOMPLETE", channelPath,
                 "启用MMS的网络通道必须配置有效远端IP和端口");
        hasError = true;
      }
    }
    if ((ied.enable_mms() || ied.enable_goose() || ied.enable_sv()) &&
        !hasEnabledChannel) {
      AddError(issues, "CONFIG_NO_ENABLED_CHANNEL", path + "/config/channels",
               "启用通信功能时至少需要一个已启用网络通道");
      hasError = true;
    }
  }

  std::unordered_set<std::string> mappingTables;
  for (int tableIndex = 0; tableIndex < config.point_mappings_size(); ++tableIndex) {
    const auto& table = config.point_mappings(tableIndex);
    const auto path = std::format("/point_mappings/{}", tableIndex);
    if (!ieds.contains(table.conn_name())) {
      AddError(issues, "CONFIG_MAPPING_IED_NOT_FOUND", path + "/conn_name",
               std::format("点映射引用的IED连接不存在: {}", table.conn_name()));
      hasError = true;
    }
    if (!mappingTables.emplace(table.conn_name()).second) {
      AddError(issues, "CONFIG_DUPLICATE_MAPPING_TABLE", path + "/conn_name",
               std::format("IED存在重复点映射表: {}", table.conn_name()));
      hasError = true;
    }
    std::unordered_set<std::string> tags;
    std::unordered_set<std::string> references;
    for (int pointIndex = 0; pointIndex < table.points_size(); ++pointIndex) {
      const auto& point = table.points(pointIndex);
      const auto pointPath = std::format("{}/points/{}", path, pointIndex);
      if (point.tag().empty()) {
        AddError(issues, "CONFIG_POINT_TAG_EMPTY", pointPath + "/tag",
                 "点映射tag不能为空");
        hasError = true;
      } else if (!tags.emplace(point.tag()).second) {
        AddError(issues, "CONFIG_DUPLICATE_POINT_TAG", pointPath + "/tag",
                 std::format("点映射tag重复: {}", point.tag()));
        hasError = true;
      }
      if (point.data_ref().empty()) {
        AddError(issues, "CONFIG_DATA_REF_EMPTY", pointPath + "/data_ref",
                 "点映射data_ref不能为空");
        hasError = true;
      }
      if (point.source() == IEC61850Proto::POINT_SOURCE_SV_DERIVED &&
          !point.data_ref().starts_with("SV_DERIVED/")) {
        AddError(issues, "CONFIG_SV_DERIVED_NAMESPACE_INVALID",
                 pointPath + "/data_ref",
                 "SV派生点data_ref必须使用SV_DERIVED/命名空间");
        hasError = true;
      }
      if (point.fc() == IEC61850Proto::FUNCTIONAL_CONSTRAINT_UNSPECIFIED) {
        AddError(issues, "CONFIG_POINT_FC_UNSPECIFIED", pointPath + "/fc",
                 "点映射fc不能为空");
        hasError = true;
      }
      const auto referenceKey = std::format("{}#{}", point.data_ref(),
                                            static_cast<int>(point.fc()));
      if (!point.data_ref().empty() &&
          !references.emplace(referenceKey).second) {
        AddError(issues, "CONFIG_DUPLICATE_DATA_REF", pointPath + "/data_ref",
                 std::format("点映射data_ref和fc组合重复: {}#{}",
                             point.data_ref(), static_cast<int>(point.fc())));
        hasError = true;
      }
      if (point.source() == IEC61850Proto::POINT_SOURCE_UNSPECIFIED) {
        AddError(issues, "CONFIG_POINT_SOURCE_UNSPECIFIED", pointPath + "/source",
                 "点映射数据来源不能为空");
        hasError = true;
      }
      if (point.value_type() == IEC61850Proto::POINT_VALUE_TYPE_UNSPECIFIED) {
        AddError(issues, "CONFIG_POINT_TYPE_UNSPECIFIED",
                 pointPath + "/value_type", "点映射值类型不能为空");
        hasError = true;
      }
      const auto iedIt = ieds.find(table.conn_name());
      if (iedIt != ieds.end()) {
        const auto& configuredIed = iedIt->second->config();
        const bool sourceDisabled =
            (point.source() == IEC61850Proto::POINT_SOURCE_MMS &&
             !configuredIed.enable_mms()) ||
            (point.source() == IEC61850Proto::POINT_SOURCE_GOOSE &&
             !configuredIed.enable_goose()) ||
            (point.source() == IEC61850Proto::POINT_SOURCE_SV_DERIVED &&
             !configuredIed.enable_sv());
        if (sourceDisabled) {
          AddError(issues, "CONFIG_POINT_SOURCE_DISABLED",
                   pointPath + "/source",
                   "点映射来源对应的IED协议功能未启用");
          hasError = true;
        }

        const auto modelIt = models.find(configuredIed.model_name());
        if (modelIt != models.end() &&
            point.source() != IEC61850Proto::POINT_SOURCE_INTERNAL &&
            point.fc() != IEC61850Proto::FUNCTIONAL_CONSTRAINT_UNSPECIFIED &&
            !point.data_ref().empty()) {
          const IEC61850Proto::SclIed* modelIed = nullptr;
          for (const auto& candidateIed : modelIt->second->ieds()) {
            if (candidateIed.name() == configuredIed.ied_name()) {
              modelIed = &candidateIed;
              break;
            }
          }
          bool foundReference = point.source() ==
                                IEC61850Proto::POINT_SOURCE_SV_DERIVED;
          if (modelIed != nullptr) {
            const auto serverAccessPointCount =
                CountServerAccessPoints(*modelIed);
            if (point.source() == IEC61850Proto::POINT_SOURCE_MMS) {
              for (const auto& attribute : modelIed->data_attributes()) {
                if (attribute.data_ref() == point.data_ref() &&
                    attribute.fc() == point.fc() &&
                    BelongsToAccessPoint(attribute.access_point(),
                                         configuredIed.access_point(),
                                         serverAccessPointCount)) {
                  foundReference = true;
                  break;
                }
              }
            } else if (point.source() == IEC61850Proto::POINT_SOURCE_GOOSE) {
              for (const auto& extRef : modelIed->ext_refs()) {
                if (extRef.fc() == point.fc() &&
                    extRef.source_data_ref() == point.data_ref() &&
                    Upper(extRef.service_type()) == "GOOSE" &&
                    BelongsToAccessPoint(extRef.access_point(),
                                         configuredIed.access_point(),
                                         serverAccessPointCount)) {
                  foundReference = true;
                  break;
                }
              }
            }
          }
          if (!foundReference) {
            AddError(issues, "CONFIG_POINT_DATA_REF_NOT_FOUND",
                     pointPath + "/data_ref",
                     std::format("模型IED中不存在点引用: {}#{}",
                                 point.data_ref(),
                                 static_cast<int>(point.fc())));
            hasError = true;
          }
        }
      }
      if (!std::isfinite(point.scale()) || !std::isfinite(point.offset()) ||
          !std::isfinite(point.deadband()) || point.deadband() < 0) {
        AddError(issues, "CONFIG_POINT_ENGINEERING_INVALID", pointPath,
                 "scale、offset和deadband必须为有限数，且deadband不能小于0");
        hasError = true;
      }
    }
  }

  if (HasErrors(issues, hasError)) {
    return Invalid(issues, "IEC61850聚合配置校验失败");
  }
  return grpc::Status::OK;
}

}  // namespace IEC61850
