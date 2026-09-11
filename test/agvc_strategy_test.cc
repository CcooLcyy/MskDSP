#include <gtest/gtest.h>

#include "AgvcStrategy.h"

namespace {

mskdsp::numeric::Decimal20 Decimal(std::string_view text) {
  auto value = mskdsp::numeric::Decimal20::Parse(text);
  EXPECT_TRUE(value.has_value());
  return value.value_or(mskdsp::numeric::Decimal20{});
}

}  // 命名空间结束

// 验证：当权重相等且没有约束生效时，按平均值分配。
TEST(AgvcStrategyTest, WeightedStrategy_AverageNoConstraints) {
  AGVC::WeightedStrategy s;
  std::vector<AGVC::AllocationMember> members{
      {.weight = 1.0, .min = 0.0, .max = 100.0},
      {.weight = 1.0, .min = 0.0, .max = 100.0},
  };

  auto out = s.Allocate(30.0, members);
  ASSERT_EQ(out.values.size(), 2u);
  EXPECT_NEAR(out.values[0], 15.0, 1e-6);
  EXPECT_NEAR(out.values[1], 15.0, 1e-6);
  EXPECT_NEAR(out.unallocated, 0.0, 1e-6);
}

// 验证：按权重比例分配时应匹配容量比例（50:100 => 1:2）。
TEST(AgvcStrategyTest, WeightedStrategy_ProportionalNoConstraints) {
  AGVC::WeightedStrategy s;
  std::vector<AGVC::AllocationMember> members{
      {.weight = 50.0, .min = 0.0, .max = 100.0},
      {.weight = 100.0, .min = 0.0, .max = 100.0},
  };

  auto out = s.Allocate(30.0, members);
  ASSERT_EQ(out.values.size(), 2u);
  EXPECT_NEAR(out.values[0], 10.0, 1e-6);
  EXPECT_NEAR(out.values[1], 20.0, 1e-6);
  EXPECT_NEAR(out.unallocated, 0.0, 1e-6);
}

// 验证：当某个成员触发上限约束时，其余成员会重新分配。
TEST(AgvcStrategyTest, WeightedStrategy_MaxSaturationRedistributes) {
  AGVC::WeightedStrategy s;
  std::vector<AGVC::AllocationMember> members{
      {.weight = 1.0, .min = 0.0, .max = 10.0},
      {.weight = 1.0, .min = 0.0, .max = 100.0},
  };

  auto out = s.Allocate(30.0, members);
  ASSERT_EQ(out.values.size(), 2u);
  EXPECT_NEAR(out.values[0], 10.0, 1e-6);
  EXPECT_NEAR(out.values[1], 20.0, 1e-6);
  EXPECT_NEAR(out.unallocated, 0.0, 1e-6);
}

// 验证：Decimal20 加权分配在无限循环小数场景中保持成员合计严格等于总目标。
TEST(AgvcStrategyTest, DecimalWeightedStrategyPreservesTotalAtTwentyDigits) {
  AGVC::WeightedStrategy strategy;
  std::vector<AGVC::DecimalAllocationMember> members{
      {.weight = Decimal("1"), .min = Decimal("0"), .max = Decimal("1")},
      {.weight = Decimal("2"), .min = Decimal("0"), .max = Decimal("1")},
  };

  auto result = strategy.AllocateDecimal(Decimal("1"), members);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->values.size(), 2u);
  EXPECT_EQ(result->values[0].ToFixedString(), "0.33333333333333333333");
  EXPECT_EQ(result->values[1].ToFixedString(), "0.66666666666666666667");
  EXPECT_EQ(result->unallocated.ToFixedString(), "0.00000000000000000000");
}

// 验证：Decimal20 加权分配触达成员上限后，以精确十进制余量重新分配给其他成员。
TEST(AgvcStrategyTest, DecimalWeightedStrategyRedistributesAfterSaturation) {
  AGVC::WeightedStrategy strategy;
  std::vector<AGVC::DecimalAllocationMember> members{
      {.weight = Decimal("1"), .min = Decimal("0"), .max = Decimal("0.1")},
      {.weight = Decimal("1"), .min = Decimal("0"), .max = Decimal("1")},
  };

  auto result = strategy.AllocateDecimal(Decimal("0.3"), members);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->values.size(), 2u);
  EXPECT_EQ(result->values[0].ToFixedString(), "0.10000000000000000000");
  EXPECT_EQ(result->values[1].ToFixedString(), "0.20000000000000000000");
  EXPECT_TRUE(result->unallocated.IsZero());
}
