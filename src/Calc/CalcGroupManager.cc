#include "CalcGroupManager.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "CalcLibInfo.h"
#include "CalcValidation.h"
#include "Logger.h"
#include "ThreadUtil.hpp"
#include "mskdsp/Decimal20.hpp"

namespace Calc {
namespace {

grpc::Status makeNotFound(const std::string &groupName) {
  return grpc::Status(grpc::StatusCode::NOT_FOUND, std::format("未找到计算分组: {}", groupName));
}

grpc::Status makeInvalid(std::string message) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::move(message));
}

grpc::Status makePreconditionFailed(std::string message) {
  return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, std::move(message));
}

const char *groupStateToString(CalcProto::GroupState state) {
  switch (state) {
  case CalcProto::GROUP_STATE_RUNNING:
    return "运行中";
  case CalcProto::GROUP_STATE_PENDING_DELETE:
    return "待删除";
  case CalcProto::GROUP_STATE_STOPPED:
    return "已停止";
  case CalcProto::GROUP_STATE_UNSPECIFIED:
  default:
    return "未指定";
  }
}

struct ItemTags {
  std::string leftInputTag;
  std::string rightInputTag;
  std::vector<std::string> inputTags;
  std::string resultTag;
};

bool isAggregateOperator(CalcProto::OperatorKind op) {
  return op == CalcProto::OPERATOR_KIND_SUM || op == CalcProto::OPERATOR_KIND_AVERAGE;
}

ItemTags makeItemTags(const CalcProto::CalcItemConfig &item) {
  ItemTags tags;
  tags.resultTag = item.item_name() + "/result";
  if (isAggregateOperator(item.operator_kind())) {
    tags.inputTags.reserve(static_cast<size_t>(item.operands_size()));
    for (int index = 0; index < item.operands_size(); ++index) {
      tags.inputTags.emplace_back(std::format("{}/input_{}", item.item_name(), index + 1));
    }
  } else {
    tags.leftInputTag = item.item_name() + "/left_input";
    tags.rightInputTag = item.item_name() + "/right_input";
    tags.inputTags.push_back(tags.leftInputTag);
    // 保留旧版本为 NOT 生成 right_input 标签的行为，但 NOT 不会订阅或计算该槽位。
    tags.inputTags.push_back(tags.rightInputTag);
  }
  return tags;
}

struct RuntimeValue {
  enum class Type {
    kBool,
    kInt,
    kDouble,
    kDecimal,
  };

  Type type{Type::kInt};
  bool boolValue{false};
  int64_t intValue{0};
  std::optional<mskdsp::numeric::Decimal20> decimalValue;
  DataCenterProto::Quality quality{DataCenterProto::QUALITY_GOOD};
  int64_t tsMs{0};
};

struct PublishAction {
  std::string itemName;
  std::string tag;
  DataCenterProto::PointValue value;
  DataCenterProto::Quality quality{DataCenterProto::QUALITY_GOOD};
  int64_t tsMs{0};
  uint64_t triggerGeneration{0};
};

bool isNumericOperator(CalcProto::OperatorKind op) {
  switch (op) {
  case CalcProto::OPERATOR_KIND_ADD:
  case CalcProto::OPERATOR_KIND_SUB:
  case CalcProto::OPERATOR_KIND_MUL:
  case CalcProto::OPERATOR_KIND_DIV:
    return true;
  default:
    return false;
  }
}

bool isLogicOperator(CalcProto::OperatorKind op) {
  switch (op) {
  case CalcProto::OPERATOR_KIND_NOT:
  case CalcProto::OPERATOR_KIND_AND:
  case CalcProto::OPERATOR_KIND_OR:
  case CalcProto::OPERATOR_KIND_XOR:
    return true;
  default:
    return false;
  }
}

bool isPeriodicMode(const CalcProto::CalcGroupConfig &config) {
  return config.trigger_mode() == CalcProto::TRIGGER_MODE_PERIODIC;
}

bool operandUsesLegacyDoubleConstant(const CalcProto::OperandSpec &operand) {
  return operand.source_kind() == CalcProto::OPERAND_SOURCE_CONSTANT &&
      operand.has_constant() && operand.constant().has_double_value();
}

bool configUsesLegacyDoubleConstant(const CalcProto::CalcGroupConfig &config) {
  for (const auto &item : config.items()) {
    if (isAggregateOperator(item.operator_kind())) {
      for (const auto &operand : item.operands()) {
        if (operandUsesLegacyDoubleConstant(operand)) {
          return true;
        }
      }
      continue;
    }
    if ((item.has_left_operand() && operandUsesLegacyDoubleConstant(item.left_operand())) ||
        (item.has_right_operand() && operandUsesLegacyDoubleConstant(item.right_operand()))) {
      return true;
    }
  }
  return false;
}

DataCenterProto::Quality combineQuality(DataCenterProto::Quality lhs, DataCenterProto::Quality rhs) {
  if (lhs == DataCenterProto::QUALITY_BAD || rhs == DataCenterProto::QUALITY_BAD) {
    return DataCenterProto::QUALITY_BAD;
  }
  if (lhs == DataCenterProto::QUALITY_UNCERTAIN ||
      rhs == DataCenterProto::QUALITY_UNCERTAIN ||
      lhs == DataCenterProto::QUALITY_UNSPECIFIED ||
      rhs == DataCenterProto::QUALITY_UNSPECIFIED) {
    return DataCenterProto::QUALITY_UNCERTAIN;
  }
  return DataCenterProto::QUALITY_GOOD;
}

bool makeValueFromConstant(const CalcProto::TypedConstant &constant,
                           RuntimeValue *out,
                           std::string *error) {
  if (out == nullptr) {
    return false;
  }
  RuntimeValue value;
  value.quality = DataCenterProto::QUALITY_GOOD;
  value.tsMs = 0;
  if (constant.has_bool_value()) {
    value.type = RuntimeValue::Type::kBool;
    value.boolValue = constant.bool_value();
    *out = std::move(value);
    return true;
  }
  if (constant.has_int_value()) {
    auto parsed = mskdsp::numeric::Decimal20::FromInt64(constant.int_value());
    if (!parsed.has_value()) {
      if (error != nullptr) {
        *error = "int_value 超出 Decimal20 可表示范围";
      }
      return false;
    }
    value.type = RuntimeValue::Type::kInt;
    value.intValue = constant.int_value();
    value.decimalValue = std::move(*parsed);
    *out = std::move(value);
    return true;
  }
  if (constant.has_double_value()) {
    auto parsed = mskdsp::numeric::Decimal20::FromDouble(constant.double_value());
    if (!parsed.has_value()) {
      if (error != nullptr) {
        *error = "double_value 不是有限数或超出 Decimal20 可表示范围";
      }
      return false;
    }
    value.type = RuntimeValue::Type::kDouble;
    value.decimalValue = std::move(*parsed);
    *out = std::move(value);
    return true;
  }
  if (constant.has_decimal_value()) {
    auto parsed = mskdsp::numeric::Decimal20::Parse(constant.decimal_value());
    if (!parsed.has_value()) {
      if (error != nullptr) {
        *error = std::format("decimal_value 解析失败: {}",
                             mskdsp::numeric::DecimalErrorMessage(parsed.error()));
      }
      return false;
    }
    value.type = RuntimeValue::Type::kDecimal;
    value.decimalValue = std::move(*parsed);
    *out = std::move(value);
    return true;
  }
  if (error != nullptr) {
    *error = "constant 未设置可识别的值";
  }
  return false;
}

bool makeValueFromPointUpdate(const DataCenterProto::PointUpdate &update, RuntimeValue *out, std::string *error) {
  if (out == nullptr) {
    return false;
  }
  RuntimeValue value;
  value.quality = update.quality();
  value.tsMs = update.ts_ms();
  if (update.value().has_bool_value()) {
    value.type = RuntimeValue::Type::kBool;
    value.boolValue = update.value().bool_value();
    *out = value;
    return true;
  }
  if (update.value().has_int_value()) {
    auto parsed = mskdsp::numeric::Decimal20::FromInt64(update.value().int_value());
    if (!parsed.has_value()) {
      if (error != nullptr) {
        *error = std::format("tag={} 的 int_value 超出 Decimal20 可表示范围", update.dst_tag());
      }
      return false;
    }
    value.type = RuntimeValue::Type::kInt;
    value.intValue = update.value().int_value();
    value.decimalValue = std::move(*parsed);
    *out = value;
    return true;
  }
  if (update.value().has_double_value()) {
    auto parsed = mskdsp::numeric::Decimal20::FromDouble(update.value().double_value());
    if (!parsed.has_value()) {
      if (error != nullptr) {
        *error = std::format("tag={} 的 double_value 不是有限数或超出 Decimal20 可表示范围", update.dst_tag());
      }
      return false;
    }
    value.type = RuntimeValue::Type::kDouble;
    value.decimalValue = std::move(*parsed);
    *out = std::move(value);
    return true;
  }
  if (update.value().has_decimal_value()) {
    auto parsed = mskdsp::numeric::Decimal20::Parse(update.value().decimal_value());
    if (!parsed.has_value()) {
      if (error != nullptr) {
        *error = std::format("tag={} 的 decimal_value 解析失败: {}",
                             update.dst_tag(),
                             mskdsp::numeric::DecimalErrorMessage(parsed.error()));
      }
      return false;
    }
    value.type = RuntimeValue::Type::kDecimal;
    value.decimalValue = std::move(*parsed);
    *out = value;
    return true;
  }
  if (error != nullptr) {
    *error = std::format("tag={} 类型不支持，当前仅支持 bool/int64/double/decimal", update.dst_tag());
  }
  return false;
}

bool tryGetOperandValue(const CalcProto::OperandSpec &operand,
                        const std::string &tag,
                        const std::unordered_map<std::string, DataCenterProto::PointUpdate> &latestByTag,
                        RuntimeValue *out,
                        bool *missing,
                        std::string *error) {
  if (out == nullptr || missing == nullptr) {
    return false;
  }
  *missing = false;

  if (operand.source_kind() == CalcProto::OPERAND_SOURCE_CONSTANT) {
    return makeValueFromConstant(operand.constant(), out, error);
  }

  auto it = latestByTag.find(tag);
  if (it == latestByTag.end()) {
    *missing = true;
    return true;
  }
  return makeValueFromPointUpdate(it->second, out, error);
}

bool isNumericValue(const RuntimeValue &value) {
  return value.type != RuntimeValue::Type::kBool && value.decimalValue.has_value();
}

bool usesExplicitDecimal(const std::vector<RuntimeValue> &values) {
  return std::any_of(values.begin(), values.end(), [](const RuntimeValue &value) {
    return value.type == RuntimeValue::Type::kDecimal;
  });
}

bool setDecimalBoundaryValue(const mskdsp::numeric::Decimal20 &value,
                             bool publishDecimal,
                             DataCenterProto::PointValue *out,
                             std::string *error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "Decimal20 发布目标为空";
    }
    return false;
  }
  if (publishDecimal) {
    out->set_decimal_value(value.ToFixedString());
    return true;
  }
  auto doubleValue = value.ToDouble();
  if (!doubleValue.has_value()) {
    if (error != nullptr) {
      *error = std::format("Decimal20 转换为兼容 double 边界失败: {}",
                           mskdsp::numeric::DecimalErrorMessage(doubleValue.error()));
    }
    return false;
  }
  out->set_double_value(*doubleValue);
  return true;
}

void setDecimalOperationError(const CalcProto::CalcItemConfig &item,
                              std::string_view operation,
                              mskdsp::numeric::DecimalError decimalError,
                              std::string *error) {
  if (error == nullptr) {
    return;
  }
  if (decimalError == mskdsp::numeric::DecimalError::kDivisionByZero) {
    *error = std::format("item_name={} 除零，跳过本轮结果发布", item.item_name());
    return;
  }
  *error = std::format("item_name={} {}失败: {}",
                       item.item_name(),
                       operation,
                       mskdsp::numeric::DecimalErrorMessage(decimalError));
}

std::string joinTags(const std::vector<std::string> &tags) {
  std::string result;
  for (size_t index = 0; index < tags.size(); ++index) {
    if (index != 0) {
      result += ", ";
    }
    result += tags[index];
  }
  return result;
}

std::optional<PublishAction> evaluateItem(const CalcProto::CalcItemConfig &item, const std::unordered_map<std::string, DataCenterProto::PointUpdate> &latestByTag, std::string *error) {
  const auto tags = makeItemTags(item);
  std::vector<const CalcProto::OperandSpec *> operands;
  operands.reserve(static_cast<size_t>(isAggregateOperator(item.operator_kind()) ? item.operands_size() : 2));
  if (isAggregateOperator(item.operator_kind())) {
    for (const auto &operand : item.operands()) {
      operands.push_back(&operand);
    }
  } else {
    operands.push_back(&item.left_operand());
    if (item.has_right_operand()) {
      operands.push_back(&item.right_operand());
    }
  }

  std::vector<RuntimeValue> values;
  values.reserve(operands.size());
  std::vector<std::string> missingTags;
  for (size_t index = 0; index < operands.size(); ++index) {
    RuntimeValue value;
    bool missing = false;
    const auto &tag = tags.inputTags[index];
    if (!tryGetOperandValue(*operands[index], tag, latestByTag, &value, &missing, error)) {
      return std::nullopt;
    }
    if (missing) {
      missingTags.push_back(tag);
    }
    values.push_back(value);
  }
  if (!missingTags.empty()) {
    if (error != nullptr) {
      *error = std::format("item_name={} 等待输入: {} 尚未收到数据", item.item_name(), joinTags(missingTags));
    }
    return std::nullopt;
  }

  PublishAction action;
  action.itemName = item.item_name();
  action.tag = tags.resultTag;

  if (isAggregateOperator(item.operator_kind())) {
    for (const auto &value : values) {
      if (!isNumericValue(value)) {
        if (error != nullptr) {
          *error = std::format("item_name={} SUM/AVERAGE 仅接受 int64/double/decimal", item.item_name());
        }
        return std::nullopt;
      }
      action.quality = combineQuality(action.quality, value.quality);
      action.tsMs = std::max(action.tsMs, value.tsMs);
    }

    auto decimalSum = mskdsp::numeric::Decimal20::FromInt64(0);
    if (!decimalSum.has_value()) {
      if (error != nullptr) {
        *error = std::format("item_name={} 初始化 Decimal20 累加器失败", item.item_name());
      }
      return std::nullopt;
    }
    for (const auto &value : values) {
      auto next = decimalSum->Add(*value.decimalValue);
      if (!next.has_value()) {
        setDecimalOperationError(item, "求和", next.error(), error);
        return std::nullopt;
      }
      decimalSum = std::move(next);
    }

    if (item.operator_kind() == CalcProto::OPERATOR_KIND_AVERAGE) {
      if (values.size() > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
        if (error != nullptr) {
          *error = std::format("item_name={} 操作数数量超出可计算范围", item.item_name());
        }
        return std::nullopt;
      }
      auto divisor = mskdsp::numeric::Decimal20::FromInt64(
          static_cast<int64_t>(values.size()));
      if (!divisor.has_value()) {
        if (error != nullptr) {
          *error = std::format("item_name={} 构造平均值除数失败", item.item_name());
        }
        return std::nullopt;
      }
      auto average = decimalSum->Divide(*divisor);
      if (!average.has_value()) {
        setDecimalOperationError(item, "平均值计算", average.error(), error);
        return std::nullopt;
      }
      if (item.has_decimal_places()) {
        auto quantized = average->Quantize(
            item.decimal_places(),
            mskdsp::numeric::RoundingMode::kHalfAwayFromZero);
        if (!quantized.has_value()) {
          setDecimalOperationError(item, "平均值量化", quantized.error(), error);
          return std::nullopt;
        }
        average = std::move(quantized);
      }
      const bool publishDecimal = usesExplicitDecimal(values) ||
          (item.has_decimal_places() && item.decimal_places() > 15);
      if (!setDecimalBoundaryValue(*average, publishDecimal, &action.value, error)) {
        return std::nullopt;
      }
      return action;
    }

    const bool allInt = std::all_of(values.begin(), values.end(), [](const RuntimeValue &value) {
      return value.type == RuntimeValue::Type::kInt;
    });
    int64_t intResult = 0;
    bool overflow = false;
    if (allInt) {
      for (const auto &value : values) {
        int64_t next = 0;
        overflow = __builtin_add_overflow(intResult, value.intValue, &next);
        if (overflow) {
          break;
        }
        intResult = next;
      }
    }
    if (allInt && !overflow) {
      action.value.set_int_value(intResult);
    } else {
      if (!setDecimalBoundaryValue(*decimalSum, usesExplicitDecimal(values), &action.value, error)) {
        return std::nullopt;
      }
    }
    return action;
  }

  const auto &left = values[0];
  RuntimeValue right;
  if (values.size() > 1) {
    right = values[1];
  }

  if (isNumericOperator(item.operator_kind())) {
    const bool leftIsNumeric = isNumericValue(left);
    const bool rightIsNumeric = isNumericValue(right);
    if (!leftIsNumeric || !rightIsNumeric) {
      if (error != nullptr) {
        *error = std::format("item_name={} 数值运算仅接受 int64/double/decimal", item.item_name());
      }
      return std::nullopt;
    }

    action.quality = combineQuality(left.quality, right.quality);
    action.tsMs = std::max(left.tsMs, right.tsMs);

    const bool allInt = left.type == RuntimeValue::Type::kInt &&
        right.type == RuntimeValue::Type::kInt;
    int64_t result = 0;
    bool overflow = false;
    const bool publishDecimal = left.type == RuntimeValue::Type::kDecimal ||
        right.type == RuntimeValue::Type::kDecimal;

    const auto publishResult = [&](auto decimalResult, std::string_view operation) -> std::optional<PublishAction> {
      if (!decimalResult.has_value()) {
        setDecimalOperationError(item, operation, decimalResult.error(), error);
        return std::nullopt;
      }
      if (allInt && item.operator_kind() != CalcProto::OPERATOR_KIND_DIV && !overflow) {
        action.value.set_int_value(result);
      } else if (!setDecimalBoundaryValue(*decimalResult, publishDecimal, &action.value, error)) {
        return std::nullopt;
      }
      return action;
    };

    switch (item.operator_kind()) {
    case CalcProto::OPERATOR_KIND_ADD:
      if (allInt) {
        overflow = __builtin_add_overflow(left.intValue, right.intValue, &result);
      }
      return publishResult(left.decimalValue->Add(*right.decimalValue), "加法");
    case CalcProto::OPERATOR_KIND_SUB:
      if (allInt) {
        overflow = __builtin_sub_overflow(left.intValue, right.intValue, &result);
      }
      return publishResult(left.decimalValue->Subtract(*right.decimalValue), "减法");
    case CalcProto::OPERATOR_KIND_MUL:
      if (allInt) {
        overflow = __builtin_mul_overflow(left.intValue, right.intValue, &result);
      }
      return publishResult(left.decimalValue->Multiply(*right.decimalValue), "乘法");
    case CalcProto::OPERATOR_KIND_DIV:
      return publishResult(left.decimalValue->Divide(*right.decimalValue), "除法");
    default:
      break;
    }
  }

  if (isLogicOperator(item.operator_kind())) {
    if (left.type != RuntimeValue::Type::kBool) {
      if (error != nullptr) {
        *error = std::format("item_name={} 逻辑运算仅接受 bool", item.item_name());
      }
      return std::nullopt;
    }
    action.quality = left.quality;
    action.tsMs = left.tsMs;
    if (item.operator_kind() == CalcProto::OPERATOR_KIND_NOT) {
      action.value.set_bool_value(!left.boolValue);
      return action;
    }
    if (right.type != RuntimeValue::Type::kBool) {
      if (error != nullptr) {
        *error = std::format("item_name={} 逻辑运算仅接受 bool", item.item_name());
      }
      return std::nullopt;
    }
    action.quality = combineQuality(left.quality, right.quality);
    action.tsMs = std::max(left.tsMs, right.tsMs);
    switch (item.operator_kind()) {
    case CalcProto::OPERATOR_KIND_AND:
      action.value.set_bool_value(left.boolValue && right.boolValue);
      return action;
    case CalcProto::OPERATOR_KIND_OR:
      action.value.set_bool_value(left.boolValue || right.boolValue);
      return action;
    case CalcProto::OPERATOR_KIND_XOR:
      action.value.set_bool_value(left.boolValue != right.boolValue);
      return action;
    default:
      break;
    }
  }

  if (error != nullptr) {
    *error = std::format("item_name={} operator_kind 非法", item.item_name());
  }
  return std::nullopt;
}

void applyTriggerTimestamp(std::vector<PublishAction> *actions,
                           const std::unordered_map<std::string, int64_t> &firstTriggerTsByItem,
                           const std::unordered_map<std::string, uint64_t> &triggerGenerationByItem) {
  if (actions == nullptr) {
    return;
  }
  for (auto &action : *actions) {
    auto it = firstTriggerTsByItem.find(action.itemName);
    if (it != firstTriggerTsByItem.end()) {
      action.tsMs = it->second;
    }
    auto generationIt = triggerGenerationByItem.find(action.itemName);
    if (generationIt != triggerGenerationByItem.end()) {
      action.triggerGeneration = generationIt->second;
    }
  }
}

void recordFirstTriggerTimestamp(const CalcProto::CalcGroupConfig &config,
                                 const DataCenterProto::PointUpdate &update,
                                 std::unordered_map<std::string, int64_t> *firstTriggerTsByItem,
                                 std::unordered_map<std::string, uint64_t> *triggerGenerationByItem,
                                 uint64_t *nextTriggerGeneration) {
  if (firstTriggerTsByItem == nullptr || triggerGenerationByItem == nullptr || nextTriggerGeneration == nullptr) {
    return;
  }
  for (const auto &item : config.items()) {
    const auto tags = makeItemTags(item);
    bool isTriggerInput = false;
    for (size_t index = 0; index < tags.inputTags.size(); ++index) {
      const CalcProto::OperandSpec *operand = nullptr;
      if (isAggregateOperator(item.operator_kind())) {
        if (index < static_cast<size_t>(item.operands_size())) {
          operand = &item.operands(static_cast<int>(index));
        }
      } else if (index == 0) {
        operand = &item.left_operand();
      } else if (index == 1 && item.has_right_operand()) {
        operand = &item.right_operand();
      }
      if (operand != nullptr && operand->source_kind() == CalcProto::OPERAND_SOURCE_ROUTED_INPUT &&
          tags.inputTags[index] == update.dst_tag()) {
        isTriggerInput = true;
        break;
      }
    }
    if (isTriggerInput && !firstTriggerTsByItem->contains(item.item_name())) {
      (*firstTriggerTsByItem)[item.item_name()] = update.ts_ms();
      (*triggerGenerationByItem)[item.item_name()] = ++(*nextTriggerGeneration);
    }
  }
}

std::vector<PublishAction> evaluateGroupLocked(const CalcProto::CalcGroupConfig &config,
                                               const std::unordered_map<std::string, DataCenterProto::PointUpdate> &latestByTag,
                                               std::vector<std::string> *errors,
                                               std::unordered_map<std::string, std::string> *itemLastErrors) {
  std::vector<PublishAction> actions;
  for (const auto &item : config.items()) {
    std::string error;
    auto action = evaluateItem(item, latestByTag, &error);
    if (itemLastErrors != nullptr) {
      (*itemLastErrors)[item.item_name()] = error;
    }
    if (!error.empty() && errors != nullptr) {
      errors->push_back(error);
    }
    if (action.has_value()) {
      actions.push_back(std::move(*action));
    }
  }
  return actions;
}

void fillOperandStatuses(const CalcProto::CalcItemConfig &item,
                         const std::unordered_map<std::string, DataCenterProto::PointUpdate> &latestByTag,
                         CalcProto::CalcItemInfo *itemInfo) {
  if (itemInfo == nullptr) {
    return;
  }
  const auto tags = makeItemTags(item);
  const auto addStatus = [&](int index, const CalcProto::OperandSpec &operand) {
    auto *status = itemInfo->add_operand_status();
    const auto &tag = tags.inputTags[static_cast<size_t>(index)];
    status->set_index(static_cast<uint32_t>(index));
    status->set_input_tag(tag);
    if (operand.source_kind() == CalcProto::OPERAND_SOURCE_CONSTANT) {
      status->set_ready(true);
      status->set_quality(DataCenterProto::QUALITY_GOOD);
      status->set_ts_ms(0);
      return;
    }
    auto it = latestByTag.find(tag);
    if (it == latestByTag.end()) {
      status->set_ready(false);
      status->set_reason("尚未收到输入数据");
      return;
    }
    status->set_quality(it->second.quality());
    status->set_ts_ms(it->second.ts_ms());
    if (it->second.value().kind_case() == DataCenterProto::PointValue::KIND_NOT_SET) {
      status->set_ready(false);
      status->set_reason("已收到输入，但值类型不支持");
      return;
    }
    const bool numericOperation = isNumericOperator(item.operator_kind()) ||
        isAggregateOperator(item.operator_kind());
    const bool numericValue = it->second.value().has_int_value() ||
        it->second.value().has_double_value() ||
        it->second.value().has_decimal_value();
    if (numericOperation && !numericValue) {
      status->set_ready(false);
      status->set_reason("已收到输入，但类型不支持当前数值运算");
      return;
    }
    if (isLogicOperator(item.operator_kind()) && !it->second.value().has_bool_value()) {
      status->set_ready(false);
      status->set_reason("已收到输入，但类型不支持当前逻辑运算");
      return;
    }
    status->set_ready(true);
    if (it->second.quality() == DataCenterProto::QUALITY_BAD) {
      status->set_reason("已收到输入，但质量为 BAD");
    } else if (it->second.quality() == DataCenterProto::QUALITY_UNCERTAIN ||
               it->second.quality() == DataCenterProto::QUALITY_UNSPECIFIED) {
      status->set_reason("已收到输入，但质量为不确定");
    }
  };

  if (isAggregateOperator(item.operator_kind())) {
    for (int index = 0; index < item.operands_size(); ++index) {
      addStatus(index, item.operands(index));
    }
    return;
  }
  addStatus(0, item.left_operand());
  if (item.has_right_operand()) {
    addStatus(1, item.right_operand());
  }
}

}  // namespace

GroupManager::GroupManager(std::string moduleName, std::filesystem::path configDbPath) :
  groupStore_(std::move(configDbPath)),
  dataCenter_(std::move(moduleName)) {}

GroupManager::~GroupManager() {
  Shutdown();
}

void GroupManager::Shutdown() {
  std::vector<std::jthread> stoppingThreads;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (shutdown_) {
      return;
    }
    shutdown_ = true;
    stoppingThreads.reserve(groupsByName_.size() * 2);
    for (auto &[groupName, group] : groupsByName_) {
      std::jthread subscribeThread;
      std::jthread periodicThread;
      stopThreadsLocked(&group, false, &subscribeThread, &periodicThread);
      if (subscribeThread.joinable()) {
        stoppingThreads.push_back(std::move(subscribeThread));
      }
      if (periodicThread.joinable()) {
        stoppingThreads.push_back(std::move(periodicThread));
      }
      LOG_DEBUG("Calc 析构时已请求停止分组线程: group_name={}", groupName);
    }
  }
  stoppingThreads.clear();
  LOG_INFO("Calc 分组管理器已停止全部运算线程");
}

void GroupManager::setDataCenterServerAddress(std::string address) {
  dataCenter_.setServerAddress(std::move(address));
}

void GroupManager::setDataCenterStub(std::shared_ptr<DataCenterProto::DataCenterService::StubInterface> stub) {
  dataCenter_.setStub(std::move(stub));
}

grpc::Status GroupManager::validateGroupName(const std::string &groupName) const {
  if (groupName.empty()) {
    return makeInvalid("group_name 不能为空");
  }
  return grpc::Status::OK;
}

grpc::Status GroupManager::validateGroupConfig(const CalcProto::CalcGroupConfig &config) const {
  return ValidateGroupConfig(config);
}

grpc::Status GroupManager::fillGroupInfoLocked(const GroupRuntime &group, CalcProto::CalcGroupInfo *out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  out->Clear();
  *out->mutable_config() = group.config;
  out->set_conn_id(group.connId);
  out->set_state(group.state);
  out->set_last_error(group.lastError);
  for (const auto &item : group.config.items()) {
    auto *itemInfo = out->add_items();
    *itemInfo->mutable_config() = item;
    const auto tags = makeItemTags(item);
    if (!isAggregateOperator(item.operator_kind())) {
      itemInfo->set_left_input_tag(tags.leftInputTag);
      itemInfo->set_right_input_tag(tags.rightInputTag);
    }
    itemInfo->set_result_tag(tags.resultTag);
    for (const auto &tag : tags.inputTags) {
      itemInfo->add_input_tags(tag);
    }
    fillOperandStatuses(item, group.latestByTag, itemInfo);
    auto errorIt = group.itemLastErrors.find(item.item_name());
    if (errorIt != group.itemLastErrors.end()) {
      itemInfo->set_last_error(errorIt->second);
    }
  }
  return grpc::Status::OK;
}

grpc::Status GroupManager::checkStartPreconditionsLocked(const GroupRuntime &group) const {
  if (group.state == CalcProto::GROUP_STATE_PENDING_DELETE) {
    return makePreconditionFailed("分组处于待删除状态");
  }
  auto status = validateGroupConfig(group.config);
  if (!status.ok()) {
    return makePreconditionFailed(std::format("分组配置未通过当前校验: {}", status.error_message()));
  }
  if (group.connId == 0) {
    return makePreconditionFailed("分组 conn_id 无效");
  }
  return grpc::Status::OK;
}

grpc::Status GroupManager::tryAutoStartGroup(const std::string &groupName, std::string_view trigger) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      return makeNotFound(groupName);
    }
    if (shutdown_) {
      return makePreconditionFailed("分组管理器正在停止，不能启动分组功能");
    }
    if (it->second.state == CalcProto::GROUP_STATE_RUNNING) {
      LOG_INFO("Calc 自动启动分组跳过: group_name={}, 触发来源={}, 原因=分组已在运行", groupName, trigger);
      return grpc::Status::OK;
    }
    auto status = checkStartPreconditionsLocked(it->second);
    if (!status.ok()) {
      it->second.lastError = status.error_message();
      LOG_INFO("Calc 自动启动分组跳过: group_name={}, 触发来源={}, 原因={}", groupName, trigger, status.error_message());
      return status;
    }
  }

  LOG_INFO("Calc 自动启动分组: group_name={}, 触发来源={}", groupName, trigger);
  auto status = StartGroup(groupName);
  if (!status.ok()) {
    LOG_WARNING("Calc 自动启动分组失败: group_name={}, 触发来源={}, 原因={}", groupName, trigger, status.error_message());
  } else {
    LOG_INFO("Calc 自动启动分组成功: group_name={}, 触发来源={}", groupName, trigger);
  }
  return status;
}

void GroupManager::TryAutoStartReadyGroups(std::string_view trigger) {
  std::vector<std::string> groupNames;
  {
    std::lock_guard<std::mutex> lock(mu_);
    groupNames.reserve(groupsByName_.size());
    for (const auto &[groupName, group] : groupsByName_) {
      if (group.state == CalcProto::GROUP_STATE_STOPPED) {
        groupNames.push_back(groupName);
      }
    }
  }
  if (groupNames.empty()) {
    LOG_INFO("Calc 自动启动检查完成: 触发来源={}, 当前无可评估分组", trigger);
    return;
  }
  for (const auto &groupName : groupNames) {
    (void)tryAutoStartGroup(groupName, trigger);
  }
}

CalcProto::GroupsConfig GroupManager::dumpGroupsConfigLocked() const {
  CalcProto::GroupsConfig config;
  for (const auto &[_, group] : groupsByName_) {
    auto *persisted = config.add_persisted_groups();
    *persisted->mutable_config() = group.config;
    persisted->set_pending_delete(group.state == CalcProto::GROUP_STATE_PENDING_DELETE);
  }
  return config;
}

grpc::Status GroupManager::saveGroupsLocked() {
  auto config = dumpGroupsConfigLocked();
  auto status = groupStore_.Save(config);
  if (!status.ok()) {
    LOG_ERROR("Calc 分组配置落盘失败: 原因={}", status.error_message());
  }
  return status;
}

grpc::Status GroupManager::restoreGroupFromConfig(const CalcProto::PersistedGroup &persisted) {
  auto status = validateGroupConfig(persisted.config());
  if (!status.ok()) {
    return status;
  }
  LOG_INFO("Calc 开始恢复分组持久化记录: group_name={}, 状态={}, item 数={}", persisted.config().group_name(), groupStateToString(persisted.pending_delete() ? CalcProto::GROUP_STATE_PENDING_DELETE : CalcProto::GROUP_STATE_STOPPED), persisted.config().items_size());

  DataCenterProto::ConnectionInfo connInfo;
  status = dataCenter_.GetOrCreateConnection(persisted.config().group_name(), &connInfo);
  if (!status.ok()) {
    LOG_ERROR("Calc 恢复分组时获取 DataCenter 连接失败: group_name={}, 原因={}", persisted.config().group_name(), status.error_message());
    return status;
  }
  if (connInfo.conn_id() == 0) {
    return grpc::Status(grpc::StatusCode::INTERNAL, "DataCenter 返回 conn_id=0");
  }

  GroupRuntime runtime;
  runtime.config = persisted.config();
  runtime.connId = connInfo.conn_id();
  runtime.state = persisted.pending_delete() ? CalcProto::GROUP_STATE_PENDING_DELETE
                                             : CalcProto::GROUP_STATE_STOPPED;
  rebuildTagCache(&runtime);

  auto tags = collectAllTags(runtime.config);
  if (!tags.empty()) {
    std::vector<std::string> tagList;
    tagList.reserve(tags.size());
    for (const auto &tag : tags) {
      tagList.push_back(tag);
    }
    auto connTagsStatus = dataCenter_.UpsertConnTags(runtime.connId, tagList, true);
    if (!connTagsStatus.ok()) {
      runtime.lastError = connTagsStatus.error_message();
      LOG_ERROR("Calc 恢复分组时同步 DataCenter 连接标签注册表失败: group_name={}, conn_id={}, 标签数={}, 原因={}", runtime.config.group_name(), runtime.connId, tagList.size(), connTagsStatus.error_message());
      std::lock_guard<std::mutex> lock(mu_);
      groupsByName_[runtime.config.group_name()] = std::move(runtime);
      return connTagsStatus;
    }
    LOG_INFO("Calc 恢复分组时已同步 DataCenter 连接标签注册表: group_name={}, conn_id={}, 标签数={}", runtime.config.group_name(), runtime.connId, tagList.size());
  }

  std::lock_guard<std::mutex> lock(mu_);
  groupsByName_[runtime.config.group_name()] = std::move(runtime);
  return grpc::Status::OK;
}

grpc::Status GroupManager::LoadPersistedConfig() {
  CalcProto::GroupsConfig config;
  auto status = groupStore_.Load(&config);
  if (!status.ok()) {
    LOG_ERROR("Calc 分组配置加载失败: 原因={}", status.error_message());
    return status;
  }
  if (config.persisted_groups_size() == 0) {
    LOG_INFO("Calc 未发现本地分组配置");
    return grpc::Status::OK;
  }

  size_t restored = 0;
  size_t failed = 0;
  for (const auto &persisted : config.persisted_groups()) {
    if (!persisted.has_config()) {
      ++failed;
      LOG_ERROR("Calc 恢复分组失败: group_name=<空>, 原因=持久化记录缺少 config");
      continue;
    }
    status = restoreGroupFromConfig(persisted);
    if (!status.ok()) {
      ++failed;
      LOG_ERROR("Calc 恢复分组失败: group_name={}, 原因={}", persisted.config().group_name(), status.error_message());
      continue;
    }
    ++restored;
    LOG_INFO("Calc 已恢复分组配置: group_name={}, 状态={}", persisted.config().group_name(), groupStateToString(persisted.pending_delete() ? CalcProto::GROUP_STATE_PENDING_DELETE : CalcProto::GROUP_STATE_STOPPED));
  }

  LOG_INFO("Calc 分组配置恢复完成: 成功={}, 失败={}", restored, failed);
  TryAutoStartReadyGroups("持久化恢复完成后");
  if (failed > 0) {
    return grpc::Status(grpc::StatusCode::INTERNAL, "部分分组恢复失败");
  }
  return grpc::Status::OK;
}

grpc::Status GroupManager::UpsertGroup(const CalcProto::UpsertGroupRequest &request, CalcProto::CalcGroupInfo *out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  auto status = validateGroupConfig(request.config());
  if (!status.ok()) {
    return status;
  }
  const auto groupName = request.config().group_name();
  uint32_t connId = 0;

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it != groupsByName_.end()) {
      if (request.create_only()) {
        return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "group_name 已存在");
      }
      if (it->second.state == CalcProto::GROUP_STATE_RUNNING) {
        return makePreconditionFailed("更新配置前请先停止分组");
      }
      if (it->second.state == CalcProto::GROUP_STATE_PENDING_DELETE) {
        return makePreconditionFailed("分组处于待删除状态");
      }
      it->second.config = request.config();
      it->second.latestByTag.clear();
      it->second.firstTriggerTsByItem.clear();
      it->second.triggerGenerationByItem.clear();
      it->second.itemLastErrors.clear();
      it->second.lastError.clear();
      rebuildTagCache(&it->second);
      connId = it->second.connId;
    } else {
      if (request.create_only()) {
        bool exists = false;
        status = dataCenter_.ConnectionExists(groupName, &exists);
        if (!status.ok()) {
          return status;
        }
        if (exists) {
          return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "group_name 已存在");
        }
      }

      DataCenterProto::ConnectionInfo connInfo;
      status = dataCenter_.GetOrCreateConnection(groupName, &connInfo);
      if (!status.ok()) {
        return status;
      }
      if (connInfo.conn_id() == 0) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "DataCenter 返回 conn_id=0");
      }

      auto [pos, _] = groupsByName_.try_emplace(groupName);
      auto &group = pos->second;
      group.config = request.config();
      group.connId = connInfo.conn_id();
      group.state = CalcProto::GROUP_STATE_STOPPED;
      group.lastError.clear();
      group.firstTriggerTsByItem.clear();
      group.triggerGenerationByItem.clear();
      group.itemLastErrors.clear();
      rebuildTagCache(&group);
      connId = group.connId;
    }

    status = saveGroupsLocked();
    if (!status.ok()) {
      auto saveIt = groupsByName_.find(groupName);
      if (saveIt != groupsByName_.end()) {
        saveIt->second.lastError = status.error_message();
      }
      return status;
    }
  }

  if (configUsesLegacyDoubleConstant(request.config())) {
    LOG_WARNING("Calc 分组配置包含旧 double 常量，将按最短往返十进制文本转换为 Decimal20: group_name={}",
                groupName);
  }

  std::vector<std::string> tagList;
  auto tags = collectAllTags(request.config());
  tagList.reserve(tags.size());
  for (const auto &tag : tags) {
    tagList.push_back(tag);
  }
  status = dataCenter_.UpsertConnTags(connId, tagList, true);
  if (!status.ok()) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it != groupsByName_.end()) {
      it->second.lastError = status.error_message();
    }
    LOG_ERROR("Calc 同步 DataCenter 连接标签注册表失败: group_name={}, conn_id={}, 标签数={}, 原因={}", groupName, connId, tagList.size(), status.error_message());
    return status;
  }
  LOG_INFO("Calc 已同步 DataCenter 连接标签注册表: group_name={}, conn_id={}, 标签数={}", groupName, connId, tagList.size());

  (void)tryAutoStartGroup(groupName, "分组配置更新成功");
  std::lock_guard<std::mutex> lock(mu_);
  auto it = groupsByName_.find(groupName);
  if (it == groupsByName_.end()) {
    return makeNotFound(groupName);
  }
  return fillGroupInfoLocked(it->second, out);
}

grpc::Status GroupManager::RenameGroup(const std::string &oldGroupName, const std::string &newGroupName, CalcProto::CalcGroupInfo *out) {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  auto status = validateGroupName(oldGroupName);
  if (!status.ok()) {
    return status;
  }
  status = validateGroupName(newGroupName);
  if (!status.ok()) {
    return status;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(oldGroupName);
    if (it == groupsByName_.end()) {
      return makeNotFound(oldGroupName);
    }
    if (oldGroupName == newGroupName) {
      return fillGroupInfoLocked(it->second, out);
    }
    if (groupsByName_.contains(newGroupName)) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "group_name 已存在");
    }
    if (it->second.state == CalcProto::GROUP_STATE_RUNNING) {
      return makePreconditionFailed("更新配置前请先停止分组");
    }
    if (it->second.state == CalcProto::GROUP_STATE_PENDING_DELETE) {
      return makePreconditionFailed("分组处于待删除状态");
    }
  }

  DataCenterProto::ConnectionInfo connInfo;
  status = dataCenter_.RenameConnection(oldGroupName, newGroupName, &connInfo);
  if (!status.ok()) {
    return status;
  }
  if (connInfo.conn_id() == 0) {
    return grpc::Status(grpc::StatusCode::INTERNAL, "DataCenter 返回 conn_id=0");
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto node = groupsByName_.extract(oldGroupName);
    if (node.empty()) {
      return makeNotFound(oldGroupName);
    }
    node.key() = newGroupName;
    node.mapped().config.set_group_name(newGroupName);
    node.mapped().connId = connInfo.conn_id();
    node.mapped().lastError.clear();
    groupsByName_.insert(std::move(node));

    status = saveGroupsLocked();
    if (!status.ok()) {
      auto it = groupsByName_.find(newGroupName);
      if (it != groupsByName_.end()) {
        it->second.lastError = status.error_message();
      }
      return status;
    }

    auto it = groupsByName_.find(newGroupName);
    if (it == groupsByName_.end()) {
      return makeNotFound(newGroupName);
    }
    return fillGroupInfoLocked(it->second, out);
  }
}

grpc::Status GroupManager::GetGroup(const std::string &groupName, CalcProto::CalcGroupInfo *out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  auto status = validateGroupName(groupName);
  if (!status.ok()) {
    return status;
  }
  std::lock_guard<std::mutex> lock(mu_);
  auto it = groupsByName_.find(groupName);
  if (it == groupsByName_.end()) {
    return makeNotFound(groupName);
  }
  return fillGroupInfoLocked(it->second, out);
}

grpc::Status GroupManager::ListGroups(CalcProto::ListGroupsResponse *out) const {
  if (out == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "out 为空");
  }
  std::lock_guard<std::mutex> lock(mu_);
  out->Clear();
  for (const auto &[_, group] : groupsByName_) {
    auto *item = out->add_groups();
    auto status = fillGroupInfoLocked(group, item);
    if (!status.ok()) {
      return status;
    }
  }
  return grpc::Status::OK;
}

void GroupManager::startThreadsLocked(const std::string &groupName, GroupRuntime *group) {
  if (group == nullptr) {
    return;
  }
  rebuildTagCache(group);

  const auto connId = group->connId;
  std::vector<std::string> subscribeTags(group->subscribeTags.begin(), group->subscribeTags.end());
  if (connId == 0) {
    group->state = CalcProto::GROUP_STATE_STOPPED;
    group->lastError = "分组连接无效";
    LOG_WARNING("Calc 分组启动运算功能失败: group_name={}, conn_id={}, 原因={}", groupName, connId, group->lastError);
    return;
  }

  if (subscribeTags.empty()) {
    group->dcSubscribeContext.reset();
    LOG_INFO("Calc 分组无需订阅 DataCenter 输入: group_name={}, conn_id={}", groupName, connId);
  } else {
    group->dcSubscribeContext = std::make_shared<grpc::ClientContext>();
    auto context = group->dcSubscribeContext;
    auto reader = dataCenter_.Subscribe(context.get(), connId, subscribeTags, true);
    if (!reader) {
      group->dcSubscribeContext.reset();
      group->state = CalcProto::GROUP_STATE_STOPPED;
      group->lastError = "建立 DataCenter 订阅失败";
      LOG_ERROR("Calc 建立 DataCenter 订阅失败: group_name={}, conn_id={}, 标签数={}", groupName, connId, subscribeTags.size());
      return;
    }

    group->dcSubscribeThread = ModuleManager::StartModuleThread(
        CalcLibInfo.LIB_NAME,
        [this, groupName, context, reader = std::move(reader)](std::stop_token st) mutable {
          std::stop_callback callback(st, [context]() { context->TryCancel(); });
          DataCenterProto::PointUpdate update;
          while (reader->Read(&update)) {
            handleUpdate(groupName, update);
          }
          auto finishStatus = reader->Finish();
          std::lock_guard<std::mutex> lock(mu_);
          auto it = groupsByName_.find(groupName);
          if (it == groupsByName_.end()) {
            return;
          }
          if (it->second.state == CalcProto::GROUP_STATE_RUNNING) {
            if (it->second.periodicThread.joinable()) {
              it->second.periodicThread.request_stop();
            }
            it->second.state = CalcProto::GROUP_STATE_STOPPED;
            it->second.lastError = finishStatus.ok() ? "DataCenter 订阅流已结束"
                                                     : std::format("DataCenter 订阅失败: {}", finishStatus.error_message());
            LOG_WARNING("Calc 分组订阅线程退出: group_name={}, 原因={}", groupName, it->second.lastError);
          }
        });
    LOG_INFO("Calc 分组已启用 DataCenter 订阅: group_name={}, conn_id={}, 订阅标签数={}", groupName, connId, subscribeTags.size());
  }

  if (isPeriodicMode(group->config)) {
    const auto period = std::chrono::milliseconds(group->config.period_ms());
    group->periodicThread = ModuleManager::StartModuleThread(
        CalcLibInfo.LIB_NAME,
        [this, groupName, period](std::stop_token st) {
          std::condition_variable_any wakeup;
          std::mutex wakeupMutex;
          std::unique_lock<std::mutex> lock(wakeupMutex);
          auto nextDeadline = std::chrono::steady_clock::now() + period;
          while (!st.stop_requested()) {
            wakeup.wait_until(lock, st, nextDeadline, [] { return false; });
            if (st.stop_requested()) {
              break;
            }
            handlePeriodicCalculation(groupName);
            nextDeadline += period;
            const auto now = std::chrono::steady_clock::now();
            while (nextDeadline <= now) {
              nextDeadline += period;
            }
          }
        });
    LOG_INFO("Calc 分组已启用周期计算: group_name={}, 周期毫秒={}", groupName, group->config.period_ms());
  }
}

grpc::Status GroupManager::StartGroup(const std::string &groupName) {
  auto status = validateGroupName(groupName);
  if (!status.ok()) {
    return status;
  }

  {
    std::jthread staleSubscribeThread;
    std::jthread stalePeriodicThread;
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      return makeNotFound(groupName);
    }
    if (shutdown_) {
      return makePreconditionFailed("分组管理器正在停止，不能启动分组功能");
    }
    if (it->second.state == CalcProto::GROUP_STATE_STOPPED &&
        (it->second.dcSubscribeThread.joinable() || it->second.periodicThread.joinable())) {
      LOG_INFO("Calc 启动分组前回收已停止线程: group_name={}", groupName);
      stopThreadsLocked(&it->second, true, &staleSubscribeThread, &stalePeriodicThread);
    }
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      return makeNotFound(groupName);
    }
    if (shutdown_) {
      return makePreconditionFailed("分组管理器正在停止，不能启动分组功能");
    }
    if (it->second.state == CalcProto::GROUP_STATE_RUNNING) {
      LOG_INFO("Calc 启动分组请求幂等成功: group_name={}, 原因=分组已在运行", groupName);
      return grpc::Status::OK;
    }
    status = checkStartPreconditionsLocked(it->second);
    if (!status.ok()) {
      it->second.lastError = status.error_message();
      return status;
    }

    it->second.latestByTag.clear();
    it->second.firstTriggerTsByItem.clear();
    it->second.triggerGenerationByItem.clear();
    it->second.itemLastErrors.clear();
    it->second.lastError.clear();
    it->second.state = CalcProto::GROUP_STATE_RUNNING;
    startThreadsLocked(groupName, &it->second);
    if (it->second.state != CalcProto::GROUP_STATE_RUNNING ||
        (!it->second.subscribeTags.empty() && !it->second.dcSubscribeThread.joinable()) ||
        (isPeriodicMode(it->second.config) && !it->second.periodicThread.joinable())) {
      if (it->second.lastError.empty()) {
        it->second.lastError = "建立 DataCenter 订阅失败";
      }
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, it->second.lastError);
    }
    if (!isPeriodicMode(it->second.config)) {
      std::vector<std::string> startupErrors;
      auto startupActions = evaluateGroupLocked(
          it->second.config, it->second.latestByTag, &startupErrors, &it->second.itemLastErrors);
      for (const auto &error : startupErrors) {
        LOG_WARNING("Calc 启动时计算跳过: group_name={}, 原因={}", groupName, error);
      }
      for (const auto &action : startupActions) {
        auto publishStatus = dataCenter_.PublishValue(
            it->second.connId, action.tag, action.value, action.quality, action.tsMs);
        if (!publishStatus.ok()) {
          LOG_ERROR("Calc 启动时发布结果失败: group_name={}, item_name={}, tag={}, 原因={}", groupName, action.itemName, action.tag, publishStatus.error_message());
          it->second.lastError = std::format("发布结果失败: {}", publishStatus.error_message());
        } else {
          LOG_DEBUG("Calc 启动时已发布结果: group_name={}, item_name={}, tag={}, quality={}, ts_ms={}",
                    groupName,
                    action.itemName,
                    action.tag,
                    static_cast<int>(action.quality),
                    action.tsMs);
        }
      }
    }
  }

  LOG_INFO("Calc 分组已启动运算功能: group_name={}", groupName);
  return grpc::Status::OK;
}

void GroupManager::stopThreadsLocked(GroupRuntime *group,
                                      bool keepPendingDeleteState,
                                      std::jthread *outSubscribeThread,
                                      std::jthread *outPeriodicThread) {
  if (group == nullptr) {
    return;
  }
  if (group->dcSubscribeContext != nullptr) {
    group->dcSubscribeContext->TryCancel();
  }
  if (group->dcSubscribeThread.joinable()) {
    group->dcSubscribeThread.request_stop();
  }
  if (outSubscribeThread != nullptr) {
    *outSubscribeThread = std::move(group->dcSubscribeThread);
  }
  if (group->periodicThread.joinable()) {
    group->periodicThread.request_stop();
  }
  if (outPeriodicThread != nullptr) {
    *outPeriodicThread = std::move(group->periodicThread);
  }
  group->dcSubscribeContext.reset();
  group->firstTriggerTsByItem.clear();
  group->triggerGenerationByItem.clear();
  if (!keepPendingDeleteState || group->state != CalcProto::GROUP_STATE_PENDING_DELETE) {
    group->state = CalcProto::GROUP_STATE_STOPPED;
  }
}

grpc::Status GroupManager::StopGroup(const std::string &groupName) {
  auto status = validateGroupName(groupName);
  if (!status.ok()) {
    return status;
  }

  std::jthread subscribeThread;
  std::jthread periodicThread;
  bool alreadyStopped = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      return makeNotFound(groupName);
    }
    if (it->second.state == CalcProto::GROUP_STATE_STOPPED) {
      alreadyStopped = true;
      if (!it->second.dcSubscribeThread.joinable() && !it->second.periodicThread.joinable()) {
        LOG_INFO("Calc 停止分组请求幂等成功: group_name={}, 原因=分组已停止", groupName);
        return grpc::Status::OK;
      }
    }
    stopThreadsLocked(&it->second, true, &subscribeThread, &periodicThread);
  }

  if (subscribeThread.joinable()) {
    subscribeThread.join();
  }
  if (periodicThread.joinable()) {
    periodicThread.join();
  }

  if (alreadyStopped) {
    LOG_INFO("Calc 停止分组请求幂等成功并已回收残留线程: group_name={}", groupName);
  } else {
    LOG_INFO("Calc 分组已停止运算功能: group_name={}", groupName);
  }
  return grpc::Status::OK;
}

grpc::Status GroupManager::DeleteGroup(const std::string &groupName) {
  auto status = validateGroupName(groupName);
  if (!status.ok()) {
    return status;
  }

  std::jthread subscribeThread;
  std::jthread periodicThread;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      return makeNotFound(groupName);
    }
    stopThreadsLocked(&it->second, false, &subscribeThread, &periodicThread);
  }

  if (subscribeThread.joinable()) {
    subscribeThread.join();
  }
  if (periodicThread.joinable()) {
    periodicThread.join();
  }

  status = dataCenter_.DeleteConnection(groupName);
  if (!status.ok() && status.error_code() != grpc::StatusCode::NOT_FOUND) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it != groupsByName_.end()) {
      it->second.state = CalcProto::GROUP_STATE_PENDING_DELETE;
      it->second.lastError = status.error_message();
      (void)saveGroupsLocked();
    }
    return status;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    groupsByName_.erase(groupName);
    status = saveGroupsLocked();
    if (!status.ok()) {
      return status;
    }
  }

  LOG_INFO("Calc 已删除分组: group_name={}", groupName);
  return grpc::Status::OK;
}

void GroupManager::handleUpdate(const std::string &groupName, const DataCenterProto::PointUpdate &update) {
  std::vector<PublishAction> actions;
  std::vector<std::string> errors;
  uint32_t connId = 0;
  bool changeMode = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end() || it->second.state != CalcProto::GROUP_STATE_RUNNING) {
      return;
    }
    it->second.latestByTag[update.dst_tag()] = update;
    connId = it->second.connId;
    if (isPeriodicMode(it->second.config)) {
      LOG_DEBUG("Calc 周期分组已更新输入缓存: group_name={}, tag={}, ts_ms={}", groupName, update.dst_tag(), update.ts_ms());
      return;
    }
    changeMode = true;
    recordFirstTriggerTimestamp(it->second.config,
                                update,
                                &it->second.firstTriggerTsByItem,
                                &it->second.triggerGenerationByItem,
                                &it->second.nextTriggerGeneration);
    actions = evaluateGroupLocked(it->second.config, it->second.latestByTag, &errors, &it->second.itemLastErrors);
    applyTriggerTimestamp(&actions, it->second.firstTriggerTsByItem, it->second.triggerGenerationByItem);
    actions.erase(std::remove_if(actions.begin(), actions.end(),
                                 [&it](const PublishAction &action) {
                                   return !it->second.firstTriggerTsByItem.contains(action.itemName);
                                 }),
                  actions.end());
  }

  for (const auto &error : errors) {
    LOG_WARNING("Calc 计算跳过: group_name={}, tag={}, 原因={}", groupName, update.dst_tag(), error);
  }
  for (const auto &action : actions) {
    auto status = dataCenter_.PublishValue(connId, action.tag, action.value, action.quality, action.tsMs);
    if (!status.ok()) {
      LOG_ERROR("Calc 发布结果失败: group_name={}, item_name={}, tag={}, 原因={}", groupName, action.itemName, action.tag, status.error_message());
    } else {
      LOG_DEBUG("Calc 已发布结果: group_name={}, item_name={}, tag={}, quality={}, ts_ms={}",
                groupName,
                action.itemName,
                action.tag,
                static_cast<int>(action.quality),
                action.tsMs);
      if (changeMode) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = groupsByName_.find(groupName);
        if (it != groupsByName_.end()) {
          auto generationIt = it->second.triggerGenerationByItem.find(action.itemName);
          if (generationIt != it->second.triggerGenerationByItem.end() &&
              generationIt->second == action.triggerGeneration) {
            it->second.triggerGenerationByItem.erase(generationIt);
            it->second.firstTriggerTsByItem.erase(action.itemName);
          }
        }
      }
    }
  }
}

void GroupManager::handlePeriodicCalculation(const std::string &groupName) {
  std::vector<PublishAction> actions;
  std::vector<std::string> errors;
  uint32_t connId = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end() || it->second.state != CalcProto::GROUP_STATE_RUNNING ||
        !isPeriodicMode(it->second.config)) {
      return;
    }
    connId = it->second.connId;
    actions = evaluateGroupLocked(it->second.config, it->second.latestByTag, &errors, &it->second.itemLastErrors);
    const auto calculationTs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
    for (auto &action : actions) {
      action.tsMs = calculationTs;
    }
  }

  for (const auto &error : errors) {
    LOG_WARNING("Calc 周期计算跳过: group_name={}, 原因={}", groupName, error);
  }
  for (const auto &action : actions) {
    auto status = dataCenter_.PublishValue(connId, action.tag, action.value, action.quality, action.tsMs);
    if (!status.ok()) {
      LOG_ERROR("Calc 周期发布结果失败: group_name={}, item_name={}, tag={}, 原因={}", groupName, action.itemName, action.tag, status.error_message());
    } else {
      LOG_DEBUG("Calc 周期已发布结果: group_name={}, item_name={}, tag={}, quality={}, ts_ms={}",
                groupName,
                action.itemName,
                action.tag,
                static_cast<int>(action.quality),
                action.tsMs);
    }
  }
}

std::unordered_set<std::string> GroupManager::collectAllTags(const CalcProto::CalcGroupConfig &config) {
  std::unordered_set<std::string> tags;
  for (const auto &item : config.items()) {
    const auto itemTags = makeItemTags(item);
    for (const auto &tag : itemTags.inputTags) {
      tags.emplace(tag);
    }
    tags.emplace(itemTags.resultTag);
  }
  return tags;
}

void GroupManager::rebuildTagCache(GroupRuntime *group) {
  if (group == nullptr) {
    return;
  }
  group->subscribeTags.clear();
  for (const auto &item : group->config.items()) {
    const auto itemTags = makeItemTags(item);
    if (isAggregateOperator(item.operator_kind())) {
      for (int index = 0; index < item.operands_size(); ++index) {
        if (item.operands(index).source_kind() == CalcProto::OPERAND_SOURCE_ROUTED_INPUT) {
          group->subscribeTags.emplace(itemTags.inputTags[static_cast<size_t>(index)]);
        }
      }
      continue;
    }
    if (item.left_operand().source_kind() == CalcProto::OPERAND_SOURCE_ROUTED_INPUT) {
      group->subscribeTags.emplace(itemTags.leftInputTag);
    }
    if (item.has_right_operand() && item.right_operand().source_kind() == CalcProto::OPERAND_SOURCE_ROUTED_INPUT) {
      group->subscribeTags.emplace(itemTags.rightInputTag);
    }
  }
}

}  // namespace Calc
