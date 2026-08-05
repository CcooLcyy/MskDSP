#include "IEC61850RealtimeSignalBus.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace IEC61850 {
namespace {

class SpscQueue {
public:
  explicit SpscQueue(std::size_t capacity) : slots_(capacity + 1) {}

  bool TryPush(const RealtimeSignalUpdate& update) noexcept {
    const auto tail = tail_.load(std::memory_order_relaxed);
    const auto next = Next(tail);
    if (next == head_.load(std::memory_order_acquire)) {
      return false;
    }
    slots_[tail] = update;
    tail_.store(next, std::memory_order_release);
    const auto depth = depth_.fetch_add(1, std::memory_order_release) + 1;
    UpdateHighWatermark(depth);
    return true;
  }

  bool TryPop(RealtimeSignalUpdate* update) noexcept {
    if (update == nullptr) {
      return false;
    }
    const auto head = head_.load(std::memory_order_relaxed);
    if (head == tail_.load(std::memory_order_acquire)) {
      return false;
    }
    *update = slots_[head];
    head_.store(Next(head), std::memory_order_release);
    depth_.fetch_sub(1, std::memory_order_release);
    return true;
  }

  std::size_t highWatermark() const noexcept {
    return highWatermark_.load(std::memory_order_acquire);
  }

private:
  std::size_t Next(std::size_t index) const noexcept {
    return index + 1 == slots_.size() ? 0 : index + 1;
  }

  void UpdateHighWatermark(std::size_t depth) noexcept {
    auto high = highWatermark_.load(std::memory_order_relaxed);
    while (high < depth &&
           !highWatermark_.compare_exchange_weak(
               high, depth, std::memory_order_release,
               std::memory_order_relaxed)) {
    }
  }

  std::vector<RealtimeSignalUpdate> slots_;
  std::atomic<std::size_t> head_ = 0;
  std::atomic<std::size_t> tail_ = 0;
  std::atomic<std::size_t> depth_ = 0;
  std::atomic<std::size_t> highWatermark_ = 0;
};

}  // namespace

struct RealtimeSignalBus::State {
  State(std::size_t producerCount, std::size_t capacity,
        std::uint64_t sessionGenerationIn) :
    producerCount(producerCount),
    capacity(capacity),
    sessionGeneration(sessionGenerationIn),
    queues(producerCount) {
    for (auto& queue : queues) {
      queue = std::make_unique<SpscQueue>(capacity);
    }
  }

  bool TryPublish(std::size_t queueIndex, std::uint64_t producerGeneration,
                  const RealtimeSignalUpdate& update) noexcept {
    if (!active.load(std::memory_order_acquire) ||
        producerGeneration != sessionGeneration ||
        update.sessionGeneration != sessionGeneration ||
        queueIndex >= queues.size()) {
      rejectedInactive.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    if (!queues[queueIndex]->TryPush(update)) {
      dropped.fetch_add(1, std::memory_order_relaxed);
      overflowLatched.store(true, std::memory_order_release);
      return false;
    }
    published.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  bool TryConsume(std::size_t* cursor,
                  RealtimeSignalUpdate* update) noexcept {
    if (cursor == nullptr || update == nullptr ||
        !active.load(std::memory_order_acquire) || queues.empty()) {
      return false;
    }
    for (std::size_t attempt = 0; attempt < queues.size(); ++attempt) {
      const auto index = (*cursor + attempt) % queues.size();
      if (queues[index]->TryPop(update)) {
        *cursor = (index + 1) % queues.size();
        consumed.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
    }
    return false;
  }

  std::size_t producerCount;
  std::size_t capacity;
  std::uint64_t sessionGeneration;
  std::vector<std::unique_ptr<SpscQueue>> queues;
  std::atomic<bool> active = true;
  std::atomic<std::uint64_t> published = 0;
  std::atomic<std::uint64_t> consumed = 0;
  std::atomic<std::uint64_t> dropped = 0;
  std::atomic<std::uint64_t> rejectedInactive = 0;
  std::atomic<bool> overflowLatched = false;
};

RealtimeSignalBus::Producer::Producer(
    std::weak_ptr<State> state, std::size_t queueIndex,
    std::uint64_t sessionGeneration) :
  state_(std::move(state)),
  queueIndex_(queueIndex),
  sessionGeneration_(sessionGeneration) {}

bool RealtimeSignalBus::Producer::TryPublish(
    const RealtimeSignalUpdate& update) const noexcept {
  const auto state = state_.lock();
  return state != nullptr &&
         state->TryPublish(queueIndex_, sessionGeneration_, update);
}

bool RealtimeSignalBus::Producer::valid() const noexcept {
  return !state_.expired();
}

RealtimeSignalBus::RealtimeSignalBus(std::size_t producerCount,
                                     std::size_t capacity,
                                     std::uint64_t sessionGeneration) :
  state_(nullptr) {
  if (producerCount == 0 || capacity == 0 ||
      capacity == std::numeric_limits<std::size_t>::max() ||
      sessionGeneration == 0) {
    throw std::invalid_argument("IEC61850实时信号总线参数无效");
  }
  state_ = std::make_shared<State>(producerCount, capacity,
                                   sessionGeneration);
}

RealtimeSignalBus::~RealtimeSignalBus() { Invalidate(); }

RealtimeSignalBus::Producer RealtimeSignalBus::producer(
    std::size_t queueIndex) const {
  if (state_ == nullptr || queueIndex >= state_->queues.size()) {
    return {};
  }
  return Producer(state_, queueIndex, state_->sessionGeneration);
}

bool RealtimeSignalBus::TryConsume(RealtimeSignalUpdate* update) noexcept {
  return state_ != nullptr &&
         state_->TryConsume(&consumerCursor_, update);
}

void RealtimeSignalBus::Invalidate() noexcept {
  if (state_ != nullptr) {
    state_->active.store(false, std::memory_order_release);
  }
}

bool RealtimeSignalBus::IsActive() const noexcept {
  return state_ != nullptr && state_->active.load(std::memory_order_acquire);
}

RealtimeSignalBusStatistics RealtimeSignalBus::statistics() const noexcept {
  RealtimeSignalBusStatistics result;
  if (state_ == nullptr) {
    return result;
  }
  result.published = state_->published.load(std::memory_order_acquire);
  result.consumed = state_->consumed.load(std::memory_order_acquire);
  result.dropped = state_->dropped.load(std::memory_order_acquire);
  result.rejectedInactive =
      state_->rejectedInactive.load(std::memory_order_acquire);
  result.overflowLatched =
      state_->overflowLatched.load(std::memory_order_acquire);
  result.active = state_->active.load(std::memory_order_acquire);
  for (const auto& queue : state_->queues) {
    result.highWatermark =
        std::max(result.highWatermark, queue->highWatermark());
  }
  return result;
}

std::size_t RealtimeSignalBus::producerCount() const noexcept {
  return state_ == nullptr ? 0 : state_->producerCount;
}

std::size_t RealtimeSignalBus::capacity() const noexcept {
  return state_ == nullptr ? 0 : state_->capacity;
}

std::uint64_t RealtimeSignalBus::sessionGeneration() const noexcept {
  return state_ == nullptr ? 0 : state_->sessionGeneration;
}

}  // namespace IEC61850
