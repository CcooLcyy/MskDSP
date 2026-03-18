#include <gtest/gtest.h>

#include "AgvcStrategy.h"

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
