#include "AgvcStrategy.h"

#include <algorithm>
#include <cmath>

namespace AGVC {
namespace {
constexpr double kEps = 1e-9;

using Decimal = mskdsp::numeric::Decimal20;
using DecimalError = mskdsp::numeric::DecimalError;

const Decimal& lowerBound(const DecimalAllocationMember& member) {
  return member.min <= member.max ? member.min : member.max;
}

const Decimal& upperBound(const DecimalAllocationMember& member) {
  return member.min <= member.max ? member.max : member.min;
}
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

std::expected<DecimalAllocationOutput, DecimalError> WeightedStrategy::AllocateDecimal(
    const Decimal& total,
    const std::vector<DecimalAllocationMember>& members) const {
  DecimalAllocationOutput out;
  out.values.assign(members.size(), Decimal{});
  if (members.empty()) {
    out.unallocated = total;
    return out;
  }

  Decimal sumMin;
  for (size_t index = 0; index < members.size(); ++index) {
    out.values[index] = lowerBound(members[index]);
    auto nextSum = sumMin.Add(out.values[index]);
    if (!nextSum.has_value()) {
      return std::unexpected(nextSum.error());
    }
    sumMin = *nextSum;
  }

  auto remainingResult = total.Subtract(sumMin);
  if (!remainingResult.has_value()) {
    return std::unexpected(remainingResult.error());
  }
  auto remaining = *remainingResult;
  if (remaining <= Decimal{}) {
    out.unallocated = remaining;
    return out;
  }

  std::vector<size_t> active;
  active.reserve(members.size());
  for (size_t index = 0; index < members.size(); ++index) {
    if (upperBound(members[index]) > out.values[index] &&
        members[index].weight > Decimal{}) {
      active.emplace_back(index);
    }
  }

  while (remaining > Decimal{} && !active.empty()) {
    Decimal totalWeight;
    for (const auto index : active) {
      auto nextWeight = totalWeight.Add(members[index].weight);
      if (!nextWeight.has_value()) {
        return std::unexpected(nextWeight.error());
      }
      totalWeight = *nextWeight;
    }
    if (totalWeight <= Decimal{}) {
      break;
    }

    std::vector<Decimal> shares;
    shares.reserve(active.size());
    std::vector<bool> saturated(active.size(), false);
    bool hasSaturation = false;
    for (size_t activeIndex = 0; activeIndex < active.size(); ++activeIndex) {
      const auto memberIndex = active[activeIndex];
      auto weighted = remaining.Multiply(members[memberIndex].weight);
      if (!weighted.has_value()) {
        return std::unexpected(weighted.error());
      }
      auto share = weighted->Divide(totalWeight);
      if (!share.has_value()) {
        return std::unexpected(share.error());
      }
      auto capacity = upperBound(members[memberIndex]).Subtract(out.values[memberIndex]);
      if (!capacity.has_value()) {
        return std::unexpected(capacity.error());
      }
      saturated[activeIndex] = *share >= *capacity;
      hasSaturation = hasSaturation || saturated[activeIndex];
      shares.emplace_back(std::move(*share));
    }

    if (hasSaturation) {
      std::vector<size_t> nextActive;
      nextActive.reserve(active.size());
      for (size_t activeIndex = 0; activeIndex < active.size(); ++activeIndex) {
        const auto memberIndex = active[activeIndex];
        if (!saturated[activeIndex]) {
          nextActive.emplace_back(memberIndex);
          continue;
        }
        auto capacity = upperBound(members[memberIndex]).Subtract(out.values[memberIndex]);
        if (!capacity.has_value()) {
          return std::unexpected(capacity.error());
        }
        if (*capacity > remaining) {
          nextActive.emplace_back(memberIndex);
          continue;
        }
        out.values[memberIndex] = upperBound(members[memberIndex]);
        auto nextRemaining = remaining.Subtract(*capacity);
        if (!nextRemaining.has_value()) {
          return std::unexpected(nextRemaining.error());
        }
        remaining = *nextRemaining;
      }
      active.swap(nextActive);
      continue;
    }

    for (size_t activeIndex = 0; activeIndex < active.size(); ++activeIndex) {
      const auto memberIndex = active[activeIndex];
      auto share = activeIndex + 1 == active.size() ? remaining : shares[activeIndex];
      if (share > remaining) {
        share = remaining;
      }
      auto capacity = upperBound(members[memberIndex]).Subtract(out.values[memberIndex]);
      if (!capacity.has_value()) {
        return std::unexpected(capacity.error());
      }
      if (share > *capacity) {
        share = *capacity;
      }
      auto nextValue = out.values[memberIndex].Add(share);
      if (!nextValue.has_value()) {
        return std::unexpected(nextValue.error());
      }
      out.values[memberIndex] = *nextValue;
      auto nextRemaining = remaining.Subtract(share);
      if (!nextRemaining.has_value()) {
        return std::unexpected(nextRemaining.error());
      }
      remaining = *nextRemaining;
    }

    if (remaining > Decimal{}) {
      std::vector<size_t> nextActive;
      nextActive.reserve(active.size());
      for (const auto index : active) {
        if (out.values[index] < upperBound(members[index])) {
          nextActive.emplace_back(index);
        }
      }
      active.swap(nextActive);
    }
  }

  out.unallocated = remaining;
  return out;
}

}  // namespace AGVC
