#pragma once

#include <cstddef>
#include <vector>

namespace AGVC {

struct AllocationMember {
  double weight{1.0};
  double min{0.0};
  double max{0.0};
};

struct AllocationOutput {
  std::vector<double> values;
  // 因约束无法分配的剩余量。
  // >0 表示触达最大约束；<0 表示触达最小约束。
  double unallocated{0.0};
};

class IStrategy {
public:
  virtual ~IStrategy() = default;
  virtual AllocationOutput Allocate(double total, const std::vector<AllocationMember>& members) const = 0;
};

class WeightedStrategy final : public IStrategy {
public:
  AllocationOutput Allocate(double total, const std::vector<AllocationMember>& members) const override;
};

}  // namespace AGVC
