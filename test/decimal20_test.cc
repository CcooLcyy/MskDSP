#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "mskdsp/Decimal20.hpp"

namespace {

using mskdsp::numeric::ApplyEngineering;
using mskdsp::numeric::Clamp;
using mskdsp::numeric::Decimal20;
using mskdsp::numeric::DecimalError;
using mskdsp::numeric::ParseConfiguredDecimal;
using mskdsp::numeric::ReverseEngineering;
using mskdsp::numeric::RoundingMode;
using mskdsp::numeric::ShouldReport;

// 验证：十进制文本按小数点后 20 位保存，且精度不受整数位数和科学计数法影响。
TEST(Decimal20Test, ParsePreservesTwentyFractionalDigitsAndScientificNotation) {
  constexpr std::string_view kPrecise =
      "123456789012345678901234567890.12345678901234567890";
  auto precise = Decimal20::Parse(kPrecise);
  ASSERT_TRUE(precise.has_value());
  EXPECT_EQ(precise->ToFixedString(), kPrecise);
  EXPECT_EQ(precise->ToString(),
            "123456789012345678901234567890.1234567890123456789");

  auto scientific = Decimal20::Parse("+1.2345678901234567890e-1");
  ASSERT_TRUE(scientific.has_value());
  EXPECT_EQ(scientific->ToFixedString(), "0.12345678901234567890");
  EXPECT_EQ(scientific->ToString(), "0.1234567890123456789");

  auto negativeZero = Decimal20::Parse("-0.00000000000000000000");
  ASSERT_TRUE(negativeZero.has_value());
  EXPECT_EQ(negativeZero->ToFixedString(), "0.00000000000000000000");
  EXPECT_EQ(negativeZero->ToString(), "0");
}

// 验证：加减保持固定比例精确语义，乘除在每步完成后量化到小数点后 20 位。
TEST(Decimal20Test, ArithmeticQuantizesEveryResultToTwentyFractionalDigits) {
  auto oneTenth = Decimal20::Parse("0.1");
  auto twoTenths = Decimal20::Parse("0.2");
  auto threeTenths = Decimal20::Parse("0.3");
  auto halfUnitAtScale = Decimal20::Parse("0.00000000000000000005");
  auto one = Decimal20::Parse("1");
  auto three = Decimal20::Parse("3");
  auto zero = Decimal20::Parse("0");
  ASSERT_TRUE(oneTenth.has_value());
  ASSERT_TRUE(twoTenths.has_value());
  ASSERT_TRUE(threeTenths.has_value());
  ASSERT_TRUE(halfUnitAtScale.has_value());
  ASSERT_TRUE(one.has_value());
  ASSERT_TRUE(three.has_value());
  ASSERT_TRUE(zero.has_value());

  auto sum = oneTenth->Add(*twoTenths);
  ASSERT_TRUE(sum.has_value());
  EXPECT_EQ(sum->ToFixedString(), "0.30000000000000000000");

  auto difference = threeTenths->Subtract(*oneTenth);
  ASSERT_TRUE(difference.has_value());
  EXPECT_EQ(difference->ToFixedString(), "0.20000000000000000000");

  auto product = halfUnitAtScale->Multiply(*oneTenth);
  ASSERT_TRUE(product.has_value());
  EXPECT_EQ(product->ToFixedString(), "0.00000000000000000001");

  auto quotient = one->Divide(*three);
  ASSERT_TRUE(quotient.has_value());
  EXPECT_EQ(quotient->ToFixedString(), "0.33333333333333333333");

  auto divisionByZero = one->Divide(*zero);
  ASSERT_FALSE(divisionByZero.has_value());
  EXPECT_EQ(divisionByZero.error(), DecimalError::kDivisionByZero);
}

// 验证：解析和显式量化在正负半数边界均采用远离零舍入。
TEST(Decimal20Test, RoundsPositiveAndNegativeTiesAwayFromZero) {
  const std::string positiveTie = "1." + std::string(20, '0') + "5";
  const std::string negativeTie = "-1." + std::string(20, '0') + "5";

  auto positiveParsed = Decimal20::Parse(positiveTie);
  auto negativeParsed = Decimal20::Parse(negativeTie);
  ASSERT_TRUE(positiveParsed.has_value());
  ASSERT_TRUE(negativeParsed.has_value());
  EXPECT_EQ(positiveParsed->ToFixedString(), "1.00000000000000000001");
  EXPECT_EQ(negativeParsed->ToFixedString(), "-1.00000000000000000001");

  auto positive = Decimal20::Parse("1.5");
  auto negative = Decimal20::Parse("-1.5");
  ASSERT_TRUE(positive.has_value());
  ASSERT_TRUE(negative.has_value());

  auto positiveRounded = positive->Quantize(0, RoundingMode::kHalfAwayFromZero);
  auto negativeRounded = negative->Quantize(0, RoundingMode::kHalfAwayFromZero);
  ASSERT_TRUE(positiveRounded.has_value());
  ASSERT_TRUE(negativeRounded.has_value());
  EXPECT_EQ(positiveRounded->ToFixedString(), "2.00000000000000000000");
  EXPECT_EQ(negativeRounded->ToFixedString(), "-2.00000000000000000000");
}

// 验证：显式截断对正负数都向零处理，不复用默认舍入规则。
TEST(Decimal20Test, TruncatesPositiveAndNegativeValuesTowardZero) {
  auto positive = Decimal20::Parse("1.999");
  auto negative = Decimal20::Parse("-1.999");
  ASSERT_TRUE(positive.has_value());
  ASSERT_TRUE(negative.has_value());

  auto positiveTruncated = positive->Quantize(2, RoundingMode::kTowardZero);
  auto negativeTruncated = negative->Quantize(2, RoundingMode::kTowardZero);
  ASSERT_TRUE(positiveTruncated.has_value());
  ASSERT_TRUE(negativeTruncated.has_value());
  EXPECT_EQ(positiveTruncated->ToFixedString(), "1.99000000000000000000");
  EXPECT_EQ(negativeTruncated->ToFixedString(), "-1.99000000000000000000");
}

// 验证：非法文本、非有限浮点边界、超长整数和非法小数位参数返回明确错误。
TEST(Decimal20Test, RejectsInvalidNonFiniteAndOutOfRangeInputs) {
  for (const std::string_view input : {"", " 1", "1 ", "0x10", "1.2x", "NaN", "Inf", ".", "+"}) {
    auto parsed = Decimal20::Parse(input);
    ASSERT_FALSE(parsed.has_value()) << input;
    EXPECT_EQ(parsed.error(), DecimalError::kInvalidFormat) << input;
  }

  auto positiveInfinity = Decimal20::FromDouble(std::numeric_limits<double>::infinity());
  auto notANumber = Decimal20::FromDouble(std::numeric_limits<double>::quiet_NaN());
  ASSERT_FALSE(positiveInfinity.has_value());
  ASSERT_FALSE(notANumber.has_value());
  EXPECT_EQ(positiveInfinity.error(), DecimalError::kNonFinite);
  EXPECT_EQ(notANumber.error(), DecimalError::kNonFinite);

  auto oversizedInteger = Decimal20::Parse(std::string(101, '9'));
  ASSERT_FALSE(oversizedInteger.has_value());
  EXPECT_EQ(oversizedInteger.error(), DecimalError::kOutOfRange);

  auto value = Decimal20::Parse("1");
  ASSERT_TRUE(value.has_value());
  auto invalidPlaces = value->Quantize(21, RoundingMode::kHalfAwayFromZero);
  ASSERT_FALSE(invalidPlaces.has_value());
  EXPECT_EQ(invalidPlaces.error(), DecimalError::kInvalidFractionalDigits);
}

// 验证：配置中的十进制文本优先于旧 double，非法文本不能静默回退。
TEST(Decimal20Test, ConfiguredDecimalPrefersTextAndOnlyFallsBackWhenEmpty) {
  auto preferred = ParseConfiguredDecimal(
      "0.12345678901234567890", 0.5);
  ASSERT_TRUE(preferred.has_value());
  EXPECT_EQ(preferred->ToFixedString(), "0.12345678901234567890");

  auto fallback = ParseConfiguredDecimal("", 0.1);
  ASSERT_TRUE(fallback.has_value());
  EXPECT_EQ(fallback->ToFixedString(), "0.10000000000000000000");

  auto invalid = ParseConfiguredDecimal("1.2.3", 0.5);
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(invalid.error(), DecimalError::kInvalidFormat);
}

// 验证：工程量正反算都把零倍率视为一，普通除零语义不受兼容规则影响。
TEST(Decimal20Test, EngineeringConversionTreatsZeroScaleAsOne) {
  auto raw = Decimal20::Parse("0.1");
  auto engineering = Decimal20::Parse("0.3");
  auto zeroScale = Decimal20::Parse("0");
  auto offset = Decimal20::Parse("0.2");
  ASSERT_TRUE(raw.has_value());
  ASSERT_TRUE(engineering.has_value());
  ASSERT_TRUE(zeroScale.has_value());
  ASSERT_TRUE(offset.has_value());

  auto applied = ApplyEngineering(*raw, *zeroScale, *offset);
  ASSERT_TRUE(applied.has_value());
  EXPECT_EQ(applied->ToFixedString(), "0.30000000000000000000");

  auto reversed = ReverseEngineering(*engineering, *zeroScale, *offset);
  ASSERT_TRUE(reversed.has_value());
  EXPECT_EQ(reversed->ToFixedString(), "0.10000000000000000000");
}

// 验证：死区精确包含等于边界的变化，限幅使用十进制顺序并拒绝反向上下限。
TEST(Decimal20Test, DeadbandAndClampUseExactDecimalComparisons) {
  auto last = Decimal20::Parse("0.1");
  auto atBoundary = Decimal20::Parse("0.3");
  auto belowBoundary = Decimal20::Parse("0.29999999999999999999");
  auto deadband = Decimal20::Parse("0.2");
  auto zero = Decimal20::Parse("0");
  auto negativeDeadband = Decimal20::Parse("-0.2");
  auto valueBelowLower = Decimal20::Parse("-2");
  auto lower = Decimal20::Parse("-1.5");
  auto upper = Decimal20::Parse("1");
  ASSERT_TRUE(last.has_value());
  ASSERT_TRUE(atBoundary.has_value());
  ASSERT_TRUE(belowBoundary.has_value());
  ASSERT_TRUE(deadband.has_value());
  ASSERT_TRUE(zero.has_value());
  ASSERT_TRUE(negativeDeadband.has_value());
  ASSERT_TRUE(valueBelowLower.has_value());
  ASSERT_TRUE(lower.has_value());
  ASSERT_TRUE(upper.has_value());

  EXPECT_TRUE(ShouldReport(*atBoundary, *last, *deadband));
  EXPECT_FALSE(ShouldReport(*belowBoundary, *last, *deadband));
  EXPECT_TRUE(ShouldReport(*last, *last, *zero));
  EXPECT_TRUE(ShouldReport(*last, *last, *negativeDeadband));

  auto clamped = Clamp(*valueBelowLower, *lower, *upper);
  ASSERT_TRUE(clamped.has_value());
  EXPECT_EQ(clamped->ToFixedString(), "-1.50000000000000000000");

  auto invalidBounds = Clamp(*last, *upper, *lower);
  ASSERT_FALSE(invalidBounds.has_value());
  EXPECT_EQ(invalidBounds.error(), DecimalError::kInvalidBounds);
}

// 验证：协议整数转换沿用正负半数远离零规则，并在有符号和无符号边界拒绝溢出。
TEST(Decimal20Test, IntegerConversionRoundsAndChecksTargetBoundaries) {
  auto positiveTie = Decimal20::Parse("1.5");
  auto negativeTie = Decimal20::Parse("-1.5");
  auto int16Maximum = Decimal20::Parse("32767");
  auto int16Overflow = Decimal20::Parse("32767.5");
  auto int16Minimum = Decimal20::Parse("-32768");
  auto int16Underflow = Decimal20::Parse("-32768.5");
  auto uint16Maximum = Decimal20::Parse("65535");
  auto uint16Overflow = Decimal20::Parse("65536");
  auto uint16Negative = Decimal20::Parse("-1");
  ASSERT_TRUE(positiveTie.has_value());
  ASSERT_TRUE(negativeTie.has_value());
  ASSERT_TRUE(int16Maximum.has_value());
  ASSERT_TRUE(int16Overflow.has_value());
  ASSERT_TRUE(int16Minimum.has_value());
  ASSERT_TRUE(int16Underflow.has_value());
  ASSERT_TRUE(uint16Maximum.has_value());
  ASSERT_TRUE(uint16Overflow.has_value());
  ASSERT_TRUE(uint16Negative.has_value());

  auto positiveRounded = positiveTie->ToInteger<int16_t>(RoundingMode::kHalfAwayFromZero);
  auto negativeRounded = negativeTie->ToInteger<int16_t>(RoundingMode::kHalfAwayFromZero);
  auto signedMaximum = int16Maximum->ToInteger<int16_t>(RoundingMode::kHalfAwayFromZero);
  auto signedMinimum = int16Minimum->ToInteger<int16_t>(RoundingMode::kHalfAwayFromZero);
  auto unsignedMaximum = uint16Maximum->ToInteger<uint16_t>(RoundingMode::kHalfAwayFromZero);
  ASSERT_TRUE(positiveRounded.has_value());
  ASSERT_TRUE(negativeRounded.has_value());
  ASSERT_TRUE(signedMaximum.has_value());
  ASSERT_TRUE(signedMinimum.has_value());
  ASSERT_TRUE(unsignedMaximum.has_value());
  EXPECT_EQ(*positiveRounded, 2);
  EXPECT_EQ(*negativeRounded, -2);
  EXPECT_EQ(*signedMaximum, std::numeric_limits<int16_t>::max());
  EXPECT_EQ(*signedMinimum, std::numeric_limits<int16_t>::min());
  EXPECT_EQ(*unsignedMaximum, std::numeric_limits<uint16_t>::max());

  auto signedOverflow = int16Overflow->ToInteger<int16_t>(RoundingMode::kHalfAwayFromZero);
  auto signedUnderflow = int16Underflow->ToInteger<int16_t>(RoundingMode::kHalfAwayFromZero);
  auto unsignedOverflow = uint16Overflow->ToInteger<uint16_t>(RoundingMode::kHalfAwayFromZero);
  auto unsignedUnderflow = uint16Negative->ToInteger<uint16_t>(RoundingMode::kHalfAwayFromZero);
  ASSERT_FALSE(signedOverflow.has_value());
  ASSERT_FALSE(signedUnderflow.has_value());
  ASSERT_FALSE(unsignedOverflow.has_value());
  ASSERT_FALSE(unsignedUnderflow.has_value());
  EXPECT_EQ(signedOverflow.error(), DecimalError::kIntegerOverflow);
  EXPECT_EQ(signedUnderflow.error(), DecimalError::kIntegerOverflow);
  EXPECT_EQ(unsignedOverflow.error(), DecimalError::kIntegerOverflow);
  EXPECT_EQ(unsignedUnderflow.error(), DecimalError::kIntegerOverflow);
}

}  // 命名空间结束
