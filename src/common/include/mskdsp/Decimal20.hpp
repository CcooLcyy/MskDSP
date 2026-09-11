#pragma once

#include <boost/multiprecision/cpp_int.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <expected>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace mskdsp::numeric {

enum class DecimalError {
  kInvalidFormat,
  kNonFinite,
  kOutOfRange,
  kDivisionByZero,
  kInvalidFractionalDigits,
  kInvalidBounds,
  kIntegerOverflow,
};

enum class RoundingMode {
  kHalfAwayFromZero,
  kTowardZero,
};

inline constexpr std::string_view DecimalErrorMessage(
    DecimalError error) noexcept {
  switch (error) {
    case DecimalError::kInvalidFormat:
      return "十进制文本格式非法";
    case DecimalError::kNonFinite:
      return "浮点边界值不是有限数";
    case DecimalError::kOutOfRange:
      return "数值超出 Decimal20 范围";
    case DecimalError::kDivisionByZero:
      return "十进制运算不允许除零";
    case DecimalError::kInvalidFractionalDigits:
      return "小数位数必须在 0 到 20 之间";
    case DecimalError::kInvalidBounds:
      return "数值下限不能大于上限";
    case DecimalError::kIntegerOverflow:
      return "数值超出目标整数类型范围";
  }
  return "未知十进制错误";
}

class Decimal20 {
 public:
  static constexpr std::uint32_t kFractionalDigits = 20;
  static constexpr std::uint32_t kMaximumIntegerDigits = 100;

  Decimal20() = default;

  static std::expected<Decimal20, DecimalError> Parse(
      std::string_view text);
  static std::expected<Decimal20, DecimalError> FromInt64(
      std::int64_t value);
  static std::expected<Decimal20, DecimalError> FromDouble(double value);
  static std::expected<Decimal20, DecimalError> FromFloat(float value);

  [[nodiscard]] std::expected<Decimal20, DecimalError> Add(
      const Decimal20 &other) const;
  [[nodiscard]] std::expected<Decimal20, DecimalError> Subtract(
      const Decimal20 &other) const;
  [[nodiscard]] std::expected<Decimal20, DecimalError> Multiply(
      const Decimal20 &other) const;
  [[nodiscard]] std::expected<Decimal20, DecimalError> Divide(
      const Decimal20 &other) const;
  [[nodiscard]] std::expected<Decimal20, DecimalError> AbsoluteDifference(
      const Decimal20 &other) const;
  [[nodiscard]] std::expected<Decimal20, DecimalError> Quantize(
      std::uint32_t fractionalDigits, RoundingMode mode) const;

  template <std::integral Integer>
  [[nodiscard]] std::expected<Integer, DecimalError> ToInteger(
      RoundingMode mode) const;

  [[nodiscard]] std::expected<double, DecimalError> ToDouble() const;
  [[nodiscard]] std::expected<float, DecimalError> ToFloat() const;
  [[nodiscard]] std::string ToFixedString() const;
  [[nodiscard]] std::string ToString() const;
  [[nodiscard]] bool IsZero() const noexcept;

  friend bool operator==(const Decimal20 &left,
                         const Decimal20 &right) noexcept {
    return left.units_ == right.units_;
  }

  friend bool operator<(const Decimal20 &left,
                        const Decimal20 &right) noexcept {
    return left.units_ < right.units_;
  }

  friend bool operator<=(const Decimal20 &left,
                         const Decimal20 &right) noexcept {
    return left.units_ <= right.units_;
  }

  friend bool operator>(const Decimal20 &left,
                        const Decimal20 &right) noexcept {
    return left.units_ > right.units_;
  }

  friend bool operator>=(const Decimal20 &left,
                         const Decimal20 &right) noexcept {
    return left.units_ >= right.units_;
  }

 private:
  using Integer = boost::multiprecision::cpp_int;

  static constexpr std::size_t kMaximumInputLength = 4096;
  static constexpr std::uint32_t kMaximumExponentMagnitude = 100000;

  explicit Decimal20(Integer units) : units_(std::move(units)) {}

  static Integer PowerOfTen(std::size_t exponent);
  static const Integer &Scale();
  static const Integer &RangeLimit();
  static Integer Absolute(Integer value);
  static Integer DivideRounded(Integer numerator, Integer denominator,
                               RoundingMode mode);
  static bool IsWithinRange(const Integer &units);
  static std::expected<Decimal20, DecimalError> FromUnitsChecked(
      Integer units);

  Integer units_{0};

  friend bool ShouldReport(const Decimal20 &current, const Decimal20 &last,
                           const Decimal20 &deadband);
};

inline Decimal20::Integer Decimal20::PowerOfTen(std::size_t exponent) {
  Integer result = 1;
  Integer factor = 10;
  while (exponent != 0) {
    if ((exponent & 1U) != 0U) {
      result *= factor;
    }
    exponent >>= 1U;
    if (exponent != 0) {
      factor *= factor;
    }
  }
  return result;
}

inline const Decimal20::Integer &Decimal20::Scale() {
  static const Integer scale = PowerOfTen(kFractionalDigits);
  return scale;
}

inline const Decimal20::Integer &Decimal20::RangeLimit() {
  static const Integer limit =
      PowerOfTen(kMaximumIntegerDigits + kFractionalDigits);
  return limit;
}

inline Decimal20::Integer Decimal20::Absolute(Integer value) {
  if (value < 0) {
    value = -value;
  }
  return value;
}

inline Decimal20::Integer Decimal20::DivideRounded(
    Integer numerator, Integer denominator, RoundingMode mode) {
  const bool negative = (numerator < 0) != (denominator < 0);
  numerator = Absolute(std::move(numerator));
  denominator = Absolute(std::move(denominator));

  Integer quotient = numerator / denominator;
  const Integer remainder = numerator % denominator;
  if (mode == RoundingMode::kHalfAwayFromZero &&
      remainder * 2 >= denominator) {
    ++quotient;
  }
  return negative ? -quotient : quotient;
}

inline bool Decimal20::IsWithinRange(const Integer &units) {
  return Absolute(units) < RangeLimit();
}

inline std::expected<Decimal20, DecimalError> Decimal20::FromUnitsChecked(
    Integer units) {
  if (!IsWithinRange(units)) {
    return std::unexpected(DecimalError::kOutOfRange);
  }
  return Decimal20(std::move(units));
}

inline std::expected<Decimal20, DecimalError> Decimal20::Parse(
    std::string_view text) {
  if (text.empty()) {
    return std::unexpected(DecimalError::kInvalidFormat);
  }
  if (text.size() > kMaximumInputLength) {
    return std::unexpected(DecimalError::kOutOfRange);
  }

  std::size_t position = 0;
  bool negative = false;
  if (text[position] == '+' || text[position] == '-') {
    negative = text[position] == '-';
    ++position;
    if (position == text.size()) {
      return std::unexpected(DecimalError::kInvalidFormat);
    }
  }

  std::string digits;
  digits.reserve(text.size());
  bool hasDigit = false;
  while (position < text.size() && text[position] >= '0' &&
         text[position] <= '9') {
    digits.push_back(text[position]);
    hasDigit = true;
    ++position;
  }

  std::size_t fractionalDigitCount = 0;
  if (position < text.size() && text[position] == '.') {
    ++position;
    while (position < text.size() && text[position] >= '0' &&
           text[position] <= '9') {
      digits.push_back(text[position]);
      hasDigit = true;
      ++fractionalDigitCount;
      ++position;
    }
  }
  if (!hasDigit) {
    return std::unexpected(DecimalError::kInvalidFormat);
  }

  std::int64_t exponent = 0;
  if (position < text.size() &&
      (text[position] == 'e' || text[position] == 'E')) {
    ++position;
    bool exponentNegative = false;
    if (position < text.size() &&
        (text[position] == '+' || text[position] == '-')) {
      exponentNegative = text[position] == '-';
      ++position;
    }
    if (position == text.size() || text[position] < '0' ||
        text[position] > '9') {
      return std::unexpected(DecimalError::kInvalidFormat);
    }

    std::uint32_t magnitude = 0;
    bool exponentOutOfRange = false;
    while (position < text.size() && text[position] >= '0' &&
           text[position] <= '9') {
      const auto digit = static_cast<std::uint32_t>(text[position] - '0');
      if (magnitude > (kMaximumExponentMagnitude - digit) / 10U) {
        exponentOutOfRange = true;
      } else if (!exponentOutOfRange) {
        magnitude = magnitude * 10U + digit;
      }
      ++position;
    }
    if (exponentOutOfRange || magnitude > kMaximumExponentMagnitude) {
      return std::unexpected(DecimalError::kOutOfRange);
    }
    exponent = exponentNegative ? -static_cast<std::int64_t>(magnitude)
                                : static_cast<std::int64_t>(magnitude);
  }

  if (position != text.size()) {
    return std::unexpected(DecimalError::kInvalidFormat);
  }

  const auto firstNonZero = digits.find_first_not_of('0');
  if (firstNonZero == std::string::npos) {
    return Decimal20{};
  }
  digits.erase(0, firstNonZero);

  const std::int64_t scaleShift =
      exponent - static_cast<std::int64_t>(fractionalDigitCount) +
      static_cast<std::int64_t>(kFractionalDigits);
  if (scaleShift >= 0) {
    const auto positiveShift = static_cast<std::size_t>(scaleShift);
    constexpr auto kMaximumUnitDigits =
        static_cast<std::size_t>(kMaximumIntegerDigits +
                                 kFractionalDigits);
    if (positiveShift > kMaximumUnitDigits ||
        digits.size() > kMaximumUnitDigits - positiveShift) {
      return std::unexpected(DecimalError::kOutOfRange);
    }
  }

  Integer significand = 0;
  for (const char character : digits) {
    significand *= 10;
    significand += static_cast<unsigned>(character - '0');
  }

  Integer units = 0;
  if (scaleShift >= 0) {
    units = significand * PowerOfTen(static_cast<std::size_t>(scaleShift));
  } else {
    const auto divisorDigits = static_cast<std::size_t>(-scaleShift);
    if (divisorDigits <= digits.size()) {
      units = DivideRounded(std::move(significand),
                            PowerOfTen(divisorDigits),
                            RoundingMode::kHalfAwayFromZero);
    }
  }
  if (negative) {
    units = -units;
  }
  return FromUnitsChecked(std::move(units));
}

inline std::expected<Decimal20, DecimalError> Decimal20::FromInt64(
    std::int64_t value) {
  return FromUnitsChecked(Integer(value) * Scale());
}

inline std::expected<Decimal20, DecimalError> Decimal20::FromDouble(
    double value) {
  if (!std::isfinite(value)) {
    return std::unexpected(DecimalError::kNonFinite);
  }
  std::array<char, 128> buffer{};
  const auto converted =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                    std::chars_format::general);
  if (converted.ec != std::errc{}) {
    return std::unexpected(DecimalError::kOutOfRange);
  }
  return Parse(std::string_view(
      buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())));
}

inline std::expected<Decimal20, DecimalError> Decimal20::FromFloat(
    float value) {
  if (!std::isfinite(value)) {
    return std::unexpected(DecimalError::kNonFinite);
  }
  std::array<char, 64> buffer{};
  const auto converted =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                    std::chars_format::general);
  if (converted.ec != std::errc{}) {
    return std::unexpected(DecimalError::kOutOfRange);
  }
  return Parse(std::string_view(
      buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())));
}

inline std::expected<Decimal20, DecimalError> Decimal20::Add(
    const Decimal20 &other) const {
  return FromUnitsChecked(units_ + other.units_);
}

inline std::expected<Decimal20, DecimalError> Decimal20::Subtract(
    const Decimal20 &other) const {
  return FromUnitsChecked(units_ - other.units_);
}

inline std::expected<Decimal20, DecimalError> Decimal20::Multiply(
    const Decimal20 &other) const {
  return FromUnitsChecked(DivideRounded(
      units_ * other.units_, Scale(), RoundingMode::kHalfAwayFromZero));
}

inline std::expected<Decimal20, DecimalError> Decimal20::Divide(
    const Decimal20 &other) const {
  if (other.units_ == 0) {
    return std::unexpected(DecimalError::kDivisionByZero);
  }
  return FromUnitsChecked(DivideRounded(
      units_ * Scale(), other.units_, RoundingMode::kHalfAwayFromZero));
}

inline std::expected<Decimal20, DecimalError>
Decimal20::AbsoluteDifference(const Decimal20 &other) const {
  return FromUnitsChecked(Absolute(units_ - other.units_));
}

inline std::expected<Decimal20, DecimalError> Decimal20::Quantize(
    std::uint32_t fractionalDigits, RoundingMode mode) const {
  if (fractionalDigits > kFractionalDigits) {
    return std::unexpected(DecimalError::kInvalidFractionalDigits);
  }
  const Integer factor = PowerOfTen(kFractionalDigits - fractionalDigits);
  Integer rounded = DivideRounded(units_, factor, mode);
  rounded *= factor;
  return FromUnitsChecked(std::move(rounded));
}

template <std::integral IntegerType>
inline std::expected<IntegerType, DecimalError> Decimal20::ToInteger(
    RoundingMode mode) const {
  static_assert(!std::is_same_v<std::remove_cv_t<IntegerType>, bool>,
                "Decimal20 不能转换为 bool 整数类型");
  const Integer rounded = DivideRounded(units_, Scale(), mode);
  const Integer minimum = std::numeric_limits<IntegerType>::min();
  const Integer maximum = std::numeric_limits<IntegerType>::max();
  if (rounded < minimum || rounded > maximum) {
    return std::unexpected(DecimalError::kIntegerOverflow);
  }
  return rounded.template convert_to<IntegerType>();
}

inline std::expected<double, DecimalError> Decimal20::ToDouble() const {
  const double converted = units_.convert_to<double>() / 1.0e20;
  if (!std::isfinite(converted)) {
    return std::unexpected(DecimalError::kOutOfRange);
  }
  return converted;
}

inline std::expected<float, DecimalError> Decimal20::ToFloat() const {
  auto converted = ToDouble();
  if (!converted.has_value() ||
      *converted > static_cast<double>(std::numeric_limits<float>::max()) ||
      *converted < -static_cast<double>(std::numeric_limits<float>::max())) {
    return std::unexpected(DecimalError::kOutOfRange);
  }
  const float result = static_cast<float>(*converted);
  if (!std::isfinite(result)) {
    return std::unexpected(DecimalError::kOutOfRange);
  }
  return result;
}

inline std::string Decimal20::ToFixedString() const {
  const bool negative = units_ < 0;
  const Integer magnitude = Absolute(units_);
  std::string digits = magnitude.convert_to<std::string>();

  std::string result;
  if (negative) {
    result.push_back('-');
  }
  if (digits.size() <= kFractionalDigits) {
    result += "0.";
    result.append(kFractionalDigits - digits.size(), '0');
    result += digits;
    return result;
  }

  const auto decimalPosition = digits.size() - kFractionalDigits;
  result.append(digits, 0, decimalPosition);
  result.push_back('.');
  result.append(digits, decimalPosition, std::string::npos);
  return result;
}

inline std::string Decimal20::ToString() const {
  std::string result = ToFixedString();
  while (!result.empty() && result.back() == '0') {
    result.pop_back();
  }
  if (!result.empty() && result.back() == '.') {
    result.pop_back();
  }
  return result;
}

inline bool Decimal20::IsZero() const noexcept { return units_ == 0; }

inline std::expected<Decimal20, DecimalError> ParseConfiguredDecimal(
    std::string_view decimalText, double legacyValue) {
  if (!decimalText.empty()) {
    return Decimal20::Parse(decimalText);
  }
  return Decimal20::FromDouble(legacyValue);
}

inline std::expected<Decimal20, DecimalError> ApplyEngineering(
    const Decimal20 &raw, const Decimal20 &scale, const Decimal20 &offset) {
  const auto effectiveScale = scale.IsZero() ? Decimal20::FromInt64(1)
                                              : std::expected<Decimal20, DecimalError>(scale);
  if (!effectiveScale.has_value()) {
    return std::unexpected(effectiveScale.error());
  }
  auto scaled = raw.Multiply(*effectiveScale);
  if (!scaled.has_value()) {
    return std::unexpected(scaled.error());
  }
  return scaled->Add(offset);
}

inline std::expected<Decimal20, DecimalError> ReverseEngineering(
    const Decimal20 &engineering, const Decimal20 &scale,
    const Decimal20 &offset) {
  auto shifted = engineering.Subtract(offset);
  if (!shifted.has_value()) {
    return std::unexpected(shifted.error());
  }
  const auto effectiveScale = scale.IsZero() ? Decimal20::FromInt64(1)
                                              : std::expected<Decimal20, DecimalError>(scale);
  if (!effectiveScale.has_value()) {
    return std::unexpected(effectiveScale.error());
  }
  return shifted->Divide(*effectiveScale);
}

inline bool ShouldReport(const Decimal20 &current, const Decimal20 &last,
                         const Decimal20 &deadband) {
  if (deadband.units_ <= 0) {
    return true;
  }
  Decimal20::Integer difference = current.units_ - last.units_;
  if (difference < 0) {
    difference = -difference;
  }
  return difference >= deadband.units_;
}

inline std::expected<Decimal20, DecimalError> Clamp(
    const Decimal20 &value, const Decimal20 &lower,
    const Decimal20 &upper) {
  if (lower > upper) {
    return std::unexpected(DecimalError::kInvalidBounds);
  }
  if (value < lower) {
    return lower;
  }
  if (value > upper) {
    return upper;
  }
  return value;
}

}  // 命名空间 mskdsp::numeric
