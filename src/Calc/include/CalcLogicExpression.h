#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Calc {

// 校验并计算逻辑表达式；失败时返回空值和中文错误，不执行隐式类型转换。
std::optional<bool> EvaluateLogicExpression(std::string_view expression,
                                          const std::vector<bool> &inputs,
                                          std::string *error);

}  // namespace Calc
