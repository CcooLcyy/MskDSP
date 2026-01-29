#include "IEC104TcpSession.h"

#include <algorithm>
#include <array>
#include <boost/asio/post.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <format>
#include <istream>
#include <string>
#include <utility>

#include "Logger.h"

namespace IEC104 {
namespace {
constexpr uint8_t kApduStart = 0x68;

constexpr uint8_t kTypeIdSinglePoint = 1;                  // M_SP_NA_1
constexpr uint8_t kTypeIdSinglePointWithTime = 30;         // M_SP_TB_1
constexpr uint8_t kTypeIdMeasuredValueShort = 13;          // M_ME_NC_1
constexpr uint8_t kTypeIdMeasuredValueShortWithTime = 36;  // M_ME_TF_1
constexpr uint8_t kTypeIdSingleCommand = 45;               // C_SC_NA_1
constexpr uint8_t kTypeIdSetpointShort = 50;               // C_SE_NC_1
constexpr uint8_t kTypeIdInterrogationCmd = 100;           // C_IC_NA_1
constexpr uint8_t kTypeIdTimeSyncCmd = 103;                // C_CS_NA_1

constexpr uint8_t kCotActivation = 6;
constexpr uint8_t kCotActivationCon = 7;
constexpr uint8_t kCotActivationTermination = 10;
constexpr uint8_t kCotInterrogatedByStation = 20;
constexpr uint8_t kCotNegative = 0x40;

constexpr uint8_t kQoiStation = 20;

constexpr uint8_t kScoSelectMask = 0x80;
constexpr uint8_t kScoValueMask = 0x01;
constexpr uint8_t kQosSelectMask = 0x80;

constexpr char kHexDigits[] = "0123456789ABCDEF";

constexpr uint32_t kMaxAsduBytes = 249;
constexpr uint32_t kAsduHeaderSize = 6;
constexpr uint32_t kMeasuredValueSq1BaseSize = 3;
constexpr uint32_t kSinglePointSq0ObjectSize = 4;
constexpr uint32_t kSinglePointSq1ObjectSize = 1;
constexpr uint32_t kMeasuredValueSq0ObjectSize = 8;
constexpr uint32_t kMeasuredValueSq1ObjectSize = 5;
constexpr uint32_t kCp56Time2aSize = 7;
constexpr uint32_t kMinMeasuredValueAsduBytes = kAsduHeaderSize + 3 + 4 + 1 + kCp56Time2aSize;
constexpr uint32_t kDefaultPointBatchWindowMs = 20;
constexpr std::chrono::seconds kSingleCommandSelectTimeout{10};

size_t maxSq0Objects(uint32_t maxAsduBytes, uint32_t objectSize) {
  if (maxAsduBytes <= kAsduHeaderSize) {
    return 0;
  }
  return (maxAsduBytes - kAsduHeaderSize) / objectSize;
}

size_t maxSq1Objects(uint32_t maxAsduBytes, uint32_t objectSize) {
  if (maxAsduBytes <= kAsduHeaderSize + kMeasuredValueSq1BaseSize) {
    return 0;
  }
  return (maxAsduBytes - kAsduHeaderSize - kMeasuredValueSq1BaseSize) / objectSize;
}

inline uint16_t parseSeq(const std::vector<uint8_t> &apdu, size_t offset) {
  return static_cast<uint16_t>((static_cast<uint16_t>(apdu.at(offset + 1)) << 7) | (apdu.at(offset) >> 1));
}

inline void writeSeq(std::vector<uint8_t> *out, size_t offset, uint16_t seq) {
  out->at(offset) = static_cast<uint8_t>((seq << 1) & 0xFF);
  out->at(offset + 1) = static_cast<uint8_t>((seq >> 7) & 0xFF);
}

inline float toFloat(double v) {
  return static_cast<float>(v);
}

inline uint8_t buildCot(uint8_t cause, bool positive) {
  uint8_t cot = static_cast<uint8_t>(cause & 0x3F);
  if (!positive) {
    cot |= kCotNegative;
  }
  return cot;
}

std::string bytesToHex(const std::vector<uint8_t> &data) {
  if (data.empty()) {
    return {};
  }
  std::string out;
  out.reserve(data.size() * 3 - 1);
  for (size_t i = 0; i < data.size(); ++i) {
    const auto byte = data[i];
    out.push_back(kHexDigits[(byte >> 4) & 0x0F]);
    out.push_back(kHexDigits[byte & 0x0F]);
    if (i + 1 != data.size()) {
      out.push_back(' ');
    }
  }
  return out;
}
}  // namespace

TcpSession::TcpSession(boost::asio::io_context &io, IEC104Proto::LinkConfig config, bool isClient) :
  io_(io),
  socket_(io),
  buffer_(),
  t0Timer_(io),
  t1Timer_(io),
  t2Timer_(io),
  t3Timer_(io),
  pointFlushTimer_(io),
  config_(std::move(config)),
  isClient_(isClient),
  apci_(parseApci(config_.apci())) {
  initPointBatchSettings();
}

TcpSession::~TcpSession() {
  t0Timer_.cancel();
  t1Timer_.cancel();
  t2Timer_.cancel();
  t3Timer_.cancel();
  pointFlushTimer_.cancel();
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

  LOG_INFO("IEC104 会话启动: conn_name={}, 角色={}, k={}, w={}, t0={}, t1={}, t2={}, t3={}", config_.conn_name(), isClient_ ? "客户端" : "服务端", apci_.k, apci_.w, apci_.t0, apci_.t1, apci_.t2, apci_.t3);
  LOG_INFO("IEC104 点值上送参数: conn_name={}, 窗口毫秒={}, 最大ASDU字节={}, 标准上限={}, 去重={}, 带时标={}", config_.conn_name(), pointBatchWindow_.count(), pointMaxAsduBytes_, config_.point_use_standard_limit(), pointDedupe_, pointWithTime_);
  startT0();
  restartT3();
  handleRead();
  if (isClient_) {
    LOG_INFO("IEC104 发送 STARTDT_ACT: conn_name={}", config_.conn_name());
    sendUFrame(UFrameType::STARTDT_ACT);
  }
}

void TcpSession::Stop() {
  if (closing_) {
    return;
  }
  closing_ = true;
  LOG_INFO("IEC104 会话停止: conn_name={}, 角色={}", config_.conn_name(), isClient_ ? "客户端" : "服务端");
  auto onClosed = onClosed_;
  boost::asio::post(io_, [self = shared_from_this(), onClosed = std::move(onClosed)]() mutable {
    self->t0Timer_.cancel();
    self->t1Timer_.cancel();
    self->t2Timer_.cancel();
    self->t3Timer_.cancel();
    self->pointFlushTimer_.cancel();
    boost::system::error_code ec;
    self->socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    self->socket_.close(ec);
    self->dataTransferActive_ = false;
    self->sendUnacked_ = 0;
    self->ackPending_ = false;
    self->recvSinceLastAck_ = 0;
    self->writeQueue_.clear();
    self->pendingAsdu_.clear();
    self->pointPendingByKey_.clear();
    self->pointPending_.clear();
    self->pointFlushScheduled_ = false;
    self->singleCommandSelectByIoa_.clear();
    self->writing_ = false;
    if (onClosed) {
      onClosed();
    }
  });
}

void TcpSession::SetPointValueCallback(PointValueCallback cb) {
  onPointValue_ = std::move(cb);
}

void TcpSession::SetInterrogationSnapshotProvider(SnapshotProvider provider) {
  interrogationSnapshotProvider_ = std::move(provider);
}

void TcpSession::SetTimeSyncCallback(TimeSyncCallback cb) {
  onTimeSync_ = std::move(cb);
}

void TcpSession::SetCommandCallback(CommandCallback cb) {
  onCommand_ = std::move(cb);
}

void TcpSession::SetClosedCallback(std::function<void()> cb) {
  onClosed_ = std::move(cb);
}

void TcpSession::SendPointValue(const PointValue &value, uint8_t cause) {
  boost::asio::post(io_, [self = shared_from_this(), value, cause]() {
    if (self->closing_) {
      return;
    }
    self->enqueuePointValue(value, cause);
  });
}

void TcpSession::SendTimeSync(int64_t tsMs) {
  boost::asio::post(io_, [self = shared_from_this(), tsMs]() {
    if (self->closing_) {
      return;
    }
    self->sendTimeSync(tsMs);
  });
}

void TcpSession::SendSingleCommand(uint32_t ioa, bool value, bool useSelect) {
  boost::asio::post(io_, [self = shared_from_this(), ioa, value, useSelect]() {
    if (self->closing_) {
      return;
    }
    if (useSelect) {
      self->sendSingleCommand(ioa, value, true);
      self->sendSingleCommand(ioa, value, false);
    } else {
      self->sendSingleCommand(ioa, value, false);
    }
  });
}

void TcpSession::SendSetpointCommand(uint32_t ioa, double value) {
  boost::asio::post(io_, [self = shared_from_this(), ioa, value]() {
    if (self->closing_) {
      return;
    }
    self->sendSetpointCommand(ioa, value);
  });
}

void TcpSession::initPointBatchSettings() {
  const auto windowMs = config_.point_batch_window_ms();
  pointBatchWindow_ = std::chrono::milliseconds(windowMs == 0 ? kDefaultPointBatchWindowMs : windowMs);

  uint32_t maxBytes = config_.point_max_asdu_bytes();
  if (config_.point_use_standard_limit() || maxBytes == 0) {
    maxBytes = kMaxAsduBytes;
  }
  maxBytes = std::min(maxBytes, kMaxAsduBytes);
  if (maxBytes < kMinMeasuredValueAsduBytes) {
    maxBytes = kMinMeasuredValueAsduBytes;
  }
  pointMaxAsduBytes_ = maxBytes;

  pointDedupe_ = config_.has_point_dedupe() ? config_.point_dedupe() : true;
  pointWithTime_ = config_.point_with_time();
}

void TcpSession::enqueuePointValue(const PointValue &value, uint8_t cause) {
  PendingPointValue entry;
  entry.value = value;
  entry.cause = cause;

  if (pointDedupe_) {
    const uint64_t key = (static_cast<uint64_t>(value.type) << 32) | value.ioa;
    pointPendingByKey_[key] = entry;
  } else {
    pointPending_.push_back(entry);
  }
  schedulePointFlush();
}

void TcpSession::schedulePointFlush() {
  if (pointBatchWindow_.count() <= 0) {
    drainPointQueue();
    return;
  }
  if (pointFlushScheduled_) {
    return;
  }
  pointFlushScheduled_ = true;
  pointFlushTimer_.expires_after(pointBatchWindow_);
  auto self = shared_from_this();
  pointFlushTimer_.async_wait([self](const boost::system::error_code &ec) { self->flushPoint(ec); });
}

void TcpSession::flushPoint(const boost::system::error_code &ec) {
  if (ec) {
    return;
  }
  pointFlushScheduled_ = false;
  drainPointQueue();
}

void TcpSession::drainPointQueue() {
  if (closing_) {
    clearPointQueue();
    return;
  }

  std::vector<PendingPointValue> pending;
  if (pointDedupe_) {
    pending.reserve(pointPendingByKey_.size());
    for (auto &pair : pointPendingByKey_) {
      pending.push_back(pair.second);
    }
    pointPendingByKey_.clear();
  } else {
    pending.swap(pointPending_);
  }

  if (pending.empty()) {
    return;
  }

  LOG_DEBUG("IEC104 点值合包刷新: conn_name={}, 待处理={}, 去重={}, window_ms={}, max_asdu_bytes={}", config_.conn_name(), pending.size(), pointDedupe_, pointBatchWindow_.count(), pointMaxAsduBytes_);

  std::unordered_map<uint32_t, std::vector<PointValue>> byKey;
  byKey.reserve(pending.size());
  for (const auto &entry : pending) {
    const uint32_t key = (static_cast<uint32_t>(entry.value.type) << 8) | entry.cause;
    byKey[key].push_back(entry.value);
  }

  for (auto &pair : byKey) {
    const uint8_t cause = static_cast<uint8_t>(pair.first & 0xFF);
    enqueuePointValuesBatch(std::move(pair.second), cause);
  }
}

void TcpSession::clearPointQueue() {
  pointPendingByKey_.clear();
  pointPending_.clear();
  pointFlushScheduled_ = false;
  pointFlushTimer_.cancel();
}

void TcpSession::enqueuePointValuesBatch(std::vector<PointValue> values, uint8_t cause) {
  if (values.empty()) {
    return;
  }
  const auto type = values.front().type;

  std::stable_sort(values.begin(), values.end(), [](const PointValue &a, const PointValue &b) {
    return a.ioa < b.ioa;
  });

  const bool withTime = pointWithTime_;
  uint32_t sq0ObjectSize = 0;
  uint32_t sq1ObjectSize = 0;
  if (type == IEC104Proto::POINT_TYPE_FLOAT) {
    sq0ObjectSize = kMeasuredValueSq0ObjectSize + (withTime ? kCp56Time2aSize : 0);
    sq1ObjectSize = kMeasuredValueSq1ObjectSize + (withTime ? kCp56Time2aSize : 0);
  } else {
    sq0ObjectSize = kSinglePointSq0ObjectSize + (withTime ? kCp56Time2aSize : 0);
    sq1ObjectSize = kSinglePointSq1ObjectSize + (withTime ? kCp56Time2aSize : 0);
  }

  auto maxSq0 = maxSq0Objects(pointMaxAsduBytes_, sq0ObjectSize);
  auto maxSq1 = maxSq1Objects(pointMaxAsduBytes_, sq1ObjectSize);
  if (maxSq0 == 0) {
    maxSq0 = 1;
  }
  if (maxSq1 == 0) {
    maxSq1 = 1;
  }

  std::vector<PointValue> sq0Buffer;
  sq0Buffer.reserve(std::min(values.size(), maxSq0));

  auto flushSq0 = [this, &sq0Buffer, maxSq0, cause, type, withTime]() {
    size_t offset = 0;
    while (offset < sq0Buffer.size()) {
      const size_t count = std::min(maxSq0, sq0Buffer.size() - offset);
      if (type == IEC104Proto::POINT_TYPE_FLOAT) {
        auto asdu = buildMeasuredValueAsduSq0(sq0Buffer, offset, count, cause, withTime);
        if (!asdu.empty()) {
          enqueueAsdu(std::move(asdu));
        } else {
          LOG_WARNING("IEC104 构造遥测报文失败: conn_name={}, count={}", config_.conn_name(), count);
        }
      } else {
        auto asdu = buildSinglePointAsduSq0(sq0Buffer, offset, count, cause, withTime);
        if (!asdu.empty()) {
          enqueueAsdu(std::move(asdu));
        } else {
          LOG_WARNING("IEC104 构造单点报文失败: conn_name={}, count={}", config_.conn_name(), count);
        }
      }
      offset += count;
    }
    sq0Buffer.clear();
  };

  size_t i = 0;
  while (i < values.size()) {
    size_t j = i + 1;
    while (j < values.size() && values[j].ioa == values[j - 1].ioa + 1) {
      ++j;
    }
    const size_t runLen = j - i;
    if (runLen >= 2) {
      flushSq0();
      size_t offset = 0;
      while (offset < runLen) {
        const size_t count = std::min(maxSq1, runLen - offset);
        if (type == IEC104Proto::POINT_TYPE_FLOAT) {
          auto asdu = buildMeasuredValueAsduSq1(values, i + offset, count, cause, withTime);
          if (!asdu.empty()) {
            enqueueAsdu(std::move(asdu));
          } else {
            LOG_WARNING("IEC104 构造遥测报文失败: conn_name={}, count={}", config_.conn_name(), count);
          }
        } else {
          auto asdu = buildSinglePointAsduSq1(values, i + offset, count, cause, withTime);
          if (!asdu.empty()) {
            enqueueAsdu(std::move(asdu));
          } else {
            LOG_WARNING("IEC104 构造单点报文失败: conn_name={}, count={}", config_.conn_name(), count);
          }
        }
        offset += count;
      }
    } else {
      sq0Buffer.push_back(values[i]);
      if (sq0Buffer.size() >= maxSq0) {
        flushSq0();
      }
    }
    i = j;
  }

  flushSq0();
}

void TcpSession::handleRead() {
  auto self = shared_from_this();
  boost::asio::async_read_until(
      socket_, buffer_, MatchIEC104{}, [self](const boost::system::error_code &ec, std::size_t length) {
        if (ec) {
          LOG_WARNING("IEC104 读取失败: conn_name={}, 错误={}", self->config_.conn_name(), ec.message());
          self->Stop();
          return;
        }
        self->restartT3();

        std::istream is(&self->buffer_);
        std::vector<uint8_t> apdu(length);
        is.read(reinterpret_cast<char *>(apdu.data()), static_cast<std::streamsize>(length));

        LOG_INFO("IEC104 报文接收: conn_name={}, 角色={}, 长度={}, 数据={}", self->config_.conn_name(), self->isClient_ ? "客户端" : "服务端", apdu.size(), bytesToHex(apdu));
        self->handleFrame(apdu);
        self->handleRead();
      });
}

TcpSession::FrameType TcpSession::frameType(const std::vector<uint8_t> &apdu) {
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

void TcpSession::handleFrame(const std::vector<uint8_t> &apdu) {
  if (apdu.size() < 6) {
    LOG_DEBUG("IEC104 丢弃过短报文: conn_name={}, 长度={}", config_.conn_name(), apdu.size());
    return;
  }
  if (apdu[0] != kApduStart) {
    LOG_DEBUG("IEC104 丢弃非法起始字节: conn_name={}, 值=0x{:02X}", config_.conn_name(), apdu[0]);
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

void TcpSession::handleIFrame(const std::vector<uint8_t> &apdu) {
  if (apdu.size() < 6 + 6) {
    LOG_DEBUG("IEC104 丢弃过短 I 帧: conn_name={}, 长度={}", config_.conn_name(), apdu.size());
    return;
  }
  auto remoteSendSeq = parseSeq(apdu, 2);
  auto remoteAckSeq = parseSeq(apdu, 4);

  LOG_DEBUG("IEC104 接收 I 帧: conn_name={}, ns={}, nr={}", config_.conn_name(), remoteSendSeq, remoteAckSeq);
  if (!dataTransferActive_) {
    LOG_WARNING("IEC104 未 STARTDT 即收到 I 帧: conn_name={}, ns={}, nr={}", config_.conn_name(), remoteSendSeq, remoteAckSeq);
    Stop();
    return;
  }

  handleAck(remoteAckSeq);
  if (closing_) {
    return;
  }

  if (remoteSendSeq != recvSeqExpected_) {
    LOG_ERROR("IEC104 发送序号异常: conn_name={}, 期望={}, 实际={}", config_.conn_name(), recvSeqExpected_, remoteSendSeq);
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

void TcpSession::handleSFrame(const std::vector<uint8_t> &apdu) {
  if (apdu.size() < 6) {
    LOG_DEBUG("IEC104 丢弃过短 S 帧: conn_name={}, 长度={}", config_.conn_name(), apdu.size());
    return;
  }
  auto remoteAckSeq = parseSeq(apdu, 4);
  LOG_DEBUG("IEC104 接收 S 帧: conn_name={}, nr={}", config_.conn_name(), remoteAckSeq);
  if (!dataTransferActive_) {
    LOG_WARNING("IEC104 未 STARTDT 即收到 S 帧: conn_name={}, nr={}", config_.conn_name(), remoteAckSeq);
    return;
  }
  handleAck(remoteAckSeq);
}

void TcpSession::handleUFrame(const std::vector<uint8_t> &apdu) {
  if (apdu.size() < 6) {
    LOG_DEBUG("IEC104 丢弃过短 U 帧: conn_name={}, 长度={}", config_.conn_name(), apdu.size());
    return;
  }
  auto type = static_cast<UFrameType>(apdu[2]);
  switch (type) {
  case UFrameType::STARTDT_ACT:
    LOG_INFO("IEC104 接收 STARTDT_ACT: conn_name={}", config_.conn_name());
    sendUFrame(UFrameType::STARTDT_CON);
    setDataTransferActive(true, "STARTDT_ACT");
    break;
  case UFrameType::STARTDT_CON:
    LOG_INFO("IEC104 接收 STARTDT_CON: conn_name={}", config_.conn_name());
    setDataTransferActive(true, "STARTDT_CON");
    break;
  case UFrameType::STOPDT_ACT:
    LOG_INFO("IEC104 接收 STOPDT_ACT: conn_name={}", config_.conn_name());
    sendUFrame(UFrameType::STOPDT_CON);
    setDataTransferActive(false, "STOPDT_ACT");
    break;
  case UFrameType::STOPDT_CON:
    LOG_INFO("IEC104 接收 STOPDT_CON: conn_name={}", config_.conn_name());
    setDataTransferActive(false, "STOPDT_CON");
    break;
  case UFrameType::TESTFR_ACT:
    LOG_DEBUG("IEC104 接收 TESTFR_ACT: conn_name={}", config_.conn_name());
    sendUFrame(UFrameType::TESTFR_CON);
    break;
  case UFrameType::TESTFR_CON:
    LOG_DEBUG("IEC104 接收 TESTFR_CON: conn_name={}", config_.conn_name());
    break;
  }
}

void TcpSession::processAsdu(const std::vector<uint8_t> &asdu) {
  if (asdu.size() < 6) {
    return;
  }
  auto typeId = asdu[0];
  switch (typeId) {
  case kTypeIdMeasuredValueShort:
    handleMeasuredValue(asdu, false);
    break;
  case kTypeIdMeasuredValueShortWithTime:
    handleMeasuredValue(asdu, true);
    break;
  case kTypeIdSinglePoint:
    handleSinglePoint(asdu, false);
    break;
  case kTypeIdSinglePointWithTime:
    handleSinglePoint(asdu, true);
    break;
  case kTypeIdSingleCommand:
    handleSingleCommand(asdu);
    break;
  case kTypeIdSetpointShort:
    handleSetpointCommand(asdu);
    break;
  case kTypeIdInterrogationCmd:
    handleInterrogation(asdu);
    break;
  case kTypeIdTimeSyncCmd:
    handleTimeSyncCommand(asdu);
    break;
  default:
    break;
  }
}

void TcpSession::handleMeasuredValue(const std::vector<uint8_t> &asdu, bool withTime) {
  const size_t minSize = withTime ? 12 + kCp56Time2aSize : 12;
  if (asdu.size() < minSize) {
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

    int64_t tsMs = 0;
    if (withTime) {
      if (asdu.size() < offset + kCp56Time2aSize) {
        return;
      }
      if (!decodeCp56Time2a(asdu.data() + offset, kCp56Time2aSize, &tsMs)) {
        LOG_WARNING("IEC104 遥测时标解析失败: conn_name={}, ioa={}", config_.conn_name(), ioa);
        tsMs = 0;
      }
      offset += kCp56Time2aSize;
    }

    if (onPointValue_) {
      PointValue mv;
      mv.ioa = ioa;
      mv.type = IEC104Proto::POINT_TYPE_FLOAT;
      mv.doubleValue = static_cast<double>(f);
      mv.quality = qds;
      mv.tsMs = tsMs;
      onPointValue_(mv);
    }
  }
}

void TcpSession::handleSinglePoint(const std::vector<uint8_t> &asdu, bool withTime) {
  const size_t minSize = withTime ? 10 + kCp56Time2aSize : 10;
  if (asdu.size() < minSize) {
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

    if (asdu.size() < offset + 1) {
      return;
    }
    auto siq = static_cast<uint8_t>(asdu[offset]);
    offset += 1;

    int64_t tsMs = 0;
    if (withTime) {
      if (asdu.size() < offset + kCp56Time2aSize) {
        return;
      }
      if (!decodeCp56Time2a(asdu.data() + offset, kCp56Time2aSize, &tsMs)) {
        LOG_WARNING("IEC104 单点时标解析失败: conn_name={}, ioa={}", config_.conn_name(), ioa);
        tsMs = 0;
      }
      offset += kCp56Time2aSize;
    }

    if (onPointValue_) {
      PointValue pv;
      pv.ioa = ioa;
      pv.type = IEC104Proto::POINT_TYPE_SINGLE;
      pv.boolValue = (siq & 0x01) != 0;
      pv.quality = siq;
      pv.tsMs = tsMs;
      onPointValue_(pv);
    }
  }
}

void TcpSession::handleSingleCommand(const std::vector<uint8_t> &asdu) {
  const size_t minSize = 6 + 3 + 1;
  if (asdu.size() < minSize) {
    return;
  }
  if (isMasterStation()) {
    LOG_INFO("IEC104 主站收到遥控命令，忽略: conn_name={}", config_.conn_name());
    return;
  }
  if (!dataTransferActive_) {
    LOG_WARNING("IEC104 未 STARTDT 即收到遥控命令: conn_name={}", config_.conn_name());
    return;
  }

  const auto cotRaw = static_cast<uint8_t>(asdu[2]);
  const auto cot = static_cast<uint8_t>(cotRaw & 0x3F);
  if (cot != kCotActivation) {
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

  const auto now = std::chrono::steady_clock::now();
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

    if (asdu.size() < offset + 1) {
      return;
    }
    const auto sco = static_cast<uint8_t>(asdu[offset]);
    offset += 1;

    const bool select = (sco & kScoSelectMask) != 0;
    const bool value = (sco & kScoValueMask) != 0;

    auto sendConfirm = [this, ioa, value, select](uint8_t cause, bool positive) {
      auto asdu = buildSingleCommandAsdu(ioa, value, select, cause, positive);
      if (asdu.empty()) {
        return;
      }
      enqueueAsdu(std::move(asdu));
    };

    if (select) {
      singleCommandSelectByIoa_[ioa] = SingleCommandSelect{value, now};
      LOG_INFO("IEC104 收到遥控预置: conn_name={}, ioa={}, value={}", config_.conn_name(), ioa, value);
      sendConfirm(kCotActivationCon, true);
      continue;
    }

    bool validSelect = false;
    auto it = singleCommandSelectByIoa_.find(ioa);
    if (it != singleCommandSelectByIoa_.end()) {
      const auto elapsed = now - it->second.time;
      if (elapsed > kSingleCommandSelectTimeout) {
        LOG_WARNING("IEC104 遥控预置已超时: conn_name={}, ioa={}", config_.conn_name(), ioa);
        singleCommandSelectByIoa_.erase(it);
      } else if (it->second.value != value) {
        LOG_WARNING("IEC104 遥控预置与执行值不一致: conn_name={}, ioa={}, 预置={}, 执行={}", config_.conn_name(), ioa, it->second.value, value);
        singleCommandSelectByIoa_.erase(it);
      } else {
        singleCommandSelectByIoa_.erase(it);
        validSelect = true;
      }
    }

    if (!validSelect) {
      LOG_WARNING("IEC104 遥控执行缺少预置: conn_name={}, ioa={}, value={}", config_.conn_name(), ioa, value);
      sendConfirm(kCotActivationCon, false);
      continue;
    }

    LOG_INFO("IEC104 收到遥控执行: conn_name={}, ioa={}, value={}", config_.conn_name(), ioa, value);
    if (onCommand_) {
      CommandValue cv;
      cv.ioa = ioa;
      cv.type = IEC104Proto::POINT_TYPE_SINGLE;
      cv.boolValue = value;
      onCommand_(cv);
    }
    sendConfirm(kCotActivationCon, true);
    sendConfirm(kCotActivationTermination, true);
  }
}

void TcpSession::handleSetpointCommand(const std::vector<uint8_t> &asdu) {
  const size_t minSize = 6 + 3 + 4 + 1;
  if (asdu.size() < minSize) {
    return;
  }
  if (isMasterStation()) {
    LOG_INFO("IEC104 主站收到设点命令，忽略: conn_name={}", config_.conn_name());
    return;
  }
  if (!dataTransferActive_) {
    LOG_WARNING("IEC104 未 STARTDT 即收到设点命令: conn_name={}", config_.conn_name());
    return;
  }

  const auto cotRaw = static_cast<uint8_t>(asdu[2]);
  const auto cot = static_cast<uint8_t>(cotRaw & 0x3F);
  if (cot != kCotActivation) {
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
    auto qos = static_cast<uint8_t>(asdu[offset + 4]);
    offset += 5;

    const bool select = (qos & kQosSelectMask) != 0;
    const double value = static_cast<double>(f);

    auto sendConfirm = [this, ioa, value, select](uint8_t cause, bool positive) {
      auto asdu = buildSetpointCommandAsdu(ioa, value, select, cause, positive);
      if (asdu.empty()) {
        return;
      }
      enqueueAsdu(std::move(asdu));
    };

    if (select) {
      LOG_INFO("IEC104 收到设点预置: conn_name={}, ioa={}, value={}", config_.conn_name(), ioa, value);
      sendConfirm(kCotActivationCon, true);
      continue;
    }

    LOG_INFO("IEC104 收到设点执行: conn_name={}, ioa={}, value={}", config_.conn_name(), ioa, value);
    if (onCommand_) {
      CommandValue cv;
      cv.ioa = ioa;
      cv.type = IEC104Proto::POINT_TYPE_FLOAT;
      cv.doubleValue = value;
      onCommand_(cv);
    }
    sendConfirm(kCotActivationCon, true);
    sendConfirm(kCotActivationTermination, true);
  }
}

void TcpSession::handleInterrogation(const std::vector<uint8_t> &asdu) {
  if (asdu.size() < 6 + 3 + 1) {
    return;
  }

  auto cot = static_cast<uint8_t>(asdu[2] & 0x3F);
  if (cot != kCotActivation) {
    return;
  }
  if (!dataTransferActive_) {
    LOG_WARNING("IEC104 未 STARTDT 即收到总召: conn_name={}", config_.conn_name());
    return;
  }

  auto qoi = static_cast<uint8_t>(asdu[6 + 3]);
  if (qoi == 0) {
    qoi = kQoiStation;
  }

  LOG_INFO("IEC104 接收总召激活: conn_name={}, qoi={}", config_.conn_name(), qoi);
  enqueueAsdu(buildInterrogationAsdu(kCotActivationCon, qoi));

  std::vector<PointValue> snapshot;
  if (interrogationSnapshotProvider_) {
    snapshot = interrogationSnapshotProvider_();
  }

  LOG_DEBUG("IEC104 总召快照: conn_name={}, 数量={}", config_.conn_name(), snapshot.size());
  if (!snapshot.empty() && !closing_ && dataTransferActive_) {
    enqueuePointValuesBatch(std::move(snapshot), kCotInterrogatedByStation);
  }

  enqueueAsdu(buildInterrogationAsdu(kCotActivationTermination, qoi));
}

void TcpSession::handleTimeSyncCommand(const std::vector<uint8_t> &asdu) {
  if (asdu.size() < kAsduHeaderSize + 3 + kCp56Time2aSize) {
    return;
  }

  auto cot = static_cast<uint8_t>(asdu[2] & 0x3F);
  if (cot != kCotActivation) {
    return;
  }
  if (!dataTransferActive_) {
    LOG_WARNING("IEC104 未 STARTDT 即收到对时: conn_name={}", config_.conn_name());
    return;
  }

  int64_t tsMs = 0;
  if (!decodeCp56Time2a(asdu.data() + kAsduHeaderSize + 3, kCp56Time2aSize, &tsMs)) {
    LOG_WARNING("IEC104 对时时标解析失败: conn_name={}", config_.conn_name());
    tsMs = 0;
  } else {
    LOG_INFO("IEC104 接收对时激活: conn_name={}, ts_ms={}", config_.conn_name(), tsMs);
  }

  if (onTimeSync_ && tsMs > 0) {
    onTimeSync_(tsMs);
  }

  auto actCon = buildTimeSyncAsdu(kCotActivationCon, tsMs);
  if (!actCon.empty()) {
    enqueueAsdu(std::move(actCon));
  }
  auto actTerm = buildTimeSyncAsdu(kCotActivationTermination, tsMs);
  if (!actTerm.empty()) {
    enqueueAsdu(std::move(actTerm));
  }
}

void TcpSession::enqueueAsdu(std::vector<uint8_t> asdu) {
  if (closing_) {
    return;
  }
  pendingAsdu_.emplace_back(std::move(asdu));
  LOG_DEBUG("IEC104 ASDU 入队: conn_name={}, 待处理={}, 激活={}", config_.conn_name(), pendingAsdu_.size(), dataTransferActive_);
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

void TcpSession::sendIFrame(const std::vector<uint8_t> &asdu) {
  if (closing_) {
    return;
  }
  if (!dataTransferActive_) {
    LOG_WARNING("IEC104 非激活状态发送 I 帧: conn_name={}", config_.conn_name());
    pendingAsdu_.emplace_front(asdu);
    return;
  }
  if (sendUnacked_ >= apci_.k) {
    LOG_DEBUG("IEC104 发送窗口已满，ASDU 入队: conn_name={}, k={}, 未确认={}", config_.conn_name(), apci_.k, sendUnacked_);
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
  LOG_DEBUG("IEC104 发送 I 帧: conn_name={}, ns={}, nr={}, 未确认={}", config_.conn_name(), sendSeq_, recvSeqExpected_, sendUnacked_ + 1);
  LOG_INFO("IEC104 报文发送: conn_name={}, 角色={}, 长度={}, 数据={}", config_.conn_name(), isClient_ ? "客户端" : "服务端", apdu.size(), bytesToHex(apdu));
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
  LOG_DEBUG("IEC104 发送 S 帧: conn_name={}, nr={}", config_.conn_name(), recvSeqExpected_);
  LOG_INFO("IEC104 报文发送: conn_name={}, 角色={}, 长度={}, 数据={}", config_.conn_name(), isClient_ ? "客户端" : "服务端", apdu.size(), bytesToHex(apdu));
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
  const char *typeName = "UNKNOWN";
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
  LOG_DEBUG("IEC104 发送 U 帧: conn_name={}, 类型={}", config_.conn_name(), typeName);
  LOG_INFO("IEC104 报文发送: conn_name={}, 角色={}, 长度={}, 数据={}", config_.conn_name(), isClient_ ? "客户端" : "服务端", apdu.size(), bytesToHex(apdu));
  enqueueWrite(std::move(apdu));
}

void TcpSession::enqueueWrite(std::vector<uint8_t> frame) {
  if (closing_) {
    return;
  }
  writeQueue_.emplace_back(std::move(frame));
  LOG_DEBUG("IEC104 写入队列: conn_name={}, 队列数={}", config_.conn_name(), writeQueue_.size());
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
      socket_, boost::asio::buffer(writeQueue_.front()), [self](const boost::system::error_code &ec, std::size_t) {
        self->writing_ = false;
        if (ec) {
          LOG_WARNING("IEC104 写入失败: conn_name={}, 错误={}", self->config_.conn_name(), ec.message());
          self->Stop();
          return;
        }
        self->writeQueue_.pop_front();
        self->doWrite();
      });
}

std::vector<uint8_t> TcpSession::buildMeasuredValueAsdu(
    uint32_t ioa, double value, uint8_t quality, uint8_t cause, int64_t tsMs, bool withTime) const {
  std::vector<uint8_t> asdu;
  const auto typeId = withTime ? kTypeIdMeasuredValueShortWithTime : kTypeIdMeasuredValueShort;
  asdu.reserve(kAsduHeaderSize + 3 + 4 + 1 + (withTime ? kCp56Time2aSize : 0));
  asdu.emplace_back(typeId);
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

  if (withTime) {
    std::array<uint8_t, kCp56Time2aSize> time{};
    if (!encodeCp56Time2a(tsMs, &time)) {
      return {};
    }
    asdu.insert(asdu.end(), time.begin(), time.end());
  }
  return asdu;
}

std::vector<uint8_t> TcpSession::buildSinglePointAsdu(
    uint32_t ioa, bool value, uint8_t quality, uint8_t cause, int64_t tsMs, bool withTime) const {
  std::vector<uint8_t> asdu;
  const auto typeId = withTime ? kTypeIdSinglePointWithTime : kTypeIdSinglePoint;
  asdu.reserve(kAsduHeaderSize + 3 + 1 + (withTime ? kCp56Time2aSize : 0));
  asdu.emplace_back(typeId);
  asdu.emplace_back(0x01);  // VSQ: 1 object, SQ=0
  asdu.emplace_back(cause & 0x3F);
  asdu.emplace_back(static_cast<uint8_t>(config_.oa() & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>(config_.ca() & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((config_.ca() >> 8) & 0xFF));

  asdu.emplace_back(static_cast<uint8_t>(ioa & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((ioa >> 8) & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((ioa >> 16) & 0xFF));

  auto siq = static_cast<uint8_t>(value ? 0x01 : 0x00);
  siq |= (quality & 0xFE);
  asdu.emplace_back(siq);

  if (withTime) {
    std::array<uint8_t, kCp56Time2aSize> time{};
    if (!encodeCp56Time2a(tsMs, &time)) {
      return {};
    }
    asdu.insert(asdu.end(), time.begin(), time.end());
  }
  return asdu;
}

std::vector<uint8_t> TcpSession::buildMeasuredValueAsduSq0(const std::vector<PointValue> &values, size_t start, size_t count, uint8_t cause, bool withTime) const {
  std::vector<uint8_t> asdu;
  if (count == 0) {
    return asdu;
  }
  const auto typeId = withTime ? kTypeIdMeasuredValueShortWithTime : kTypeIdMeasuredValueShort;
  const uint32_t objectSize = kMeasuredValueSq0ObjectSize + (withTime ? kCp56Time2aSize : 0);
  asdu.reserve(kAsduHeaderSize + objectSize * count);
  asdu.emplace_back(typeId);
  asdu.emplace_back(static_cast<uint8_t>(count & 0x7F));
  asdu.emplace_back(cause & 0x3F);
  asdu.emplace_back(static_cast<uint8_t>(config_.oa() & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>(config_.ca() & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((config_.ca() >> 8) & 0xFF));

  for (size_t i = 0; i < count; ++i) {
    const auto &mv = values[start + i];
    asdu.emplace_back(static_cast<uint8_t>(mv.ioa & 0xFF));
    asdu.emplace_back(static_cast<uint8_t>((mv.ioa >> 8) & 0xFF));
    asdu.emplace_back(static_cast<uint8_t>((mv.ioa >> 16) & 0xFF));

    float f = toFloat(mv.doubleValue);
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(f));
    asdu.emplace_back(static_cast<uint8_t>(bits & 0xFF));
    asdu.emplace_back(static_cast<uint8_t>((bits >> 8) & 0xFF));
    asdu.emplace_back(static_cast<uint8_t>((bits >> 16) & 0xFF));
    asdu.emplace_back(static_cast<uint8_t>((bits >> 24) & 0xFF));
    asdu.emplace_back(mv.quality);
    if (withTime) {
      std::array<uint8_t, kCp56Time2aSize> time{};
      if (!encodeCp56Time2a(mv.tsMs, &time)) {
        return {};
      }
      asdu.insert(asdu.end(), time.begin(), time.end());
    }
  }
  return asdu;
}

std::vector<uint8_t> TcpSession::buildMeasuredValueAsduSq1(const std::vector<PointValue> &values, size_t start, size_t count, uint8_t cause, bool withTime) const {
  std::vector<uint8_t> asdu;
  if (count == 0) {
    return asdu;
  }
  const auto typeId = withTime ? kTypeIdMeasuredValueShortWithTime : kTypeIdMeasuredValueShort;
  const uint32_t objectSize = kMeasuredValueSq1ObjectSize + (withTime ? kCp56Time2aSize : 0);
  asdu.reserve(kAsduHeaderSize + kMeasuredValueSq1BaseSize + objectSize * count);
  asdu.emplace_back(typeId);
  asdu.emplace_back(static_cast<uint8_t>(0x80 | (count & 0x7F)));
  asdu.emplace_back(cause & 0x3F);
  asdu.emplace_back(static_cast<uint8_t>(config_.oa() & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>(config_.ca() & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((config_.ca() >> 8) & 0xFF));

  const auto baseIoa = values[start].ioa;
  asdu.emplace_back(static_cast<uint8_t>(baseIoa & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((baseIoa >> 8) & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((baseIoa >> 16) & 0xFF));

  for (size_t i = 0; i < count; ++i) {
    const auto &mv = values[start + i];
    float f = toFloat(mv.doubleValue);
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(f));
    asdu.emplace_back(static_cast<uint8_t>(bits & 0xFF));
    asdu.emplace_back(static_cast<uint8_t>((bits >> 8) & 0xFF));
    asdu.emplace_back(static_cast<uint8_t>((bits >> 16) & 0xFF));
    asdu.emplace_back(static_cast<uint8_t>((bits >> 24) & 0xFF));
    asdu.emplace_back(mv.quality);
    if (withTime) {
      std::array<uint8_t, kCp56Time2aSize> time{};
      if (!encodeCp56Time2a(mv.tsMs, &time)) {
        return {};
      }
      asdu.insert(asdu.end(), time.begin(), time.end());
    }
  }
  return asdu;
}

std::vector<uint8_t> TcpSession::buildSinglePointAsduSq0(const std::vector<PointValue> &values, size_t start, size_t count, uint8_t cause, bool withTime) const {
  std::vector<uint8_t> asdu;
  if (count == 0) {
    return asdu;
  }
  const auto typeId = withTime ? kTypeIdSinglePointWithTime : kTypeIdSinglePoint;
  const uint32_t objectSize = kSinglePointSq0ObjectSize + (withTime ? kCp56Time2aSize : 0);
  asdu.reserve(kAsduHeaderSize + objectSize * count);
  asdu.emplace_back(typeId);
  asdu.emplace_back(static_cast<uint8_t>(count & 0x7F));
  asdu.emplace_back(cause & 0x3F);
  asdu.emplace_back(static_cast<uint8_t>(config_.oa() & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>(config_.ca() & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((config_.ca() >> 8) & 0xFF));

  for (size_t i = 0; i < count; ++i) {
    const auto &pv = values[start + i];
    asdu.emplace_back(static_cast<uint8_t>(pv.ioa & 0xFF));
    asdu.emplace_back(static_cast<uint8_t>((pv.ioa >> 8) & 0xFF));
    asdu.emplace_back(static_cast<uint8_t>((pv.ioa >> 16) & 0xFF));
    auto siq = static_cast<uint8_t>(pv.boolValue ? 0x01 : 0x00);
    siq |= (pv.quality & 0xFE);
    asdu.emplace_back(siq);
    if (withTime) {
      std::array<uint8_t, kCp56Time2aSize> time{};
      if (!encodeCp56Time2a(pv.tsMs, &time)) {
        return {};
      }
      asdu.insert(asdu.end(), time.begin(), time.end());
    }
  }
  return asdu;
}

std::vector<uint8_t> TcpSession::buildSinglePointAsduSq1(const std::vector<PointValue> &values, size_t start, size_t count, uint8_t cause, bool withTime) const {
  std::vector<uint8_t> asdu;
  if (count == 0) {
    return asdu;
  }
  const auto typeId = withTime ? kTypeIdSinglePointWithTime : kTypeIdSinglePoint;
  const uint32_t objectSize = kSinglePointSq1ObjectSize + (withTime ? kCp56Time2aSize : 0);
  asdu.reserve(kAsduHeaderSize + kMeasuredValueSq1BaseSize + objectSize * count);
  asdu.emplace_back(typeId);
  asdu.emplace_back(static_cast<uint8_t>(0x80 | (count & 0x7F)));
  asdu.emplace_back(cause & 0x3F);
  asdu.emplace_back(static_cast<uint8_t>(config_.oa() & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>(config_.ca() & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((config_.ca() >> 8) & 0xFF));

  const auto baseIoa = values[start].ioa;
  asdu.emplace_back(static_cast<uint8_t>(baseIoa & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((baseIoa >> 8) & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((baseIoa >> 16) & 0xFF));

  for (size_t i = 0; i < count; ++i) {
    const auto &pv = values[start + i];
    auto siq = static_cast<uint8_t>(pv.boolValue ? 0x01 : 0x00);
    siq |= (pv.quality & 0xFE);
    asdu.emplace_back(siq);
    if (withTime) {
      std::array<uint8_t, kCp56Time2aSize> time{};
      if (!encodeCp56Time2a(pv.tsMs, &time)) {
        return {};
      }
      asdu.insert(asdu.end(), time.begin(), time.end());
    }
  }
  return asdu;
}

std::vector<uint8_t> TcpSession::buildInterrogationAsdu(uint8_t cause, uint8_t qoi) const {
  std::vector<uint8_t> asdu;
  asdu.reserve(kAsduHeaderSize + 3 + 1);
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

std::vector<uint8_t> TcpSession::buildTimeSyncAsdu(uint8_t cause, int64_t tsMs) const {
  std::vector<uint8_t> asdu;
  asdu.reserve(kAsduHeaderSize + 3 + kCp56Time2aSize);
  asdu.emplace_back(kTypeIdTimeSyncCmd);
  asdu.emplace_back(0x01);
  asdu.emplace_back(cause & 0x3F);
  asdu.emplace_back(static_cast<uint8_t>(config_.oa() & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>(config_.ca() & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((config_.ca() >> 8) & 0xFF));

  asdu.emplace_back(0x00);
  asdu.emplace_back(0x00);
  asdu.emplace_back(0x00);

  std::array<uint8_t, kCp56Time2aSize> time{};
  if (!encodeCp56Time2a(tsMs, &time)) {
    return {};
  }
  asdu.insert(asdu.end(), time.begin(), time.end());
  return asdu;
}

std::vector<uint8_t> TcpSession::buildSingleCommandAsdu(uint32_t ioa, bool value, bool select, uint8_t cause, bool positive) const {
  std::vector<uint8_t> asdu;
  asdu.reserve(kAsduHeaderSize + 3 + 1);
  asdu.emplace_back(kTypeIdSingleCommand);
  asdu.emplace_back(0x01);
  asdu.emplace_back(buildCot(cause, positive));
  asdu.emplace_back(static_cast<uint8_t>(config_.oa() & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>(config_.ca() & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((config_.ca() >> 8) & 0xFF));

  asdu.emplace_back(static_cast<uint8_t>(ioa & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((ioa >> 8) & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((ioa >> 16) & 0xFF));

  uint8_t sco = value ? kScoValueMask : 0x00;
  if (select) {
    sco |= kScoSelectMask;
  }
  asdu.emplace_back(sco);
  return asdu;
}

std::vector<uint8_t> TcpSession::buildSetpointCommandAsdu(uint32_t ioa, double value, bool select, uint8_t cause, bool positive) const {
  std::vector<uint8_t> asdu;
  asdu.reserve(kAsduHeaderSize + 3 + 4 + 1);
  asdu.emplace_back(kTypeIdSetpointShort);
  asdu.emplace_back(0x01);
  asdu.emplace_back(buildCot(cause, positive));
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

  uint8_t qos = 0x00;
  if (select) {
    qos |= kQosSelectMask;
  }
  asdu.emplace_back(qos);
  return asdu;
}

bool TcpSession::encodeCp56Time2a(int64_t tsMs, std::array<uint8_t, kCp56Time2aSize> *out) {
  if (out == nullptr) {
    return false;
  }
  if (tsMs <= 0) {
    auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
    tsMs = now.time_since_epoch().count();
  }

  auto tp = std::chrono::system_clock::time_point(std::chrono::milliseconds(tsMs));
  std::time_t tt = std::chrono::system_clock::to_time_t(tp);
  std::tm tm{};
  if (localtime_r(&tt, &tm) == nullptr) {
    return false;
  }

  const int ms = static_cast<int>(tsMs % 1000);
  const int msec = tm.tm_sec * 1000 + (ms < 0 ? 0 : ms);
  if (msec < 0 || msec > 59999) {
    return false;
  }
  (*out)[0] = static_cast<uint8_t>(msec & 0xFF);
  (*out)[1] = static_cast<uint8_t>((msec >> 8) & 0xFF);

  (*out)[2] = static_cast<uint8_t>(tm.tm_min & 0x3F);
  uint8_t hour = static_cast<uint8_t>(tm.tm_hour & 0x1F);
  if (tm.tm_isdst > 0) {
    hour |= 0x80;
  }
  (*out)[3] = hour;

  const int dayOfWeek = (tm.tm_wday == 0) ? 7 : tm.tm_wday;
  (*out)[4] = static_cast<uint8_t>((tm.tm_mday & 0x1F) | ((dayOfWeek & 0x07) << 5));
  (*out)[5] = static_cast<uint8_t>((tm.tm_mon + 1) & 0x0F);
  const int year = (tm.tm_year + 1900) % 100;
  (*out)[6] = static_cast<uint8_t>(year & 0x7F);
  return true;
}

bool TcpSession::decodeCp56Time2a(const uint8_t *data, size_t size, int64_t *outMs) {
  if (data == nullptr || outMs == nullptr || size < kCp56Time2aSize) {
    return false;
  }
  const uint16_t msec = static_cast<uint16_t>(data[0] | (static_cast<uint16_t>(data[1]) << 8));
  if (msec > 59999) {
    return false;
  }
  const int minute = data[2] & 0x3F;
  const int hour = data[3] & 0x1F;
  const bool isDst = (data[3] & 0x80) != 0;
  const int day = data[4] & 0x1F;
  const int month = data[5] & 0x0F;
  const int year = data[6] & 0x7F;
  if (minute > 59 || hour > 23 || day <= 0 || day > 31 || month <= 0 || month > 12) {
    return false;
  }

  std::tm tm{};
  tm.tm_year = 2000 + year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = static_cast<int>(msec / 1000);
  tm.tm_isdst = isDst ? 1 : 0;
  std::time_t tt = std::mktime(&tm);
  if (tt == static_cast<std::time_t>(-1)) {
    return false;
  }
  const int ms = static_cast<int>(msec % 1000);
  *outMs = static_cast<int64_t>(tt) * 1000 + ms;
  return true;
}

uint16_t TcpSession::seqDistance(uint16_t from, uint16_t to) {
  return static_cast<uint16_t>((to + 32768 - from) % 32768);
}

void TcpSession::handleAck(uint16_t remoteAckSeq) {
  if (sendUnacked_ == 0) {
    if (remoteAckSeq != sendAckedSeq_) {
      LOG_ERROR("IEC104 无效确认: conn_name={}, 确认号={}, 期望={}", config_.conn_name(), remoteAckSeq, sendAckedSeq_);
      Stop();
    }
    return;
  }
  auto diff = seqDistance(sendAckedSeq_, remoteAckSeq);
  if (diff == 0) {
    return;
  }
  if (diff > sendUnacked_) {
    LOG_ERROR("IEC104 确认序号越界: conn_name={}, 确认号={}, 最旧={}, 未确认={}", config_.conn_name(), remoteAckSeq, sendAckedSeq_, sendUnacked_);
    Stop();
    return;
  }
  sendAckedSeq_ = remoteAckSeq;
  sendUnacked_ -= diff;
  LOG_DEBUG("IEC104 确认更新: conn_name={}, 已确认={}, 剩余={}", config_.conn_name(), diff, sendUnacked_);
  if (sendUnacked_ == 0) {
    stopT1();
  } else {
    startT1();
  }
  trySendPending();
}

void TcpSession::setDataTransferActive(bool active, const char *reason) {
  if (dataTransferActive_ == active) {
    return;
  }
  dataTransferActive_ = active;
  if (active) {
    LOG_INFO("IEC104 数据传输已激活: conn_name={}, 原因={}", config_.conn_name(), reason);
    stopT0();
    if (isMasterStation() && !autoInterrogationSent_) {
      sendAutoInterrogation(kQoiStation);
    }
    trySendPending();
    return;
  }

  LOG_INFO("IEC104 数据传输已停止: conn_name={}, 原因={}", config_.conn_name(), reason);
  stopT1();
  stopT2();
  ackPending_ = false;
  recvSinceLastAck_ = 0;
  sendUnacked_ = 0;
  sendAckedSeq_ = sendSeq_;
  pendingAsdu_.clear();
  clearPointQueue();
}

bool TcpSession::isMasterStation() const {
  if (config_.station_role() == IEC104Proto::STATION_ROLE_MASTER) {
    return true;
  }
  if (config_.station_role() == IEC104Proto::STATION_ROLE_SLAVE) {
    return false;
  }
  return config_.role() == IEC104Proto::ROLE_CLIENT;
}

void TcpSession::sendAutoInterrogation(uint8_t qoi) {
  if (!isMasterStation() || autoInterrogationSent_) {
    return;
  }
  autoInterrogationSent_ = true;
  LOG_INFO("IEC104 自动总召: conn_name={}, qoi={}", config_.conn_name(), qoi);
  enqueueAsdu(buildInterrogationAsdu(kCotActivation, qoi));
}

void TcpSession::sendTimeSync(int64_t tsMs) {
  if (!isMasterStation()) {
    LOG_WARNING("IEC104 非主站发送对时命令: conn_name={}", config_.conn_name());
    return;
  }
  if (!dataTransferActive_) {
    LOG_WARNING("IEC104 未激活状态发送对时命令: conn_name={}", config_.conn_name());
    return;
  }
  auto asdu = buildTimeSyncAsdu(kCotActivation, tsMs);
  if (asdu.empty()) {
    LOG_WARNING("IEC104 构造对时报文失败: conn_name={}", config_.conn_name());
    return;
  }
  LOG_INFO("IEC104 发送对时命令: conn_name={}, ts_ms={}", config_.conn_name(), tsMs);
  enqueueAsdu(std::move(asdu));
}

void TcpSession::sendSingleCommand(uint32_t ioa, bool value, bool select) {
  if (!isMasterStation()) {
    LOG_WARNING("IEC104 非主站发送遥控命令: conn_name={}", config_.conn_name());
    return;
  }
  if (!dataTransferActive_) {
    LOG_WARNING("IEC104 未激活状态发送遥控命令: conn_name={}", config_.conn_name());
    return;
  }
  auto asdu = buildSingleCommandAsdu(ioa, value, select, kCotActivation, true);
  if (asdu.empty()) {
    LOG_WARNING("IEC104 构造遥控命令失败: conn_name={}, ioa={}", config_.conn_name(), ioa);
    return;
  }
  LOG_INFO("IEC104 发送遥控命令: conn_name={}, ioa={}, value={}, select={}", config_.conn_name(), ioa, value, select);
  enqueueAsdu(std::move(asdu));
}

void TcpSession::sendSetpointCommand(uint32_t ioa, double value) {
  if (!isMasterStation()) {
    LOG_WARNING("IEC104 非主站发送设点命令: conn_name={}", config_.conn_name());
    return;
  }
  if (!dataTransferActive_) {
    LOG_WARNING("IEC104 未激活状态发送设点命令: conn_name={}", config_.conn_name());
    return;
  }
  if (!std::isfinite(value)) {
    LOG_WARNING("IEC104 设点值非法: conn_name={}, ioa={}, value={}", config_.conn_name(), ioa, value);
    return;
  }
  auto asdu = buildSetpointCommandAsdu(ioa, value, false, kCotActivation, true);
  if (asdu.empty()) {
    LOG_WARNING("IEC104 构造设点命令失败: conn_name={}, ioa={}", config_.conn_name(), ioa);
    return;
  }
  LOG_INFO("IEC104 发送设点命令: conn_name={}, ioa={}, value={}", config_.conn_name(), ioa, value);
  enqueueAsdu(std::move(asdu));
}

void TcpSession::startT0() {
  if (apci_.t0 <= 0) {
    return;
  }
  t0Timer_.expires_after(std::chrono::seconds(apci_.t0));
  auto self = shared_from_this();
  t0Timer_.async_wait([self](const boost::system::error_code &ec) { self->onT0Timeout(ec); });
  LOG_DEBUG("IEC104 启动 t0: conn_name={}, t0={}", config_.conn_name(), apci_.t0);
}

void TcpSession::stopT0() {
  t0Timer_.cancel();
}

void TcpSession::onT0Timeout(const boost::system::error_code &ec) {
  if (ec) {
    return;
  }
  if (closing_ || dataTransferActive_) {
    return;
  }
  LOG_WARNING("IEC104 t0 超时: conn_name={}, t0={}", config_.conn_name(), apci_.t0);
  Stop();
}

void TcpSession::startT1() {
  if (apci_.t1 <= 0 || sendUnacked_ == 0) {
    return;
  }
  t1Timer_.expires_after(std::chrono::seconds(apci_.t1));
  auto self = shared_from_this();
  t1Timer_.async_wait([self](const boost::system::error_code &ec) { self->onT1Timeout(ec); });
  LOG_DEBUG("IEC104 启动 t1: conn_name={}, t1={}, 未确认={}", config_.conn_name(), apci_.t1, sendUnacked_);
}

void TcpSession::stopT1() {
  t1Timer_.cancel();
}

void TcpSession::onT1Timeout(const boost::system::error_code &ec) {
  if (ec) {
    return;
  }
  if (closing_ || sendUnacked_ == 0) {
    return;
  }
  LOG_WARNING("IEC104 t1 超时: conn_name={}, t1={}, 未确认={}", config_.conn_name(), apci_.t1, sendUnacked_);
  Stop();
}

void TcpSession::startT2() {
  if (apci_.t2 <= 0 || !ackPending_) {
    return;
  }
  t2Timer_.expires_after(std::chrono::seconds(apci_.t2));
  auto self = shared_from_this();
  t2Timer_.async_wait([self](const boost::system::error_code &ec) { self->onT2Timeout(ec); });
  LOG_DEBUG("IEC104 启动 t2: conn_name={}, t2={}, 待确认={}", config_.conn_name(), apci_.t2, recvSinceLastAck_);
}

void TcpSession::stopT2() {
  t2Timer_.cancel();
}

void TcpSession::onT2Timeout(const boost::system::error_code &ec) {
  if (ec) {
    return;
  }
  if (closing_ || !ackPending_) {
    return;
  }
  LOG_DEBUG("IEC104 t2 超时: conn_name={}, 待确认={}", config_.conn_name(), recvSinceLastAck_);
  sendSFrame();
}

TcpSession::Apci TcpSession::parseApci(const IEC104Proto::APCIParameters &in) {
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
  t3Timer_.async_wait([self](const boost::system::error_code &ec) { self->onT3Timeout(ec); });
}

void TcpSession::onT3Timeout(const boost::system::error_code &ec) {
  if (ec) {
    return;
  }
  if (closing_) {
    return;
  }
  if (dataTransferActive_) {
    LOG_DEBUG("IEC104 t3 超时: conn_name={}, t3={}", config_.conn_name(), apci_.t3);
    sendUFrame(UFrameType::TESTFR_ACT);
  }
  restartT3();
}

}  // namespace IEC104
