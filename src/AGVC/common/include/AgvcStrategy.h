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
  // Remaining amount that could not be allocated due to constraints.
  // >0: hit max constraints; <0: hit min constraints.
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

