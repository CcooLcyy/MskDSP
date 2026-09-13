#include <gtest/gtest.h>

#include <array>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <poll.h>
#include <string>
#include <utility>

#include "IEC104TcpLink.h"

namespace {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

constexpr uint8_t kApduStart = 0x68;
constexpr uint8_t kStartDtAct = 0x07;
constexpr uint8_t kStartDtCon = 0x0B;
constexpr uint8_t kTestFrAct = 0x43;

uint16_t ReserveLoopbackPort() {
  asio::io_context io;
  tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
  return acceptor.local_endpoint().port();
}

IEC104Proto::LinkConfig MakeServerConfig(const std::string &remoteIp) {
  IEC104Proto::LinkConfig config;
  config.set_conn_name("tcp-whitelist");
  config.set_role(IEC104Proto::ROLE_SERVER);
  config.mutable_local()->set_ip("127.0.0.1");
  config.mutable_local()->set_port(ReserveLoopbackPort());
  config.mutable_remote()->set_ip(remoteIp);
  config.mutable_apci()->set_t0(2);
  config.mutable_apci()->set_t1(2);
  config.mutable_apci()->set_t2(1);
  config.mutable_apci()->set_t3(5);
  return config;
}

struct ClientSocket {
  std::shared_ptr<asio::io_context> io;
  tcp::socket socket;
};

ClientSocket ConnectLoopback(uint16_t port, const asio::ip::address_v4 &source = asio::ip::address_v4::loopback()) {
  auto io = std::make_shared<asio::io_context>();
  tcp::socket socket(*io);
  socket.open(tcp::v4());
  socket.bind(tcp::endpoint(source, 0));
  socket.connect(tcp::endpoint(asio::ip::address_v4::loopback(), port));
  return {std::move(io), std::move(socket)};
}

bool WaitReadable(tcp::socket &socket, std::chrono::milliseconds timeout) {
  pollfd pfd{};
  pfd.fd = socket.native_handle();
  pfd.events = POLLIN;
  const auto result = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
  return result > 0 && (pfd.revents & POLLIN) != 0;
}

bool ReadStartDtCon(tcp::socket &socket) {
  if (!WaitReadable(socket, std::chrono::milliseconds(2000))) {
    return false;
  }

  std::array<uint8_t, 6> frame{};
  boost::system::error_code ec;
  asio::read(socket, asio::buffer(frame), ec);
  return !ec && frame[0] == kApduStart && frame[1] == 0x04 && frame[2] == kStartDtCon && frame[3] == 0x00 &&
         frame[4] == 0x00 && frame[5] == 0x00;
}

bool WaitForPeerClose(tcp::socket &socket, std::chrono::milliseconds timeout) {
  pollfd pfd{};
  pfd.fd = socket.native_handle();
  pfd.events = POLLIN | POLLHUP | POLLERR;
  const auto result = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
  if (result <= 0 || (pfd.revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
    return false;
  }

  std::array<uint8_t, 1> byte{};
  boost::system::error_code ec;
  const auto bytes = socket.read_some(asio::buffer(byte), ec);
  return bytes == 0 && (ec == asio::error::eof || ec == asio::error::connection_reset ||
                        ec == asio::error::operation_aborted);
}

void SendStartDtAct(tcp::socket &socket) {
  const std::array<uint8_t, 6> frame = {kApduStart, 0x04, kStartDtAct, 0x00, 0x00, 0x00};
  boost::system::error_code ec;
  asio::write(socket, asio::buffer(frame), ec);
  ASSERT_FALSE(ec) << ec.message();
}

}  // namespace

// 验证服务端 TcpLink 在无会话、建立会话和对端断开时报告连接状态。
TEST(IEC104TcpLinkTest, ServerConnectionStateTracksAcceptedSession) {
  auto config = MakeServerConfig("");
  IEC104::TcpLink link(config);
  std::promise<IEC104Proto::ConnectionState> connectedPromise;
  std::promise<IEC104Proto::ConnectionState> disconnectedPromise;
  link.SetConnectionStateCallback([&](IEC104Proto::ConnectionState state) {
    if (state == IEC104Proto::CONNECTION_STATE_CONNECTED) {
      try {
        connectedPromise.set_value(state);
      } catch (const std::future_error&) {
      }
    } else if (state == IEC104Proto::CONNECTION_STATE_DISCONNECTED) {
      try {
        disconnectedPromise.set_value(state);
      } catch (const std::future_error&) {
      }
    }
  });

  ASSERT_TRUE(link.Start().ok());
  EXPECT_EQ(link.ConnectionState(), IEC104Proto::CONNECTION_STATE_DISCONNECTED);

  auto client = ConnectLoopback(static_cast<uint16_t>(config.local().port()));
  SendStartDtAct(client.socket);
  EXPECT_TRUE(ReadStartDtCon(client.socket));

  auto connected = connectedPromise.get_future();
  ASSERT_EQ(connected.wait_for(std::chrono::milliseconds(1000)), std::future_status::ready);
  EXPECT_EQ(connected.get(), IEC104Proto::CONNECTION_STATE_CONNECTED);
  EXPECT_EQ(link.ConnectionState(), IEC104Proto::CONNECTION_STATE_CONNECTED);

  boost::system::error_code ec;
  client.socket.shutdown(tcp::socket::shutdown_both, ec);
  client.socket.close(ec);
  auto disconnected = disconnectedPromise.get_future();
  ASSERT_EQ(disconnected.wait_for(std::chrono::milliseconds(1000)), std::future_status::ready);
  EXPECT_EQ(disconnected.get(), IEC104Proto::CONNECTION_STATE_DISCONNECTED);
  EXPECT_EQ(link.ConnectionState(), IEC104Proto::CONNECTION_STATE_DISCONNECTED);
  link.Stop();
}

// 验证服务端接受连接后，IEC104 会话会按 t3 发送 TESTFR_ACT；该行为属于连接存活检测。
TEST(IEC104TcpLinkTest, ServerSessionSendsTestFrameForT3Keepalive) {
  auto config = MakeServerConfig("");
  config.mutable_apci()->set_t3(1);
  IEC104::TcpLink link(config);
  ASSERT_TRUE(link.Start().ok());

  auto client = ConnectLoopback(static_cast<uint16_t>(config.local().port()));
  SendStartDtAct(client.socket);
  EXPECT_TRUE(ReadStartDtCon(client.socket));
  ASSERT_TRUE(WaitReadable(client.socket, std::chrono::milliseconds(2500)));

  std::array<uint8_t, 6> frame{};
  boost::system::error_code ec;
  asio::read(client.socket, asio::buffer(frame), ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(frame[0], kApduStart);
  EXPECT_EQ(frame[2], kTestFrAct);

  link.Stop();
}

// 验证 ROLE_SERVER 仅接受来源地址命中 remote.ip 白名单的 TCP client。
TEST(IEC104TcpLinkTest, ServerAcceptsWhitelistedClientIp) {
  auto config = MakeServerConfig("127.0.0.1");
  config.mutable_remote()->set_port(1);
  IEC104::TcpLink link(config);
  ASSERT_TRUE(link.Start().ok());

  auto client = ConnectLoopback(static_cast<uint16_t>(config.local().port()));
  SendStartDtAct(client.socket);
  EXPECT_TRUE(ReadStartDtCon(client.socket));

  link.Stop();
}

// 验证 ROLE_SERVER 会关闭非白名单 client，并在拒绝后继续接受白名单来源。
TEST(IEC104TcpLinkTest, ServerRejectsNonWhitelistedClientIp) {
  auto config = MakeServerConfig("127.0.0.2");
  IEC104::TcpLink link(config);
  ASSERT_TRUE(link.Start().ok());

  auto client = ConnectLoopback(static_cast<uint16_t>(config.local().port()));
  EXPECT_TRUE(WaitForPeerClose(client.socket, std::chrono::milliseconds(2000)));

  auto allowedClient = ConnectLoopback(static_cast<uint16_t>(config.local().port()),
                                       asio::ip::make_address_v4("127.0.0.2"));
  SendStartDtAct(allowedClient.socket);
  EXPECT_TRUE(ReadStartDtCon(allowedClient.socket));

  link.Stop();
}

// 验证 ROLE_SERVER 的 remote.ip 为空或未指定地址时保持兼容，允许任意来源连接。
TEST(IEC104TcpLinkTest, ServerAllowsClientWhenWhitelistIsDisabled) {
  for (const auto &remoteIp : {"", "0.0.0.0", "::"}) {
    auto config = MakeServerConfig(remoteIp);
    IEC104::TcpLink link(config);
    ASSERT_TRUE(link.Start().ok());

    auto client = ConnectLoopback(static_cast<uint16_t>(config.local().port()));
    SendStartDtAct(client.socket);
    EXPECT_TRUE(ReadStartDtCon(client.socket));

    link.Stop();
  }
}
