#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace IEC61850 {

enum class RealtimeSignalSource : std::uint8_t {
  GOOSE = 1,
  SV_DERIVED = 2,
  INTERNAL = 3,
};

enum class RealtimeSignalValueType : std::uint8_t {
  BOOLEAN = 1,
  INTEGER = 2,
  FLOATING = 3,
};

enum class RealtimeNetworkChannel : std::uint8_t {
  UNSPECIFIED = 0,
  A = 1,
  B = 2,
};

union RealtimeSignalScalar {
  bool booleanValue;
  std::int64_t integerValue;
  double floatingValue;
};

struct RealtimeSignalUpdate {
  std::uint32_t signalId = 0;
  std::uint64_t sessionGeneration = 0;
  RealtimeSignalSource source = RealtimeSignalSource::INTERNAL;
  RealtimeSignalValueType valueType = RealtimeSignalValueType::BOOLEAN;
  RealtimeNetworkChannel channel = RealtimeNetworkChannel::UNSPECIFIED;
  std::uint32_t qualityBits = 0;
  std::int64_t timestampNs = 0;
  std::uint64_t sequence = 0;
  RealtimeSignalScalar value{};
};

static_assert(std::is_trivially_copyable_v<RealtimeSignalScalar>);
static_assert(std::is_trivially_copyable_v<RealtimeSignalUpdate>);

struct RealtimeSignalBusStatistics {
  std::uint64_t published = 0;
  std::uint64_t consumed = 0;
  std::uint64_t dropped = 0;
  std::uint64_t rejectedInactive = 0;
  std::size_t highWatermark = 0;
  bool overflowLatched = false;
  bool active = false;
};

// 模块内部实时信号总线。生产者各自使用一个SPSC队列，消费者只能有一个。
class RealtimeSignalBus {
private:
  struct State;

public:
  class Producer {
  public:
    Producer() = default;

    bool TryPublish(const RealtimeSignalUpdate& update) const noexcept;
    bool valid() const noexcept;

  private:
    friend class RealtimeSignalBus;

    Producer(std::weak_ptr<State> state, std::size_t queueIndex,
             std::uint64_t sessionGeneration);

    std::weak_ptr<State> state_;
    std::size_t queueIndex_ = 0;
    std::uint64_t sessionGeneration_ = 0;
  };

  RealtimeSignalBus(std::size_t producerCount, std::size_t capacity,
                    std::uint64_t sessionGeneration);
  ~RealtimeSignalBus();

  RealtimeSignalBus(const RealtimeSignalBus&) = delete;
  RealtimeSignalBus& operator=(const RealtimeSignalBus&) = delete;

  Producer producer(std::size_t queueIndex) const;
  bool TryConsume(RealtimeSignalUpdate* update) noexcept;
  void Invalidate() noexcept;
  bool IsActive() const noexcept;
  RealtimeSignalBusStatistics statistics() const noexcept;

  std::size_t producerCount() const noexcept;
  std::size_t capacity() const noexcept;
  std::uint64_t sessionGeneration() const noexcept;

private:
  std::shared_ptr<State> state_;
  std::size_t consumerCursor_ = 0;
};

}  // namespace IEC61850
