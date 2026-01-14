#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/streambuf.hpp>

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
  using MeasuredValueCallback = TcpLink::MeasuredValueCallback;
  using SnapshotProvider = TcpLink::SnapshotProvider;

  TcpSession(boost::asio::io_context& io, IEC104Proto::LinkConfig config, bool isClient);
  ~TcpSession();

  void Start(boost::asio::ip::tcp::socket socket);
  void Stop();

  void SendMeasuredValue(uint32_t ioa, double value, uint8_t quality, uint8_t cause);

  void SetMeasuredValueCallback(MeasuredValueCallback cb);
  void SetInterrogationSnapshotProvider(SnapshotProvider provider);
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
  boost::asio::steady_timer t3Timer_;

  IEC104Proto::LinkConfig config_;
  bool isClient_;
  Apci apci_;
  uint16_t sendSeq_ = 0;
  uint16_t recvSeqExpected_ = 0;
  bool dataTransferActive_ = false;
  bool closing_ = false;

  std::deque<std::vector<uint8_t>> writeQueue_;
  bool writing_ = false;

  MeasuredValueCallback onMeasuredValue_;
  SnapshotProvider interrogationSnapshotProvider_;
  std::function<void()> onClosed_;

  void handleRead();
  void handleFrame(const std::vector<uint8_t>& apdu);
  void handleIFrame(const std::vector<uint8_t>& apdu);
  void handleSFrame(const std::vector<uint8_t>& apdu);
  void handleUFrame(const std::vector<uint8_t>& apdu);

  void processAsdu(const std::vector<uint8_t>& asdu);
  void handleMeasuredValue(const std::vector<uint8_t>& asdu);
  void handleInterrogation(const std::vector<uint8_t>& asdu);

  void sendIFrame(const std::vector<uint8_t>& asdu);
  void sendSFrame();
  void sendUFrame(UFrameType type);
  void enqueueWrite(std::vector<uint8_t> frame);
  void doWrite();

  std::vector<uint8_t> buildMeasuredValueAsdu(uint32_t ioa, double value, uint8_t quality, uint8_t cause) const;
  std::vector<uint8_t> buildInterrogationAsdu(uint8_t cause, uint8_t qoi) const;

  static FrameType frameType(const std::vector<uint8_t>& apdu);
  static Apci parseApci(const IEC104Proto::APCIParameters& in);

  void restartT3();
  void onT3Timeout(const boost::system::error_code& ec);
};

}  // namespace IEC104

namespace boost::asio {
template <>
struct is_match_condition<IEC104::MatchIEC104> : std::true_type {};
}  // namespace boost::asio
