#include "AgvcStrategy.h"

#include <algorithm>
#include <cmath>

namespace AGVC {
namespace {
constexpr double kEps = 1e-9;
}  // namespace

AllocationOutput WeightedStrategy::Allocate(double total, const std::vector<AllocationMember>& members) const {
  AllocationOutput out;
  out.values.assign(members.size(), 0.0);
  out.unallocated = 0.0;

  if (members.empty()) {
    out.unallocated = total;
    return out;
  }

  // 从最小约束开始。
  double sumMin = 0.0;
  for (size_t i = 0; i < members.size(); ++i) {
    const auto& m = members[i];
    const auto lo = std::min(m.min, m.max);
    const auto hi = std::max(m.min, m.max);
    out.values[i] = lo;
    sumMin += lo;
  }

  double remaining = total - sumMin;
  if (remaining <= kEps) {
    // 总量小于最小值之和：不能再低于最小值。
    out.unallocated = remaining;
    return out;
  }

  std::vector<size_t> active;
  active.reserve(members.size());
  for (size_t i = 0; i < members.size(); ++i) {
    const auto& m = members[i];
    const auto hi = std::max(m.min, m.max);
    if (hi > out.values[i] + kEps && m.weight > 0.0) {
      active.emplace_back(i);
    }
  }

  while (remaining > kEps && !active.empty()) {
    const auto remainingRound = remaining;
    remaining = 0.0;

    double sumW = 0.0;
    for (auto idx : active) {
      sumW += members[idx].weight;
    }
    if (sumW <= kEps) {
      break;
    }

    bool anySaturated = false;
    std::vector<size_t> nextActive;
    nextActive.reserve(active.size());

    // 按权重分配剩余量；若有成员达到最大值，
    // 剔除该成员并在下一轮重新分配剩余量。
    for (auto idx : active) {
      const auto& m = members[idx];
      const auto hi = std::max(m.min, m.max);
      const auto cap = hi - out.values[idx];
      const auto share = remainingRound * (m.weight / sumW);

      if (share >= cap - kEps) {  // saturate
        out.values[idx] = hi;     // allocate 'cap'
        remaining += (share - cap);
        anySaturated = true;
        continue;
      }

      out.values[idx] += share;

      if (hi > out.values[idx] + kEps) {
        nextActive.emplace_back(idx);
      }
    }

    if (!anySaturated || remaining <= kEps) {
      break;
    }
    active.swap(nextActive);
  }

  out.unallocated = remaining;
  return out;
}

}  // namespace AGVC
