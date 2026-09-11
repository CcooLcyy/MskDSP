#pragma once

#include <cstddef>
#include <expected>
#include <vector>

#include "mskdsp/Decimal20.hpp"

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

struct DecimalAllocationMember {
  mskdsp::numeric::Decimal20 weight;
  mskdsp::numeric::Decimal20 min;
  mskdsp::numeric::Decimal20 max;
};

struct DecimalAllocationOutput {
  std::vector<mskdsp::numeric::Decimal20> values;
  // 因约束无法分配的精确十进制剩余量。
  // >0 表示触达最大约束；<0 表示触达最小约束。
  mskdsp::numeric::Decimal20 unallocated;
};

class IStrategy {
public:
  virtual ~IStrategy() = default;
  virtual AllocationOutput Allocate(double total, const std::vector<AllocationMember>& members) const = 0;
};

class WeightedStrategy final : public IStrategy {
public:
  AllocationOutput Allocate(double total, const std::vector<AllocationMember>& members) const override;
  std::expected<DecimalAllocationOutput, mskdsp::numeric::DecimalError> AllocateDecimal(
      const mskdsp::numeric::Decimal20& total,
      const std::vector<DecimalAllocationMember>& members) const;
};

}  // namespace AGVC
