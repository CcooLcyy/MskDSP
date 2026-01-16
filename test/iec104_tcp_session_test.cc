#include <gtest/gtest.h>

#include <array>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <future>
#include <memory>
#include <poll.h>
#include <string>
#include <thread>
#include <vector>

#include "IEC104TcpSession.h"

namespace {
using tcp = boost::asio::ip::tcp;

constexpr uint8_t kApduStart = 0x68;
constexpr uint8_t kUStartDtAct = 0x07;
constexpr uint8_t kUStartDtCon = 0x0B;
constexpr uint8_t kTypeIdInterrogationCmd = 100;
constexpr uint8_t kCotActivation = 6;
constexpr uint8_t kQoiStation = 20;

enum class FrameType { I, S, U };

bool WaitReadable(tcp::socket& socket, std::chrono::milliseconds timeout) {
  pollfd pfd{};
  pfd.fd = socket.native_handle();
  pfd.events = POLLIN;
  auto rc = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
  return rc > 0 && (pfd.revents & POLLIN);
}

std::vector<uint8_t> ReadApduWithTimeout(tcp::socket& socket,
                                         std::chrono::milliseconds timeout,
                                         const char* label) {
  if (!WaitReadable(socket, timeout)) {
    ADD_FAILURE() << label << " timed out after " << timeout.count() << "ms";
    return {};
  }

  std::array<uint8_t, 2> header{};
  boost::system::error_code ec;
  boost::asio::read(socket, boost::asio::buffer(header), ec);
  if (ec) {
    ADD_FAILURE() << label << " read header failed: " << ec.message();
    return {};
  }
  if (header[0] != kApduStart) {
    ADD_FAILURE() << label << " invalid start byte: " << static_cast<int>(header[0]);
    return {};
  }

  auto payload_len = static_cast<size_t>(header[1]);
  std::vector<uint8_t> apdu(2 + payload_len);
  apdu[0] = header[0];
  apdu[1] = header[1];
  if (payload_len > 0) {
    boost::asio::read(socket, boost::asio::buffer(apdu.data() + 2, payload_len), ec);
    if (ec) {
      ADD_FAILURE() << label << " read payload failed: " << ec.message();
      return {};
    }
  }
  return apdu;
}

FrameType FrameTypeOf(const std::vector<uint8_t>& apdu) {
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

uint16_t ParseSeq(const std::vector<uint8_t>& apdu, size_t offset) {
  return static_cast<uint16_t>((static_cast<uint16_t>(apdu.at(offset + 1)) << 7) | (apdu.at(offset) >> 1));
}

void WriteSeq(std::vector<uint8_t>* out, size_t offset, uint16_t seq) {
  out->at(offset) = static_cast<uint8_t>((seq << 1) & 0xFF);
  out->at(offset + 1) = static_cast<uint8_t>((seq >> 7) & 0xFF);
}

std::vector<uint8_t> BuildUFrame(uint8_t type) {
  std::vector<uint8_t> apdu(6);
  apdu[0] = kApduStart;
  apdu[1] = 0x04;
  apdu[2] = type;
  apdu[3] = 0x00;
  apdu[4] = 0x00;
  apdu[5] = 0x00;
  return apdu;
}

std::vector<uint8_t> BuildIFrame(uint16_t send_seq, uint16_t recv_seq, const std::vector<uint8_t>& asdu) {
  std::vector<uint8_t> apdu;
  apdu.resize(6);
  apdu[0] = kApduStart;
  apdu[1] = static_cast<uint8_t>(4 + asdu.size());
  WriteSeq(&apdu, 2, send_seq);
  WriteSeq(&apdu, 4, recv_seq);
  apdu.insert(apdu.end(), asdu.begin(), asdu.end());
  return apdu;
}

struct SocketPair {
  tcp::socket session_socket;
  tcp::socket peer_socket;
};

SocketPair MakeConnectedSockets(boost::asio::io_context& session_io) {
  boost::asio::io_context peer_io;
  tcp::acceptor acceptor(peer_io, tcp::endpoint(tcp::v4(), 0));
  auto port = acceptor.local_endpoint().port();

  tcp::socket peer_socket(peer_io);
  std::promise<boost::system::error_code> accept_result;
  std::thread accept_thread([&]() {
    boost::system::error_code ec;
    acceptor.accept(peer_socket, ec);
    accept_result.set_value(ec);
  });

  tcp::socket session_socket(session_io);
  session_socket.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), port));

  auto ec = accept_result.get_future().get();
  accept_thread.join();
  EXPECT_EQ(ec.value(), 0);

  return {std::move(session_socket), std::move(peer_socket)};
}

IEC104Proto::LinkConfig MakeConfig(const std::string& name,
                                   IEC104Proto::Role role,
                                   int t0,
                                   int t1,
                                   int t2,
                                   int t3,
                                   uint16_t w) {
  IEC104Proto::LinkConfig config;
  config.set_conn_name(name);
  config.set_role(role);
  config.set_ca(1);
  config.set_oa(1);
  auto* apci = config.mutable_apci();
  apci->set_k(12);
  apci->set_w(w);
  apci->set_t0(t0);
  apci->set_t1(t1);
  apci->set_t2(t2);
  apci->set_t3(t3);
  return config;
}
}  // namespace

// Verifies client sends interrogation after STARTDT confirmation.
TEST(IEC104TcpSessionTest, ClientAutoInterrogationAfterStartDt) {
  auto io = std::make_shared<boost::asio::io_context>();
  auto sockets = MakeConnectedSockets(*io);

  auto config = MakeConfig("auto-interrogation", IEC104Proto::ROLE_CLIENT, 2, 2, 1, 5, 8);
  auto session = std::make_shared<IEC104::TcpSession>(*io, config, true);
  session->Start(std::move(sockets.session_socket));

  std::jthread session_thread([&]() { io->run(); });

  auto start_act = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "STARTDT_ACT");
  ASSERT_FALSE(start_act.empty());
  EXPECT_EQ(FrameTypeOf(start_act), FrameType::U);
  EXPECT_EQ(start_act[2], kUStartDtAct);

  auto start_con = BuildUFrame(kUStartDtCon);
  boost::asio::write(sockets.peer_socket, boost::asio::buffer(start_con));

  auto interrogation = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "INTERROGATION");
  ASSERT_FALSE(interrogation.empty());
  ASSERT_EQ(FrameTypeOf(interrogation), FrameType::I);
  ASSERT_GT(interrogation.size(), 6u);
  EXPECT_EQ(interrogation[6], kTypeIdInterrogationCmd);
  EXPECT_EQ(static_cast<uint8_t>(interrogation[8] & 0x3F), kCotActivation);
  EXPECT_EQ(interrogation.back(), kQoiStation);

  session->Stop();
  io->stop();
}

// Verifies delayed S-frame acknowledgement is triggered by t2 timeout.
TEST(IEC104TcpSessionTest, DelayedAckUsesT2Timer) {
  auto io = std::make_shared<boost::asio::io_context>();
  auto sockets = MakeConnectedSockets(*io);

  auto config = MakeConfig("t2-ack", IEC104Proto::ROLE_SERVER, 2, 2, 1, 5, 2);
  auto session = std::make_shared<IEC104::TcpSession>(*io, config, false);
  session->Start(std::move(sockets.session_socket));

  std::jthread session_thread([&]() { io->run(); });

  auto start_act = BuildUFrame(kUStartDtAct);
  boost::asio::write(sockets.peer_socket, boost::asio::buffer(start_act));

  auto start_con = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "STARTDT_CON");
  ASSERT_FALSE(start_con.empty());
  EXPECT_EQ(FrameTypeOf(start_con), FrameType::U);
  EXPECT_EQ(start_con[2], kUStartDtCon);

  const std::vector<uint8_t> dummy_asdu = {0xFF, 0x01, 0x00, 0x00, 0x00, 0x00};
  auto i_frame = BuildIFrame(0, 0, dummy_asdu);
  boost::asio::write(sockets.peer_socket, boost::asio::buffer(i_frame));

  EXPECT_FALSE(WaitReadable(sockets.peer_socket, std::chrono::milliseconds(200)));

  auto s_frame = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "S_FRAME");
  ASSERT_FALSE(s_frame.empty());
  EXPECT_EQ(FrameTypeOf(s_frame), FrameType::S);
  EXPECT_EQ(ParseSeq(s_frame, 4), 1);

  session->Stop();
  io->stop();
}
