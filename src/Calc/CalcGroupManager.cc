#include "CalcGroupManager.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "CalcLibInfo.h"
#include "CalcValidation.h"
#include "Logger.h"
#include "ThreadUtil.hpp"

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
  };

  Type type{Type::kInt};
  bool boolValue{false};
  int64_t intValue{0};
  double doubleValue{0.0};
  DataCenterProto::Quality quality{DataCenterProto::QUALITY_GOOD};
  int64_t tsMs{0};
};

struct PublishAction {
  std::string itemName;
  std::string tag;
  DataCenterProto::PointValue value;
  DataCenterProto::Quality quality{DataCenterProto::QUALITY_GOOD};
  int64_t tsMs{0};
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

RuntimeValue makeValueFromConstant(const CalcProto::TypedConstant &constant) {
  RuntimeValue value;
  value.quality = DataCenterProto::QUALITY_GOOD;
  value.tsMs = 0;
  if (constant.has_bool_value()) {
    value.type = RuntimeValue::Type::kBool;
    value.boolValue = constant.bool_value();
    return value;
  }
  if (constant.has_double_value()) {
    value.type = RuntimeValue::Type::kDouble;
    value.doubleValue = constant.double_value();
    return value;
  }
  value.type = RuntimeValue::Type::kInt;
  value.intValue = constant.int_value();
  return value;
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
    value.type = RuntimeValue::Type::kInt;
    value.intValue = update.value().int_value();
    *out = value;
    return true;
  }
  if (update.value().has_double_value()) {
    value.type = RuntimeValue::Type::kDouble;
    value.doubleValue = update.value().double_value();
    *out = value;
    return true;
  }
  if (error != nullptr) {
    *error = std::format("tag={} 类型不支持，当前仅支持 bool/int64/double", update.dst_tag());
  }
  return false;
}

void setPointValueFromRuntimeValue(const RuntimeValue &value, DataCenterProto::PointValue *out) {
  out->Clear();
  switch (value.type) {
  case RuntimeValue::Type::kBool:
    out->set_bool_value(value.boolValue);
    return;
  case RuntimeValue::Type::kInt:
    out->set_int_value(value.intValue);
    return;
  case RuntimeValue::Type::kDouble:
    out->set_double_value(value.doubleValue);
    return;
  }
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
    *out = makeValueFromConstant(operand.constant());
    return true;
  }

  auto it = latestByTag.find(tag);
  if (it == latestByTag.end()) {
    *missing = true;
    return true;
  }
  return makeValueFromPointUpdate(it->second, out, error);
}

bool isNumericValue(const RuntimeValue &value) {
  return value.type == RuntimeValue::Type::kInt || value.type == RuntimeValue::Type::kDouble;
}

double toDouble(const RuntimeValue &value) {
  return value.type == RuntimeValue::Type::kDouble ? value.doubleValue : static_cast<double>(value.intValue);
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
          *error = std::format("item_name={} SUM/AVERAGE 仅接受 int64/double", item.item_name());
        }
        return std::nullopt;
      }
      action.quality = combineQuality(action.quality, value.quality);
      action.tsMs = std::max(action.tsMs, value.tsMs);
    }

    if (item.operator_kind() == CalcProto::OPERATOR_KIND_AVERAGE) {
      double sum = 0.0;
      for (const auto &value : values) {
        sum += toDouble(value);
      }
      double average = sum / static_cast<double>(values.size());
      if (item.has_decimal_places()) {
        const double scale = std::pow(10.0, static_cast<double>(item.decimal_places()));
        if (std::isfinite(scale) && scale > 0.0) {
          average = std::round(average * scale) / scale;
        }
      }
      action.value.set_double_value(average);
      return action;
    }

    bool allInt = true;
    int64_t intResult = 0;
    bool overflow = false;
    double doubleResult = 0.0;
    for (const auto &value : values) {
      allInt = allInt && value.type == RuntimeValue::Type::kInt;
      if (allInt && !overflow) {
        int64_t next = 0;
        overflow = __builtin_add_overflow(intResult, value.intValue, &next);
        if (!overflow) {
          intResult = next;
        }
      }
      doubleResult += toDouble(value);
    }
    if (allInt && !overflow) {
      action.value.set_int_value(intResult);
    } else {
      action.value.set_double_value(doubleResult);
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
        *error = std::format("item_name={} 数值运算仅接受 int64/double", item.item_name());
      }
      return std::nullopt;
    }

    action.quality = combineQuality(left.quality, right.quality);
    action.tsMs = std::max(left.tsMs, right.tsMs);

    const bool useDouble = item.operator_kind() == CalcProto::OPERATOR_KIND_DIV ||
        left.type == RuntimeValue::Type::kDouble ||
        right.type == RuntimeValue::Type::kDouble;
    if (item.operator_kind() == CalcProto::OPERATOR_KIND_DIV) {
      const double rhs = toDouble(right);
      if (rhs == 0.0) {
        if (error != nullptr) {
          *error = std::format("item_name={} 除零，跳过本轮结果发布", item.item_name());
        }
        return std::nullopt;
      }
      const double lhs = toDouble(left);
      action.value.set_double_value(lhs / rhs);
      return action;
    }

    if (useDouble) {
      const double lhs = toDouble(left);
      const double rhs = toDouble(right);
      switch (item.operator_kind()) {
      case CalcProto::OPERATOR_KIND_ADD:
        action.value.set_double_value(lhs + rhs);
        return action;
      case CalcProto::OPERATOR_KIND_SUB:
        action.value.set_double_value(lhs - rhs);
        return action;
      case CalcProto::OPERATOR_KIND_MUL:
        action.value.set_double_value(lhs * rhs);
        return action;
      default:
        break;
      }
    }

    int64_t result = 0;
    bool overflow = false;
    switch (item.operator_kind()) {
    case CalcProto::OPERATOR_KIND_ADD:
      overflow = __builtin_add_overflow(left.intValue, right.intValue, &result);
      break;
    case CalcProto::OPERATOR_KIND_SUB:
      overflow = __builtin_sub_overflow(left.intValue, right.intValue, &result);
      break;
    case CalcProto::OPERATOR_KIND_MUL:
      overflow = __builtin_mul_overflow(left.intValue, right.intValue, &result);
      break;
    default:
      break;
    }
    if (overflow) {
      const double lhs = static_cast<double>(left.intValue);
      const double rhs = static_cast<double>(right.intValue);
      switch (item.operator_kind()) {
      case CalcProto::OPERATOR_KIND_ADD:
        action.value.set_double_value(lhs + rhs);
        return action;
      case CalcProto::OPERATOR_KIND_SUB:
        action.value.set_double_value(lhs - rhs);
        return action;
      case CalcProto::OPERATOR_KIND_MUL:
        action.value.set_double_value(lhs * rhs);
        return action;
      default:
        break;
      }
    }
    action.value.set_int_value(result);
    return action;
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
        it->second.value().has_double_value();
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
    return;
  }

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
          it->second.state = CalcProto::GROUP_STATE_STOPPED;
          it->second.lastError = finishStatus.ok() ? "DataCenter 订阅流已结束"
                                                   : std::format("DataCenter 订阅失败: {}", finishStatus.error_message());
          LOG_WARNING("Calc 分组订阅线程退出: group_name={}, 原因={}", groupName, it->second.lastError);
        }
      });
  LOG_INFO("Calc 分组已启用 DataCenter 订阅: group_name={}, conn_id={}, 订阅标签数={}", groupName, connId, subscribeTags.size());
}

grpc::Status GroupManager::StartGroup(const std::string &groupName) {
  auto status = validateGroupName(groupName);
  if (!status.ok()) {
    return status;
  }

  std::vector<PublishAction> startupActions;
  std::vector<std::string> startupErrors;
  uint32_t startupConnId = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      return makeNotFound(groupName);
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
    it->second.itemLastErrors.clear();
    it->second.lastError.clear();
    it->second.state = CalcProto::GROUP_STATE_RUNNING;
    startThreadsLocked(groupName, &it->second);
    if (it->second.state != CalcProto::GROUP_STATE_RUNNING ||
        (!it->second.subscribeTags.empty() && !it->second.dcSubscribeThread.joinable())) {
      if (it->second.lastError.empty()) {
        it->second.lastError = "建立 DataCenter 订阅失败";
      }
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, it->second.lastError);
    }
    startupConnId = it->second.connId;
    startupActions = evaluateGroupLocked(it->second.config, it->second.latestByTag, &startupErrors, &it->second.itemLastErrors);
  }

  for (const auto &error : startupErrors) {
    LOG_WARNING("Calc 启动时计算跳过: group_name={}, 原因={}", groupName, error);
  }
  for (const auto &action : startupActions) {
    auto publishStatus = dataCenter_.PublishValue(startupConnId, action.tag, action.value, action.quality, action.tsMs);
    if (!publishStatus.ok()) {
      LOG_ERROR("Calc 启动时发布结果失败: group_name={}, item_name={}, tag={}, 原因={}", groupName, action.itemName, action.tag, publishStatus.error_message());
      std::lock_guard<std::mutex> lock(mu_);
      auto it = groupsByName_.find(groupName);
      if (it != groupsByName_.end()) {
        it->second.lastError = std::format("发布结果失败: {}", publishStatus.error_message());
      }
    } else {
      LOG_DEBUG("Calc 启动时已发布结果: group_name={}, item_name={}, tag={}", groupName, action.itemName, action.tag);
    }
  }

  LOG_INFO("Calc 分组已启动运算功能: group_name={}", groupName);
  return grpc::Status::OK;
}

void GroupManager::stopThreadsLocked(GroupRuntime *group, bool keepPendingDeleteState, std::jthread *outThread) {
  if (group == nullptr) {
    return;
  }
  if (group->dcSubscribeContext != nullptr) {
    group->dcSubscribeContext->TryCancel();
  }
  if (outThread != nullptr) {
    *outThread = std::move(group->dcSubscribeThread);
  }
  group->dcSubscribeContext.reset();
  if (!keepPendingDeleteState || group->state != CalcProto::GROUP_STATE_PENDING_DELETE) {
    group->state = CalcProto::GROUP_STATE_STOPPED;
  }
}

grpc::Status GroupManager::StopGroup(const std::string &groupName) {
  auto status = validateGroupName(groupName);
  if (!status.ok()) {
    return status;
  }

  std::jthread thread;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      return makeNotFound(groupName);
    }
    if (it->second.state == CalcProto::GROUP_STATE_STOPPED) {
      LOG_INFO("Calc 停止分组请求幂等成功: group_name={}, 原因=分组已停止", groupName);
      return grpc::Status::OK;
    }
    stopThreadsLocked(&it->second, true, &thread);
  }

  LOG_INFO("Calc 分组已停止运算功能: group_name={}", groupName);
  return grpc::Status::OK;
}

grpc::Status GroupManager::DeleteGroup(const std::string &groupName) {
  auto status = validateGroupName(groupName);
  if (!status.ok()) {
    return status;
  }

  std::jthread thread;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end()) {
      return makeNotFound(groupName);
    }
    stopThreadsLocked(&it->second, false, &thread);
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
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = groupsByName_.find(groupName);
    if (it == groupsByName_.end() || it->second.state != CalcProto::GROUP_STATE_RUNNING) {
      return;
    }
    it->second.latestByTag[update.dst_tag()] = update;
    connId = it->second.connId;
    actions = evaluateGroupLocked(it->second.config, it->second.latestByTag, &errors, &it->second.itemLastErrors);
  }

  for (const auto &error : errors) {
    LOG_WARNING("Calc 计算跳过: group_name={}, tag={}, 原因={}", groupName, update.dst_tag(), error);
  }
  for (const auto &action : actions) {
    auto status = dataCenter_.PublishValue(connId, action.tag, action.value, action.quality, action.tsMs);
    if (!status.ok()) {
      LOG_ERROR("Calc 发布结果失败: group_name={}, item_name={}, tag={}, 原因={}", groupName, action.itemName, action.tag, status.error_message());
    } else {
      LOG_DEBUG("Calc 已发布结果: group_name={}, item_name={}, tag={}", groupName, action.itemName, action.tag);
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
