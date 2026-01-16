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

#include "Logger.h"

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
  t0Timer_(io),
  t1Timer_(io),
  t2Timer_(io),
  t3Timer_(io),
  config_(std::move(config)),
  isClient_(isClient),
  apci_(parseApci(config_.apci())) {}

TcpSession::~TcpSession() {
  t0Timer_.cancel();
  t1Timer_.cancel();
  t2Timer_.cancel();
  t3Timer_.cancel();
  boost::system::error_code ec;
  socket_.close(ec);
}

void TcpSession::Start(boost::asio::ip::tcp::socket socket) {
  socket_ = std::move(socket);
  closing_ = false;
  dataTransferActive_ = false;
  ackPending_ = false;
  autoInterrogationSent_ = false;
  sendSeq_ = 0;
  sendAckedSeq_ = 0;
  recvSeqExpected_ = 0;
  sendUnacked_ = 0;
  recvSinceLastAck_ = 0;
  pendingAsdu_.clear();
  writeQueue_.clear();
  writing_ = false;

  LOG_INFO("IEC104 session start: conn_name={}, role={}, k={}, w={}, t0={}, t1={}, t2={}, t3={}",
           config_.conn_name(),
           isClient_ ? "CLIENT" : "SERVER",
           apci_.k,
           apci_.w,
           apci_.t0,
           apci_.t1,
           apci_.t2,
           apci_.t3);
  startT0();
  restartT3();
  handleRead();
  if (isClient_) {
    LOG_INFO("IEC104 send STARTDT_ACT: conn_name={}", config_.conn_name());
    sendUFrame(UFrameType::STARTDT_ACT);
  }
}

void TcpSession::Stop() {
  if (closing_) {
    return;
  }
  closing_ = true;
  LOG_INFO("IEC104 session stop: conn_name={}, role={}", config_.conn_name(), isClient_ ? "CLIENT" : "SERVER");
  auto onClosed = onClosed_;
  boost::asio::post(io_, [self = shared_from_this(), onClosed = std::move(onClosed)]() mutable {
    self->t0Timer_.cancel();
    self->t1Timer_.cancel();
    self->t2Timer_.cancel();
    self->t3Timer_.cancel();
    boost::system::error_code ec;
    self->socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    self->socket_.close(ec);
    self->dataTransferActive_ = false;
    self->sendUnacked_ = 0;
    self->ackPending_ = false;
    self->recvSinceLastAck_ = 0;
    self->writeQueue_.clear();
    self->pendingAsdu_.clear();
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
    if (self->closing_) {
      return;
    }
    auto asdu = self->buildMeasuredValueAsdu(ioa, value, quality, cause);
    self->enqueueAsdu(std::move(asdu));
  });
}

void TcpSession::handleRead() {
  auto self = shared_from_this();
  boost::asio::async_read_until(
      socket_, buffer_, MatchIEC104{}, [self](const boost::system::error_code& ec, std::size_t length) {
        if (ec) {
          LOG_WARNING("IEC104 read failed: conn_name={}, error={}", self->config_.conn_name(), ec.message());
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
    LOG_DEBUG("IEC104 drop short frame: conn_name={}, size={}", config_.conn_name(), apdu.size());
    return;
  }
  if (apdu[0] != kApduStart) {
    LOG_DEBUG("IEC104 drop invalid start byte: conn_name={}, value=0x{:02X}", config_.conn_name(), apdu[0]);
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
    LOG_DEBUG("IEC104 drop short I frame: conn_name={}, size={}", config_.conn_name(), apdu.size());
    return;
  }
  auto remoteSendSeq = parseSeq(apdu, 2);
  auto remoteAckSeq = parseSeq(apdu, 4);

  LOG_DEBUG("IEC104 recv I frame: conn_name={}, ns={}, nr={}", config_.conn_name(), remoteSendSeq, remoteAckSeq);
  if (!dataTransferActive_) {
    LOG_WARNING("IEC104 recv I frame before STARTDT: conn_name={}, ns={}, nr={}", config_.conn_name(), remoteSendSeq, remoteAckSeq);
    Stop();
    return;
  }

  handleAck(remoteAckSeq);
  if (closing_) {
    return;
  }

  if (remoteSendSeq != recvSeqExpected_) {
    LOG_ERROR("IEC104 unexpected send seq: conn_name={}, expect={}, got={}",
              config_.conn_name(),
              recvSeqExpected_,
              remoteSendSeq);
    Stop();
    return;
  }
  recvSeqExpected_ = static_cast<uint16_t>((recvSeqExpected_ + 1) % 32768);
  ackPending_ = true;
  recvSinceLastAck_++;
  if (recvSinceLastAck_ >= apci_.w) {
    sendSFrame();
  } else {
    startT2();
  }

  processAsdu(std::vector<uint8_t>(apdu.begin() + 6, apdu.end()));
}

void TcpSession::handleSFrame(const std::vector<uint8_t>& apdu) {
  if (apdu.size() < 6) {
    LOG_DEBUG("IEC104 drop short S frame: conn_name={}, size={}", config_.conn_name(), apdu.size());
    return;
  }
  auto remoteAckSeq = parseSeq(apdu, 4);
  LOG_DEBUG("IEC104 recv S frame: conn_name={}, nr={}", config_.conn_name(), remoteAckSeq);
  if (!dataTransferActive_) {
    LOG_WARNING("IEC104 recv S frame before STARTDT: conn_name={}, nr={}", config_.conn_name(), remoteAckSeq);
    return;
  }
  handleAck(remoteAckSeq);
}

void TcpSession::handleUFrame(const std::vector<uint8_t>& apdu) {
  if (apdu.size() < 6) {
    LOG_DEBUG("IEC104 drop short U frame: conn_name={}, size={}", config_.conn_name(), apdu.size());
    return;
  }
  auto type = static_cast<UFrameType>(apdu[2]);
  switch (type) {
  case UFrameType::STARTDT_ACT:
    LOG_INFO("IEC104 recv STARTDT_ACT: conn_name={}", config_.conn_name());
    sendUFrame(UFrameType::STARTDT_CON);
    setDataTransferActive(true, "STARTDT_ACT");
    break;
  case UFrameType::STARTDT_CON:
    LOG_INFO("IEC104 recv STARTDT_CON: conn_name={}", config_.conn_name());
    setDataTransferActive(true, "STARTDT_CON");
    break;
  case UFrameType::STOPDT_ACT:
    LOG_INFO("IEC104 recv STOPDT_ACT: conn_name={}", config_.conn_name());
    sendUFrame(UFrameType::STOPDT_CON);
    setDataTransferActive(false, "STOPDT_ACT");
    break;
  case UFrameType::STOPDT_CON:
    LOG_INFO("IEC104 recv STOPDT_CON: conn_name={}", config_.conn_name());
    setDataTransferActive(false, "STOPDT_CON");
    break;
  case UFrameType::TESTFR_ACT:
    LOG_DEBUG("IEC104 recv TESTFR_ACT: conn_name={}", config_.conn_name());
    sendUFrame(UFrameType::TESTFR_CON);
    break;
  case UFrameType::TESTFR_CON:
    LOG_DEBUG("IEC104 recv TESTFR_CON: conn_name={}", config_.conn_name());
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
  if (!dataTransferActive_) {
    LOG_WARNING("IEC104 recv interrogation before STARTDT: conn_name={}", config_.conn_name());
    return;
  }

  auto qoi = static_cast<uint8_t>(asdu[6 + 3]);
  if (qoi == 0) {
    qoi = kQoiStation;
  }

  LOG_INFO("IEC104 recv interrogation activation: conn_name={}, qoi={}", config_.conn_name(), qoi);
  enqueueAsdu(buildInterrogationAsdu(kCotActivationCon, qoi));

  std::vector<MeasuredValue> snapshot;
  if (interrogationSnapshotProvider_) {
    snapshot = interrogationSnapshotProvider_();
  }

  LOG_DEBUG("IEC104 interrogation snapshot: conn_name={}, count={}", config_.conn_name(), snapshot.size());
  for (const auto& mv : snapshot) {
    if (closing_ || !dataTransferActive_) {
      break;
    }
    enqueueAsdu(buildMeasuredValueAsdu(mv.ioa, mv.value, mv.quality, kCotInterrogatedByStation));
  }

  enqueueAsdu(buildInterrogationAsdu(kCotActivationTermination, qoi));
}

void TcpSession::enqueueAsdu(std::vector<uint8_t> asdu) {
  if (closing_) {
    return;
  }
  pendingAsdu_.emplace_back(std::move(asdu));
  LOG_DEBUG("IEC104 queue ASDU: conn_name={}, pending={}, active={}",
            config_.conn_name(),
            pendingAsdu_.size(),
            dataTransferActive_);
  trySendPending();
}

void TcpSession::trySendPending() {
  if (closing_ || !dataTransferActive_) {
    return;
  }
  while (!pendingAsdu_.empty() && sendUnacked_ < apci_.k) {
    auto asdu = std::move(pendingAsdu_.front());
    pendingAsdu_.pop_front();
    sendIFrame(asdu);
  }
}

void TcpSession::sendIFrame(const std::vector<uint8_t>& asdu) {
  if (closing_) {
    return;
  }
  if (!dataTransferActive_) {
    LOG_WARNING("IEC104 send I frame while inactive: conn_name={}", config_.conn_name());
    pendingAsdu_.emplace_front(asdu);
    return;
  }
  if (sendUnacked_ >= apci_.k) {
    LOG_DEBUG("IEC104 send window full, queue ASDU: conn_name={}, k={}, unacked={}",
              config_.conn_name(),
              apci_.k,
              sendUnacked_);
    pendingAsdu_.emplace_front(asdu);
    return;
  }

  std::vector<uint8_t> apdu;
  apdu.resize(6);
  apdu[0] = kApduStart;
  apdu[1] = static_cast<uint8_t>(4 + asdu.size());
  writeSeq(&apdu, 2, sendSeq_);
  writeSeq(&apdu, 4, recvSeqExpected_);
  apdu.insert(apdu.end(), asdu.begin(), asdu.end());
  LOG_DEBUG("IEC104 send I frame: conn_name={}, ns={}, nr={}, unacked={}",
            config_.conn_name(),
            sendSeq_,
            recvSeqExpected_,
            sendUnacked_ + 1);
  sendSeq_ = static_cast<uint16_t>((sendSeq_ + 1) % 32768);
  sendUnacked_++;
  if (ackPending_) {
    ackPending_ = false;
    recvSinceLastAck_ = 0;
    stopT2();
  }
  startT1();
  enqueueWrite(std::move(apdu));
}

void TcpSession::sendSFrame() {
  if (closing_ || !dataTransferActive_) {
    return;
  }
  std::vector<uint8_t> apdu(6);
  apdu[0] = kApduStart;
  apdu[1] = 0x04;
  apdu[2] = 0x01;
  apdu[3] = 0x00;
  writeSeq(&apdu, 4, recvSeqExpected_);
  LOG_DEBUG("IEC104 send S frame: conn_name={}, nr={}", config_.conn_name(), recvSeqExpected_);
  ackPending_ = false;
  recvSinceLastAck_ = 0;
  stopT2();
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
  const char* typeName = "UNKNOWN";
  switch (type) {
  case UFrameType::STARTDT_ACT:
    typeName = "STARTDT_ACT";
    break;
  case UFrameType::STARTDT_CON:
    typeName = "STARTDT_CON";
    break;
  case UFrameType::STOPDT_ACT:
    typeName = "STOPDT_ACT";
    break;
  case UFrameType::STOPDT_CON:
    typeName = "STOPDT_CON";
    break;
  case UFrameType::TESTFR_ACT:
    typeName = "TESTFR_ACT";
    break;
  case UFrameType::TESTFR_CON:
    typeName = "TESTFR_CON";
    break;
  }
  LOG_DEBUG("IEC104 send U frame: conn_name={}, type={}", config_.conn_name(), typeName);
  enqueueWrite(std::move(apdu));
}

void TcpSession::enqueueWrite(std::vector<uint8_t> frame) {
  if (closing_) {
    return;
  }
  writeQueue_.emplace_back(std::move(frame));
  LOG_DEBUG("IEC104 enqueue write: conn_name={}, queued={}", config_.conn_name(), writeQueue_.size());
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
          LOG_WARNING("IEC104 write failed: conn_name={}, error={}", self->config_.conn_name(), ec.message());
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

uint16_t TcpSession::seqDistance(uint16_t from, uint16_t to) {
  return static_cast<uint16_t>((to + 32768 - from) % 32768);
}

void TcpSession::handleAck(uint16_t remoteAckSeq) {
  if (sendUnacked_ == 0) {
    if (remoteAckSeq != sendAckedSeq_) {
      LOG_ERROR("IEC104 invalid ack: conn_name={}, ack={}, expected={}",
                config_.conn_name(),
                remoteAckSeq,
                sendAckedSeq_);
      Stop();
    }
    return;
  }
  auto diff = seqDistance(sendAckedSeq_, remoteAckSeq);
  if (diff == 0) {
    return;
  }
  if (diff > sendUnacked_) {
    LOG_ERROR("IEC104 ack out of range: conn_name={}, ack={}, oldest={}, unacked={}",
              config_.conn_name(),
              remoteAckSeq,
              sendAckedSeq_,
              sendUnacked_);
    Stop();
    return;
  }
  sendAckedSeq_ = remoteAckSeq;
  sendUnacked_ -= diff;
  LOG_DEBUG("IEC104 ack update: conn_name={}, acked={}, remaining={}",
            config_.conn_name(),
            diff,
            sendUnacked_);
  if (sendUnacked_ == 0) {
    stopT1();
  } else {
    startT1();
  }
  trySendPending();
}

void TcpSession::setDataTransferActive(bool active, const char* reason) {
  if (dataTransferActive_ == active) {
    return;
  }
  dataTransferActive_ = active;
  if (active) {
    LOG_INFO("IEC104 data transfer active: conn_name={}, reason={}", config_.conn_name(), reason);
    stopT0();
    if (isClient_ && !autoInterrogationSent_) {
      sendAutoInterrogation(kQoiStation);
    }
    trySendPending();
    return;
  }

  LOG_INFO("IEC104 data transfer stopped: conn_name={}, reason={}", config_.conn_name(), reason);
  stopT1();
  stopT2();
  ackPending_ = false;
  recvSinceLastAck_ = 0;
  sendUnacked_ = 0;
  sendAckedSeq_ = sendSeq_;
  pendingAsdu_.clear();
}

void TcpSession::sendAutoInterrogation(uint8_t qoi) {
  if (!isClient_ || autoInterrogationSent_) {
    return;
  }
  autoInterrogationSent_ = true;
  LOG_INFO("IEC104 auto interrogation: conn_name={}, qoi={}", config_.conn_name(), qoi);
  enqueueAsdu(buildInterrogationAsdu(kCotActivation, qoi));
}

void TcpSession::startT0() {
  if (apci_.t0 <= 0) {
    return;
  }
  t0Timer_.expires_after(std::chrono::seconds(apci_.t0));
  auto self = shared_from_this();
  t0Timer_.async_wait([self](const boost::system::error_code& ec) { self->onT0Timeout(ec); });
  LOG_DEBUG("IEC104 start t0: conn_name={}, t0={}", config_.conn_name(), apci_.t0);
}

void TcpSession::stopT0() {
  t0Timer_.cancel();
}

void TcpSession::onT0Timeout(const boost::system::error_code& ec) {
  if (ec) {
    return;
  }
  if (closing_ || dataTransferActive_) {
    return;
  }
  LOG_WARNING("IEC104 t0 timeout: conn_name={}, t0={}", config_.conn_name(), apci_.t0);
  Stop();
}

void TcpSession::startT1() {
  if (apci_.t1 <= 0 || sendUnacked_ == 0) {
    return;
  }
  t1Timer_.expires_after(std::chrono::seconds(apci_.t1));
  auto self = shared_from_this();
  t1Timer_.async_wait([self](const boost::system::error_code& ec) { self->onT1Timeout(ec); });
  LOG_DEBUG("IEC104 start t1: conn_name={}, t1={}, unacked={}", config_.conn_name(), apci_.t1, sendUnacked_);
}

void TcpSession::stopT1() {
  t1Timer_.cancel();
}

void TcpSession::onT1Timeout(const boost::system::error_code& ec) {
  if (ec) {
    return;
  }
  if (closing_ || sendUnacked_ == 0) {
    return;
  }
  LOG_WARNING("IEC104 t1 timeout: conn_name={}, t1={}, unacked={}", config_.conn_name(), apci_.t1, sendUnacked_);
  Stop();
}

void TcpSession::startT2() {
  if (apci_.t2 <= 0 || !ackPending_) {
    return;
  }
  t2Timer_.expires_after(std::chrono::seconds(apci_.t2));
  auto self = shared_from_this();
  t2Timer_.async_wait([self](const boost::system::error_code& ec) { self->onT2Timeout(ec); });
  LOG_DEBUG("IEC104 start t2: conn_name={}, t2={}, pending_ack={}", config_.conn_name(), apci_.t2, recvSinceLastAck_);
}

void TcpSession::stopT2() {
  t2Timer_.cancel();
}

void TcpSession::onT2Timeout(const boost::system::error_code& ec) {
  if (ec) {
    return;
  }
  if (closing_ || !ackPending_) {
    return;
  }
  LOG_DEBUG("IEC104 t2 timeout: conn_name={}, pending_ack={}", config_.conn_name(), recvSinceLastAck_);
  sendSFrame();
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
    LOG_DEBUG("IEC104 t3 timeout: conn_name={}, t3={}", config_.conn_name(), apci_.t3);
    sendUFrame(UFrameType::TESTFR_ACT);
  }
  restartT3();
}

}  // namespace IEC104
