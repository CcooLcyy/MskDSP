#include "IEC104TcpSession.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <istream>
#include <utility>

#include <boost/asio/post.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>

namespace IEC104 {
namespace {
constexpr uint8_t kApduStart = 0x68;

constexpr uint8_t kTypeIdMeasuredValueShort = 13;  // M_ME_NC_1
constexpr uint8_t kTypeIdInterrogationCmd = 100;   // C_IC_NA_1

constexpr uint8_t kCotActivation = 6;
constexpr uint8_t kCotActivationCon = 7;
constexpr uint8_t kCotActivationTermination = 10;
constexpr uint8_t kCotInterrogatedByStation = 20;

constexpr uint8_t kQoiStation = 20;

inline uint16_t parseSeq(const std::vector<uint8_t>& apdu, size_t offset) {
  return static_cast<uint16_t>((static_cast<uint16_t>(apdu.at(offset + 1)) << 7) | (apdu.at(offset) >> 1));
}

inline void writeSeq(std::vector<uint8_t>* out, size_t offset, uint16_t seq) {
  out->at(offset) = static_cast<uint8_t>((seq << 1) & 0xFF);
  out->at(offset + 1) = static_cast<uint8_t>((seq >> 7) & 0xFF);
}

inline float toFloat(double v) {
  return static_cast<float>(v);
}
}  // namespace

TcpSession::TcpSession(boost::asio::io_context& io, IEC104Proto::LinkConfig config, bool isClient) :
  io_(io),
  socket_(io),
  buffer_(),
  t3Timer_(io),
  config_(std::move(config)),
  isClient_(isClient),
  apci_(parseApci(config_.apci())) {}

TcpSession::~TcpSession() {
  t3Timer_.cancel();
  boost::system::error_code ec;
  socket_.close(ec);
}

void TcpSession::Start(boost::asio::ip::tcp::socket socket) {
  socket_ = std::move(socket);
  restartT3();
  handleRead();
  if (isClient_) {
    sendUFrame(UFrameType::STARTDT_ACT);
  }
}

void TcpSession::Stop() {
  if (closing_) {
    return;
  }
  closing_ = true;
  auto onClosed = onClosed_;
  boost::asio::post(io_, [self = shared_from_this(), onClosed = std::move(onClosed)]() mutable {
    self->t3Timer_.cancel();
    boost::system::error_code ec;
    self->socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    self->socket_.close(ec);
    self->writeQueue_.clear();
    self->writing_ = false;
    if (onClosed) {
      onClosed();
    }
  });
}

void TcpSession::SetMeasuredValueCallback(MeasuredValueCallback cb) {
  onMeasuredValue_ = std::move(cb);
}

void TcpSession::SetInterrogationSnapshotProvider(SnapshotProvider provider) {
  interrogationSnapshotProvider_ = std::move(provider);
}

void TcpSession::SetClosedCallback(std::function<void()> cb) {
  onClosed_ = std::move(cb);
}

void TcpSession::SendMeasuredValue(uint32_t ioa, double value, uint8_t quality, uint8_t cause) {
  boost::asio::post(io_, [self = shared_from_this(), ioa, value, quality, cause]() {
    if (self->closing_ || !self->dataTransferActive_) {
      return;
    }
    auto asdu = self->buildMeasuredValueAsdu(ioa, value, quality, cause);
    self->sendIFrame(asdu);
  });
}

void TcpSession::handleRead() {
  auto self = shared_from_this();
  boost::asio::async_read_until(
      socket_, buffer_, MatchIEC104{}, [self](const boost::system::error_code& ec, std::size_t length) {
        if (ec) {
          self->Stop();
          return;
        }
        self->restartT3();

        std::istream is(&self->buffer_);
        std::vector<uint8_t> apdu(length);
        is.read(reinterpret_cast<char*>(apdu.data()), static_cast<std::streamsize>(length));

        self->handleFrame(apdu);
        self->handleRead();
      });
}

TcpSession::FrameType TcpSession::frameType(const std::vector<uint8_t>& apdu) {
  if (apdu.size() < 6) {
    return FrameType::U;
  }
  auto c0 = apdu[2];
  if ((c0 & 0x03) == 0x03) {
    return FrameType::U;
  }
  if (c0 == 0x01) {
    return FrameType::S;
  }
  return FrameType::I;
}

void TcpSession::handleFrame(const std::vector<uint8_t>& apdu) {
  if (apdu.size() < 6) {
    return;
  }
  if (apdu[0] != kApduStart) {
    return;
  }

  switch (frameType(apdu)) {
  case FrameType::I:
    handleIFrame(apdu);
    break;
  case FrameType::S:
    handleSFrame(apdu);
    break;
  case FrameType::U:
    handleUFrame(apdu);
    break;
  }
}

void TcpSession::handleIFrame(const std::vector<uint8_t>& apdu) {
  if (apdu.size() < 6 + 6) {
    return;
  }
  auto remoteSendSeq = parseSeq(apdu, 2);
  auto remoteAckSeq = parseSeq(apdu, 4);
  (void)remoteAckSeq;

  if (remoteSendSeq != recvSeqExpected_) {
    // Out-of-order; best-effort ignore.
    return;
  }
  recvSeqExpected_ = static_cast<uint16_t>((recvSeqExpected_ + 1) % 32768);

  sendSFrame();
  processAsdu(std::vector<uint8_t>(apdu.begin() + 6, apdu.end()));
}

void TcpSession::handleSFrame(const std::vector<uint8_t>& apdu) {
  if (apdu.size() < 6) {
    return;
  }
}

void TcpSession::handleUFrame(const std::vector<uint8_t>& apdu) {
  if (apdu.size() < 6) {
    return;
  }
  auto type = static_cast<UFrameType>(apdu[2]);
  switch (type) {
  case UFrameType::STARTDT_ACT:
    sendUFrame(UFrameType::STARTDT_CON);
    dataTransferActive_ = true;
    break;
  case UFrameType::STARTDT_CON:
    dataTransferActive_ = true;
    break;
  case UFrameType::STOPDT_ACT:
    sendUFrame(UFrameType::STOPDT_CON);
    dataTransferActive_ = false;
    break;
  case UFrameType::STOPDT_CON:
    dataTransferActive_ = false;
    break;
  case UFrameType::TESTFR_ACT:
    sendUFrame(UFrameType::TESTFR_CON);
    break;
  case UFrameType::TESTFR_CON:
    break;
  }
}

void TcpSession::processAsdu(const std::vector<uint8_t>& asdu) {
  if (asdu.size() < 6) {
    return;
  }
  auto typeId = asdu[0];
  switch (typeId) {
  case kTypeIdMeasuredValueShort:
    handleMeasuredValue(asdu);
    break;
  case kTypeIdInterrogationCmd:
    handleInterrogation(asdu);
    break;
  default:
    break;
  }
}

void TcpSession::handleMeasuredValue(const std::vector<uint8_t>& asdu) {
  if (asdu.size() < 12) {
    return;
  }
  auto vsq = asdu[1];
  auto sq = (vsq & 0x80) != 0;
  auto count = static_cast<int>(vsq & 0x7F);
  if (count <= 0) {
    return;
  }

  size_t offset = 6;
  uint32_t baseIoa = 0;
  if (sq) {
    if (asdu.size() < offset + 3) {
      return;
    }
    baseIoa = static_cast<uint32_t>(asdu[offset]) |
              (static_cast<uint32_t>(asdu[offset + 1]) << 8) |
              (static_cast<uint32_t>(asdu[offset + 2]) << 16);
    offset += 3;
  }

  for (int i = 0; i < count; ++i) {
    uint32_t ioa = 0;
    if (!sq) {
      if (asdu.size() < offset + 3) {
        return;
      }
      ioa = static_cast<uint32_t>(asdu[offset]) |
            (static_cast<uint32_t>(asdu[offset + 1]) << 8) |
            (static_cast<uint32_t>(asdu[offset + 2]) << 16);
      offset += 3;
    } else {
      ioa = baseIoa + static_cast<uint32_t>(i);
    }

    if (asdu.size() < offset + 5) {
      return;
    }
    uint32_t bits = static_cast<uint32_t>(asdu[offset]) |
                    (static_cast<uint32_t>(asdu[offset + 1]) << 8) |
                    (static_cast<uint32_t>(asdu[offset + 2]) << 16) |
                    (static_cast<uint32_t>(asdu[offset + 3]) << 24);
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    auto qds = static_cast<uint8_t>(asdu[offset + 4]);
    offset += 5;

    if (onMeasuredValue_) {
      MeasuredValue mv;
      mv.ioa = ioa;
      mv.value = static_cast<double>(f);
      mv.quality = qds;
      onMeasuredValue_(mv);
    }
  }
}

void TcpSession::handleInterrogation(const std::vector<uint8_t>& asdu) {
  if (asdu.size() < 6 + 3 + 1) {
    return;
  }

  auto cot = static_cast<uint8_t>(asdu[2] & 0x3F);
  if (cot != kCotActivation) {
    return;
  }

  auto qoi = static_cast<uint8_t>(asdu[6 + 3]);
  if (qoi == 0) {
    qoi = kQoiStation;
  }

  sendIFrame(buildInterrogationAsdu(kCotActivationCon, qoi));

  std::vector<MeasuredValue> snapshot;
  if (interrogationSnapshotProvider_) {
    snapshot = interrogationSnapshotProvider_();
  }

  for (const auto& mv : snapshot) {
    if (closing_ || !dataTransferActive_) {
      break;
    }
    sendIFrame(buildMeasuredValueAsdu(mv.ioa, mv.value, mv.quality, kCotInterrogatedByStation));
  }

  sendIFrame(buildInterrogationAsdu(kCotActivationTermination, qoi));
}

void TcpSession::sendIFrame(const std::vector<uint8_t>& asdu) {
  std::vector<uint8_t> apdu;
  apdu.resize(6);
  apdu[0] = kApduStart;
  apdu[1] = static_cast<uint8_t>(4 + asdu.size());
  writeSeq(&apdu, 2, sendSeq_);
  writeSeq(&apdu, 4, recvSeqExpected_);
  apdu.insert(apdu.end(), asdu.begin(), asdu.end());
  sendSeq_ = static_cast<uint16_t>((sendSeq_ + 1) % 32768);
  enqueueWrite(std::move(apdu));
}

void TcpSession::sendSFrame() {
  std::vector<uint8_t> apdu(6);
  apdu[0] = kApduStart;
  apdu[1] = 0x04;
  apdu[2] = 0x01;
  apdu[3] = 0x00;
  writeSeq(&apdu, 4, recvSeqExpected_);
  enqueueWrite(std::move(apdu));
}

void TcpSession::sendUFrame(UFrameType type) {
  std::vector<uint8_t> apdu(6);
  apdu[0] = kApduStart;
  apdu[1] = 0x04;
  apdu[2] = static_cast<uint8_t>(type);
  apdu[3] = 0x00;
  apdu[4] = 0x00;
  apdu[5] = 0x00;
  enqueueWrite(std::move(apdu));
}

void TcpSession::enqueueWrite(std::vector<uint8_t> frame) {
  if (closing_) {
    return;
  }
  writeQueue_.emplace_back(std::move(frame));
  if (!writing_) {
    doWrite();
  }
}

void TcpSession::doWrite() {
  if (closing_ || writing_ || writeQueue_.empty()) {
    return;
  }
  writing_ = true;
  auto self = shared_from_this();
  boost::asio::async_write(
      socket_, boost::asio::buffer(writeQueue_.front()), [self](const boost::system::error_code& ec, std::size_t) {
        self->writing_ = false;
        if (ec) {
          self->Stop();
          return;
        }
        self->writeQueue_.pop_front();
        self->doWrite();
      });
}

std::vector<uint8_t> TcpSession::buildMeasuredValueAsdu(uint32_t ioa, double value, uint8_t quality, uint8_t cause) const {
  std::vector<uint8_t> asdu;
  asdu.reserve(6 + 3 + 4 + 1);
  asdu.emplace_back(kTypeIdMeasuredValueShort);
  asdu.emplace_back(0x01);  // VSQ: 1 object, SQ=0
  asdu.emplace_back(cause & 0x3F);
  asdu.emplace_back(static_cast<uint8_t>(config_.oa() & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>(config_.ca() & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((config_.ca() >> 8) & 0xFF));

  asdu.emplace_back(static_cast<uint8_t>(ioa & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((ioa >> 8) & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((ioa >> 16) & 0xFF));

  float f = toFloat(value);
  uint32_t bits = 0;
  std::memcpy(&bits, &f, sizeof(f));
  asdu.emplace_back(static_cast<uint8_t>(bits & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((bits >> 8) & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((bits >> 16) & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((bits >> 24) & 0xFF));
  asdu.emplace_back(quality);
  return asdu;
}

std::vector<uint8_t> TcpSession::buildInterrogationAsdu(uint8_t cause, uint8_t qoi) const {
  std::vector<uint8_t> asdu;
  asdu.reserve(6 + 3 + 1);
  asdu.emplace_back(kTypeIdInterrogationCmd);
  asdu.emplace_back(0x01);
  asdu.emplace_back(cause & 0x3F);
  asdu.emplace_back(static_cast<uint8_t>(config_.oa() & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>(config_.ca() & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((config_.ca() >> 8) & 0xFF));

  asdu.emplace_back(0x00);
  asdu.emplace_back(0x00);
  asdu.emplace_back(0x00);
  asdu.emplace_back(qoi);
  return asdu;
}

TcpSession::Apci TcpSession::parseApci(const IEC104Proto::APCIParameters& in) {
  Apci out;
  if (in.k() > 0) {
    out.k = static_cast<uint16_t>(in.k());
  }
  if (in.w() > 0) {
    out.w = static_cast<uint16_t>(in.w());
  }
  if (in.t0() > 0) {
    out.t0 = static_cast<int>(in.t0());
  }
  if (in.t1() > 0) {
    out.t1 = static_cast<int>(in.t1());
  }
  if (in.t2() > 0) {
    out.t2 = static_cast<int>(in.t2());
  }
  if (in.t3() > 0) {
    out.t3 = static_cast<int>(in.t3());
  }
  return out;
}

void TcpSession::restartT3() {
  t3Timer_.expires_after(std::chrono::seconds(apci_.t3));
  auto self = shared_from_this();
  t3Timer_.async_wait([self](const boost::system::error_code& ec) { self->onT3Timeout(ec); });
}

void TcpSession::onT3Timeout(const boost::system::error_code& ec) {
  if (ec) {
    return;
  }
  if (closing_) {
    return;
  }
  if (dataTransferActive_) {
    sendUFrame(UFrameType::TESTFR_ACT);
  }
  restartT3();
}

}  // namespace IEC104
