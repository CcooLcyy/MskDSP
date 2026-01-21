#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/system/error_code.hpp>

#include "IEC104TcpLink.h"

namespace IEC104 {

struct MatchIEC104 {
  template <typename Iterator>
  std::pair<Iterator, bool> operator()(Iterator begin, Iterator end) const {
    auto available = std::distance(begin, end);
    if (available < 2) {
      return {begin, false};
    }
    auto cur = begin;
    while (cur != end && static_cast<uint8_t>(*cur) != 0x68) {
      ++cur;
    }
    if (cur == end) {
      return {begin, false};
    }
    if (std::distance(cur, end) < 2) {
      return {begin, false};
    }
    auto apduLen = static_cast<uint8_t>(*(cur + 1));
    if (static_cast<int>(apduLen) + 2 > std::distance(cur, end)) {
      return {begin, false};
    }
    return {cur + apduLen + 2, true};
  }
};

class TcpSession : public std::enable_shared_from_this<TcpSession> {
public:
  using PointValueCallback = TcpLink::PointValueCallback;
  using SnapshotProvider = TcpLink::SnapshotProvider;
  using TimeSyncCallback = TcpLink::TimeSyncCallback;

  TcpSession(boost::asio::io_context& io, IEC104Proto::LinkConfig config, bool isClient);
  ~TcpSession();

  void Start(boost::asio::ip::tcp::socket socket);
  void Stop();

  void SendPointValue(const PointValue& value, uint8_t cause);
  void SendTimeSync(int64_t tsMs);

  void SetPointValueCallback(PointValueCallback cb);
  void SetInterrogationSnapshotProvider(SnapshotProvider provider);
  void SetTimeSyncCallback(TimeSyncCallback cb);
  void SetClosedCallback(std::function<void()> cb);

private:
  enum class FrameType { I, S, U };
  enum class UFrameType : uint8_t {
    STARTDT_ACT = 0x07,
    STARTDT_CON = 0x0B,
    STOPDT_ACT = 0x13,
    STOPDT_CON = 0x23,
    TESTFR_ACT = 0x43,
    TESTFR_CON = 0x83,
  };

  struct Apci {
    uint16_t k = 12;
    uint16_t w = 8;
    int t0 = 30;
    int t1 = 15;
    int t2 = 10;
    int t3 = 20;
  };

  boost::asio::io_context& io_;
  boost::asio::ip::tcp::socket socket_;
  boost::asio::streambuf buffer_;
  boost::asio::steady_timer t0Timer_;
  boost::asio::steady_timer t1Timer_;
  boost::asio::steady_timer t2Timer_;
  boost::asio::steady_timer t3Timer_;
  boost::asio::steady_timer telemetryFlushTimer_;

  IEC104Proto::LinkConfig config_;
  bool isClient_;
  Apci apci_;
  uint16_t sendSeq_ = 0;
  uint16_t sendAckedSeq_ = 0;
  uint16_t recvSeqExpected_ = 0;
  size_t sendUnacked_ = 0;
  size_t recvSinceLastAck_ = 0;
  bool dataTransferActive_ = false;
  bool closing_ = false;
  bool ackPending_ = false;
  bool autoInterrogationSent_ = false;

  std::deque<std::vector<uint8_t>> writeQueue_;
  std::deque<std::vector<uint8_t>> pendingAsdu_;
  bool writing_ = false;

  PointValueCallback onPointValue_;
  SnapshotProvider interrogationSnapshotProvider_;
  TimeSyncCallback onTimeSync_;
  std::function<void()> onClosed_;

  struct PendingPointValue {
    PointValue value;
    uint8_t cause = 0;
  };

  std::unordered_map<uint64_t, PendingPointValue> telemetryPendingByKey_;
  std::vector<PendingPointValue> telemetryPending_;
  std::chrono::milliseconds telemetryBatchWindow_{0};
  uint32_t telemetryMaxAsduBytes_ = 0;
  bool telemetryDedupe_ = true;
  bool telemetryFlushScheduled_ = false;

  void handleRead();
  void handleFrame(const std::vector<uint8_t>& apdu);
  void handleIFrame(const std::vector<uint8_t>& apdu);
  void handleSFrame(const std::vector<uint8_t>& apdu);
  void handleUFrame(const std::vector<uint8_t>& apdu);

  void processAsdu(const std::vector<uint8_t>& asdu);
  void handleMeasuredValue(const std::vector<uint8_t>& asdu, bool withTime);
  void handleSinglePoint(const std::vector<uint8_t>& asdu, bool withTime);
  void handleTimeSyncCommand(const std::vector<uint8_t>& asdu);
  void handleInterrogation(const std::vector<uint8_t>& asdu);

  void enqueueAsdu(std::vector<uint8_t> asdu);
  void enqueuePointValue(const PointValue& value, uint8_t cause);
  void scheduleTelemetryFlush();
  void flushTelemetry(const boost::system::error_code& ec);
  void drainTelemetryQueue();
  void clearTelemetryQueue();
  void enqueuePointValuesBatch(std::vector<PointValue> values, uint8_t cause);
  void trySendPending();
  void sendIFrame(const std::vector<uint8_t>& asdu);
  void sendSFrame();
  void sendUFrame(UFrameType type);
  void enqueueWrite(std::vector<uint8_t> frame);
  void doWrite();

  std::vector<uint8_t> buildMeasuredValueAsdu(uint32_t ioa, double value, uint8_t quality, uint8_t cause, int64_t tsMs, bool withTime) const;
  std::vector<uint8_t> buildSinglePointAsdu(uint32_t ioa, bool value, uint8_t quality, uint8_t cause, int64_t tsMs, bool withTime) const;
  std::vector<uint8_t> buildMeasuredValueAsduSq0(const std::vector<PointValue>& values,
                                                  size_t start,
                                                  size_t count,
                                                  uint8_t cause,
                                                  bool withTime) const;
  std::vector<uint8_t> buildMeasuredValueAsduSq1(const std::vector<PointValue>& values,
                                                   size_t start,
                                                   size_t count,
                                                   uint8_t cause,
                                                   bool withTime) const;
  std::vector<uint8_t> buildSinglePointAsduSq0(const std::vector<PointValue>& values,
                                               size_t start,
                                               size_t count,
                                               uint8_t cause,
                                               bool withTime) const;
  std::vector<uint8_t> buildSinglePointAsduSq1(const std::vector<PointValue>& values,
                                               size_t start,
                                               size_t count,
                                               uint8_t cause,
                                               bool withTime) const;
  std::vector<uint8_t> buildInterrogationAsdu(uint8_t cause, uint8_t qoi) const;
  std::vector<uint8_t> buildTimeSyncAsdu(uint8_t cause, int64_t tsMs) const;
  static bool encodeCp56Time2a(int64_t tsMs, std::array<uint8_t, 7>* out);
  static bool decodeCp56Time2a(const uint8_t* data, size_t size, int64_t* outMs);

  static FrameType frameType(const std::vector<uint8_t>& apdu);
  static uint16_t seqDistance(uint16_t from, uint16_t to);
  static Apci parseApci(const IEC104Proto::APCIParameters& in);

  void handleAck(uint16_t remoteAckSeq);
  void setDataTransferActive(bool active, const char* reason);
  bool isMasterStation() const;
  void sendAutoInterrogation(uint8_t qoi);
  void sendTimeSync(int64_t tsMs);
  void initTelemetryBatchSettings();

  void startT0();
  void stopT0();
  void onT0Timeout(const boost::system::error_code& ec);

  void startT1();
  void stopT1();
  void onT1Timeout(const boost::system::error_code& ec);

  void startT2();
  void stopT2();
  void onT2Timeout(const boost::system::error_code& ec);

  void restartT3();
  void onT3Timeout(const boost::system::error_code& ec);
};

}  // namespace IEC104

namespace boost::asio {
template <>
struct is_match_condition<IEC104::MatchIEC104> : std::true_type {};
}  // namespace boost::asio
