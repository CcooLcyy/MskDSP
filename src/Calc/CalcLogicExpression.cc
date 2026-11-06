#include "CalcLogicExpression.h"

#include <cctype>
#include <stdexcept>

namespace Calc {
namespace {

class LogicParser {
public:
  LogicParser(std::string_view text, const std::vector<bool> &inputs)
      : text_(text), inputs_(inputs), used_(inputs.size(), false) {}

  bool Parse() {
    if (text_.empty() || text_.size() > 4096 || inputs_.empty() || inputs_.size() > 256) {
      Fail("表达式需为1至4096字节，输入需为1至256个");
    }
    Next();
    bool result = Or();
    if (!token_.empty()) Fail("表达式存在多余内容或缺少运算符");
    for (bool used : used_) if (!used) Fail("表达式必须引用每个已配置输入");
    return result;
  }

private:
  [[noreturn]] void Fail(const char *message) const { throw std::runtime_error(message); }

  void Next() {
    while (position_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[position_]))) ++position_;
    token_.clear();
    if (position_ == text_.size()) return;
    char ch = text_[position_++];
    if (std::isalpha(static_cast<unsigned char>(ch))) {
      token_ += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      while (position_ < text_.size() && std::isalpha(static_cast<unsigned char>(text_[position_]))) {
        token_ += static_cast<char>(std::toupper(static_cast<unsigned char>(text_[position_++])));
      }
    } else if (ch == '&' || ch == '|') {
      if (position_ == text_.size() || text_[position_++] != ch) Fail("与、或符号必须为 && 或 ||");
      token_ = ch == '&' ? "AND" : "OR";
    } else if (ch == '!') token_ = "NOT";
    else if (ch == '^') token_ = "XOR";
    else if (ch == '(' || ch == ')') token_ += ch;
    else Fail("表达式包含不支持的字符");
  }

  bool Or() {
    bool result = Xor();
    while (token_ == "OR") { Next(); bool right = Xor(); result = result || right; }
    return result;
  }
  bool Xor() {
    bool result = And();
    while (token_ == "XOR") { Next(); bool right = And(); result = result != right; }
    return result;
  }
  bool And() {
    bool result = Unary();
    while (token_ == "AND") { Next(); bool right = Unary(); result = result && right; }
    return result;
  }
  bool Unary() {
    if (token_ == "NOT" || token_ == "(") {
      if (++depth_ > 64) Fail("表达式嵌套不能超过64层");
      bool negate = token_ == "NOT";
      Next();
      bool result = negate ? Unary() : Or();
      if (!negate) { if (token_ != ")") Fail("表达式缺少右括号"); Next(); }
      --depth_;
      return negate ? !result : result;
    }
    if (token_.empty() || token_ == ")" || token_ == "AND" || token_ == "OR" || token_ == "XOR") {
      Fail("表达式缺少输入或非运算的操作数");
    }
    size_t index = 0;
    for (char ch : token_) {
      index = index * 26 + static_cast<size_t>(ch - 'A' + 1);
      if (index > inputs_.size()) Fail("表达式引用了未配置的输入别名");
    }
    --index;
    used_[index] = true;
    bool result = inputs_[index];
    Next();
    return result;
  }

  std::string_view text_;
  const std::vector<bool> &inputs_;
  std::vector<bool> used_;
  std::string token_;
  size_t position_{0};
  int depth_{0};
};

}  // namespace

std::optional<bool> EvaluateLogicExpression(std::string_view expression,
                                          const std::vector<bool> &inputs,
                                          std::string *error) {
  if (error != nullptr) error->clear();
  try { return LogicParser(expression, inputs).Parse(); }
  catch (const std::runtime_error &failure) {
    if (error != nullptr) *error = failure.what();
    return std::nullopt;
  }
}

}  // namespace Calc
