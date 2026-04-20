#include "CalcValidation.h"

#include <format>
#include <string>
#include <string_view>
#include <unordered_set>

namespace Calc {
namespace {

grpc::Status makeInvalid(std::string message) {
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::move(message));
}

bool isNumericConstantKind(const CalcProto::TypedConstant &constant) {
  return constant.has_int_value() || constant.has_double_value();
}

grpc::Status validateOperand(const CalcProto::OperandSpec &operand, CalcProto::OperatorKind operatorKind, std::string_view fieldName) {
  if (operand.source_kind() == CalcProto::OPERAND_SOURCE_UNSPECIFIED) {
    return makeInvalid(std::format("{} source_kind 不能为空", fieldName));
  }
  if (operand.source_kind() == CalcProto::OPERAND_SOURCE_ROUTED_INPUT) {
    if (operand.has_constant()) {
      return makeInvalid(std::format("{} 为 ROUTED_INPUT 时不能携带 constant", fieldName));
    }
    return grpc::Status::OK;
  }
  if (operand.source_kind() != CalcProto::OPERAND_SOURCE_CONSTANT) {
    return makeInvalid(std::format("{} source_kind 非法", fieldName));
  }
  if (!operand.has_constant() || operand.constant().kind_case() == CalcProto::TypedConstant::KIND_NOT_SET) {
    return makeInvalid(std::format("{} 为 CONSTANT 时必须提供 constant", fieldName));
  }

  switch (operatorKind) {
  case CalcProto::OPERATOR_KIND_ADD:
  case CalcProto::OPERATOR_KIND_SUB:
  case CalcProto::OPERATOR_KIND_MUL:
  case CalcProto::OPERATOR_KIND_DIV:
    if (!isNumericConstantKind(operand.constant())) {
      return makeInvalid(std::format("{} 的 constant 必须为 int/double", fieldName));
    }
    return grpc::Status::OK;
  case CalcProto::OPERATOR_KIND_NOT:
  case CalcProto::OPERATOR_KIND_AND:
  case CalcProto::OPERATOR_KIND_OR:
  case CalcProto::OPERATOR_KIND_XOR:
    if (!operand.constant().has_bool_value()) {
      return makeInvalid(std::format("{} 的 constant 必须为 bool", fieldName));
    }
    return grpc::Status::OK;
  case CalcProto::OPERATOR_KIND_UNSPECIFIED:
  default:
    return makeInvalid("operator_kind 非法");
  }
}

}  // namespace

grpc::Status ValidateGroupConfig(const CalcProto::CalcGroupConfig &config) {
  if (config.group_name().empty()) {
    return makeInvalid("group_name 不能为空");
  }
  if (config.items_size() <= 0) {
    return makeInvalid("items 不能为空");
  }

  std::unordered_set<std::string> itemNames;
  itemNames.reserve(static_cast<size_t>(config.items_size()));
  for (const auto &item : config.items()) {
    if (item.item_name().empty()) {
      return makeInvalid("item_name 不能为空");
    }
    if (!itemNames.emplace(item.item_name()).second) {
      return makeInvalid(std::format("item_name 重复: {}", item.item_name()));
    }
    if (!item.has_left_operand()) {
      return makeInvalid(std::format("items[{}].left_operand 不能为空", item.item_name()));
    }

    switch (item.operator_kind()) {
    case CalcProto::OPERATOR_KIND_ADD:
    case CalcProto::OPERATOR_KIND_SUB:
    case CalcProto::OPERATOR_KIND_MUL:
    case CalcProto::OPERATOR_KIND_DIV:
    case CalcProto::OPERATOR_KIND_AND:
    case CalcProto::OPERATOR_KIND_OR:
    case CalcProto::OPERATOR_KIND_XOR:
      if (!item.has_right_operand()) {
        return makeInvalid(std::format("items[{}].right_operand 不能为空（二元运算）", item.item_name()));
      }
      break;
    case CalcProto::OPERATOR_KIND_NOT:
      if (item.has_right_operand()) {
        return makeInvalid(std::format("items[{}].right_operand 不允许设置（NOT 为单目运算）", item.item_name()));
      }
      break;
    case CalcProto::OPERATOR_KIND_UNSPECIFIED:
    default:
      return makeInvalid(std::format("items[{}].operator_kind 非法", item.item_name()));
    }

    auto status = validateOperand(item.left_operand(), item.operator_kind(), std::format("items[{}].left_operand", item.item_name()));
    if (!status.ok()) {
      return status;
    }
    if (item.has_right_operand()) {
      status = validateOperand(item.right_operand(), item.operator_kind(), std::format("items[{}].right_operand", item.item_name()));
      if (!status.ok()) {
        return status;
      }
    }

    const bool leftRouted = item.left_operand().source_kind() == CalcProto::OPERAND_SOURCE_ROUTED_INPUT;
    const bool rightRouted = item.has_right_operand() &&
        item.right_operand().source_kind() == CalcProto::OPERAND_SOURCE_ROUTED_INPUT;
    if (!leftRouted && !rightRouted) {
      return makeInvalid(std::format("items[{}] 至少一侧必须为 ROUTED_INPUT", item.item_name()));
    }
  }
  return grpc::Status::OK;
}

grpc::Status ValidateGroupsConfig(const CalcProto::GroupsConfig &config) {
  std::unordered_set<std::string> groupNames;
  groupNames.reserve(static_cast<size_t>(config.persisted_groups_size()));
  for (const auto &persisted : config.persisted_groups()) {
    if (!persisted.has_config()) {
      return makeInvalid("persisted_groups.config 不能为空");
    }
    auto status = ValidateGroupConfig(persisted.config());
    if (!status.ok()) {
      return status;
    }
    if (!groupNames.emplace(persisted.config().group_name()).second) {
      return makeInvalid(std::format("persisted_groups 包含重复的 group_name: {}", persisted.config().group_name()));
    }
  }
  return grpc::Status::OK;
}

}  // namespace Calc
