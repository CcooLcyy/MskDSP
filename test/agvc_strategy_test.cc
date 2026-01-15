#include <gtest/gtest.h>

#include "AgvcStrategy.h"

// Verify average distribution when weights are equal and no constraints bind.
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

// Verify proportional distribution matches capacity ratio (50:100 => 1:2).
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

// Verify redistribution when one member hits max constraint.
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

