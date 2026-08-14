#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <poll.h>
#include <string>
#include <thread>
#include <vector>

#include "IEC104LibInfo.h"
#include "IEC104TcpSession.h"
#include "ThreadUtil.hpp"

namespace {
using tcp = boost::asio::ip::tcp;

constexpr uint8_t kApduStart = 0x68;
constexpr uint8_t kUStartDtAct = 0x07;
constexpr uint8_t kUStartDtCon = 0x0B;
constexpr uint8_t kTypeIdSinglePoint = 1;
constexpr uint8_t kTypeIdSinglePointWithTime = 30;
constexpr uint8_t kTypeIdMeasuredValueShort = 13;
constexpr uint8_t kTypeIdMeasuredValueShortWithTime = 36;
constexpr uint8_t kTypeIdInterrogationCmd = 100;
constexpr uint8_t kTypeIdSingleCommand = 45;
constexpr uint8_t kTypeIdSetpointShort = 50;
constexpr uint8_t kCotActivation = 6;
constexpr uint8_t kCotActivationCon = 7;
constexpr uint8_t kCotActivationTermination = 10;
constexpr uint8_t kCotInterrogatedByStation = 20;
constexpr uint8_t kCotNegative = 0x40;
constexpr uint8_t kCotSpontaneous = 3;
constexpr uint8_t kQoiStation = 20;
constexpr uint8_t kScoSelectMask = 0x80;
constexpr uint8_t kScoValueMask = 0x01;
constexpr uint8_t kQosSelectMask = 0x80;

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

std::vector<uint8_t> BuildSingleCommandAsdu(uint32_t ioa, bool value, bool select, uint8_t cause) {
  std::vector<uint8_t> asdu;
  asdu.reserve(10);
  asdu.emplace_back(kTypeIdSingleCommand);
  asdu.emplace_back(0x01);
  asdu.emplace_back(static_cast<uint8_t>(cause & 0x3F));
  asdu.emplace_back(0x00);
  asdu.emplace_back(0x01);
  asdu.emplace_back(0x00);
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

std::vector<uint8_t> BuildSetpointCommandAsdu(uint32_t ioa, float value, bool select, uint8_t cause) {
  std::vector<uint8_t> asdu;
  asdu.reserve(14);
  asdu.emplace_back(kTypeIdSetpointShort);
  asdu.emplace_back(0x01);
  asdu.emplace_back(static_cast<uint8_t>(cause & 0x3F));
  asdu.emplace_back(0x00);
  asdu.emplace_back(0x01);
  asdu.emplace_back(0x00);
  asdu.emplace_back(static_cast<uint8_t>(ioa & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((ioa >> 8) & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((ioa >> 16) & 0xFF));
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(value));
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

std::vector<uint8_t> BuildInterrogationAsdu(uint8_t cause, uint8_t qoi) {
  return {kTypeIdInterrogationCmd,
          0x01,
          static_cast<uint8_t>(cause & 0x3F),
          0x01,
          0x01,
          0x00,
          0x00,
          0x00,
          0x00,
          qoi};
}

struct SocketPair {
  std::shared_ptr<boost::asio::io_context> peer_io;
  tcp::socket session_socket;
  tcp::socket peer_socket;
};

SocketPair MakeConnectedSockets(boost::asio::io_context& session_io) {
  auto peer_io = std::make_shared<boost::asio::io_context>();
  tcp::acceptor acceptor(*peer_io, tcp::endpoint(tcp::v4(), 0));
  auto port = acceptor.local_endpoint().port();

  tcp::socket peer_socket(*peer_io);
  std::promise<boost::system::error_code> accept_result;
  std::jthread accept_thread = ModuleManager::StartModuleThread(
      IEC104LibInfo.LIB_NAME,
      [&]() {
        boost::system::error_code ec;
        acceptor.accept(peer_socket, ec);
        accept_result.set_value(ec);
      });

  tcp::socket session_socket(session_io);
  session_socket.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), port));

  auto ec = accept_result.get_future().get();
  accept_thread.join();
  EXPECT_EQ(ec.value(), 0);

  return {std::move(peer_io), std::move(session_socket), std::move(peer_socket)};
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
}  // 命名空间结束

// 验证：客户端在收到 STARTDT 确认后会自动发送总召。
TEST(IEC104TcpSessionTest, ClientAutoInterrogationAfterStartDt) {
  auto io = std::make_shared<boost::asio::io_context>();
  auto sockets = MakeConnectedSockets(*io);

  auto config = MakeConfig("auto-interrogation", IEC104Proto::ROLE_CLIENT, 2, 2, 1, 5, 8);
  auto session = std::make_shared<IEC104::TcpSession>(*io, config, true);
  session->Start(std::move(sockets.session_socket));

  std::jthread session_thread = ModuleManager::StartModuleThread(
      IEC104LibInfo.LIB_NAME,
      [&]() { io->run(); });

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

// 验证总召快照包含混合点类型时，会按类型分别编码为单点和短浮点报文。
TEST(IEC104TcpSessionTest, InterrogationSnapshotWithMixedPointTypes) {
  auto io = std::make_shared<boost::asio::io_context>();
  auto sockets = MakeConnectedSockets(*io);

  auto config = MakeConfig("interrogation-mixed-types", IEC104Proto::ROLE_SERVER, 2, 2, 1, 5, 8);
  config.set_point_with_time(false);
  auto session = std::make_shared<IEC104::TcpSession>(*io, config, false);

  session->SetInterrogationSnapshotProvider([] {
    IEC104::PointValue single;
    single.ioa = 200;
    single.type = IEC104Proto::POINT_TYPE_SINGLE;
    single.boolValue = true;

    IEC104::PointValue measured;
    measured.ioa = 100;
    measured.type = IEC104Proto::POINT_TYPE_FLOAT;
    measured.doubleValue = 12.5;

    // 首个点故意使用单点，覆盖按首点类型编码整批快照的回归场景。
    return std::vector<IEC104::PointValue>{single, measured};
  });
  session->Start(std::move(sockets.session_socket));

  std::jthread session_thread = ModuleManager::StartModuleThread(
      IEC104LibInfo.LIB_NAME,
      [&]() { io->run(); });

  auto start_act = BuildUFrame(kUStartDtAct);
  boost::asio::write(sockets.peer_socket, boost::asio::buffer(start_act));

  auto start_con = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "总召启动确认");
  ASSERT_FALSE(start_con.empty());
  ASSERT_EQ(FrameTypeOf(start_con), FrameType::U);
  EXPECT_EQ(start_con[2], kUStartDtCon);

  auto interrogation = BuildIFrame(0, 0, BuildInterrogationAsdu(kCotActivation, kQoiStation));
  boost::asio::write(sockets.peer_socket, boost::asio::buffer(interrogation));

  bool foundConfirmation = false;
  bool foundSingle = false;
  bool foundMeasured = false;
  bool foundTermination = false;
  for (int i = 0; i < 4; ++i) {
    auto apdu = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "总召响应");
    ASSERT_FALSE(apdu.empty());
    ASSERT_EQ(FrameTypeOf(apdu), FrameType::I);
    ASSERT_GT(apdu.size(), 7u);

    const auto typeId = apdu[6];
    const auto cause = static_cast<uint8_t>(apdu[8] & 0x3F);
    const auto count = static_cast<uint8_t>(apdu[7] & 0x7F);
    if (typeId == kTypeIdInterrogationCmd && cause == kCotActivationCon) {
      EXPECT_EQ(count, 1);
      EXPECT_EQ(apdu.back(), kQoiStation);
      foundConfirmation = true;
    } else if (typeId == kTypeIdSinglePoint) {
      foundSingle = true;
      EXPECT_EQ(cause, kCotInterrogatedByStation);
      EXPECT_EQ(count, 1);
      ASSERT_GE(apdu.size(), 16u);
      EXPECT_EQ(static_cast<uint32_t>(apdu[12]) |
                    (static_cast<uint32_t>(apdu[13]) << 8) |
                    (static_cast<uint32_t>(apdu[14]) << 16),
                200u);
      EXPECT_EQ(apdu[15] & 0x01, 0x01);
    } else if (typeId == kTypeIdMeasuredValueShort) {
      foundMeasured = true;
      EXPECT_EQ(cause, kCotInterrogatedByStation);
      EXPECT_EQ(count, 1);
      ASSERT_GE(apdu.size(), 20u);
      EXPECT_EQ(static_cast<uint32_t>(apdu[12]) |
                    (static_cast<uint32_t>(apdu[13]) << 8) |
                    (static_cast<uint32_t>(apdu[14]) << 16),
                100u);
      uint32_t bits = static_cast<uint32_t>(apdu[15]) |
          (static_cast<uint32_t>(apdu[16]) << 8) |
          (static_cast<uint32_t>(apdu[17]) << 16) |
          (static_cast<uint32_t>(apdu[18]) << 24);
      float value = 0.0F;
      std::memcpy(&value, &bits, sizeof(value));
      EXPECT_FLOAT_EQ(value, 12.5F);
      EXPECT_EQ(apdu[19], 0x00);
    } else if (typeId == kTypeIdInterrogationCmd) {
      EXPECT_EQ(cause, kCotActivationTermination);
      EXPECT_EQ(apdu.back(), kQoiStation);
      foundTermination = true;
    } else {
      ADD_FAILURE() << "总召响应出现未预期类型: " << static_cast<int>(typeId);
    }
  }

  EXPECT_TRUE(foundConfirmation);
  EXPECT_TRUE(foundSingle);
  EXPECT_TRUE(foundMeasured);
  EXPECT_TRUE(foundTermination);

  session->Stop();
  io->stop();
}

// 验证：延迟的 S 帧确认会由 t2 超时触发。
TEST(IEC104TcpSessionTest, DelayedAckUsesT2Timer) {
  auto io = std::make_shared<boost::asio::io_context>();
  auto sockets = MakeConnectedSockets(*io);

  auto config = MakeConfig("t2-ack", IEC104Proto::ROLE_SERVER, 2, 2, 1, 5, 2);
  auto session = std::make_shared<IEC104::TcpSession>(*io, config, false);
  session->Start(std::move(sockets.session_socket));

  std::jthread session_thread = ModuleManager::StartModuleThread(
      IEC104LibInfo.LIB_NAME,
      [&]() { io->run(); });

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

// 验证遥控预置后执行会回调命令并发送确认报文。
TEST(IEC104TcpSessionTest, SingleCommandSelectExecute) {
  auto io = std::make_shared<boost::asio::io_context>();
  auto sockets = MakeConnectedSockets(*io);

  auto config = MakeConfig("cmd-select-execute", IEC104Proto::ROLE_SERVER, 2, 2, 1, 5, 8);
  auto session = std::make_shared<IEC104::TcpSession>(*io, config, false);

  std::promise<IEC104::CommandValue> cmdPromise;
  session->SetCommandCallback([&](const IEC104::CommandValue& cv) {
    cmdPromise.set_value(cv);
    return IEC104::CommandResult{};
  });
  session->Start(std::move(sockets.session_socket));

  std::jthread session_thread = ModuleManager::StartModuleThread(
      IEC104LibInfo.LIB_NAME,
      [&]() { io->run(); });

  auto start_act = BuildUFrame(kUStartDtAct);
  boost::asio::write(sockets.peer_socket, boost::asio::buffer(start_act));

  auto start_con = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "STARTDT_CON");
  ASSERT_FALSE(start_con.empty());
  EXPECT_EQ(FrameTypeOf(start_con), FrameType::U);
  EXPECT_EQ(start_con[2], kUStartDtCon);

  auto select_asdu = BuildSingleCommandAsdu(100, true, true, kCotActivation);
  auto select_frame = BuildIFrame(0, 0, select_asdu);
  boost::asio::write(sockets.peer_socket, boost::asio::buffer(select_frame));

  auto select_con = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "SELECT_CON");
  ASSERT_FALSE(select_con.empty());
  ASSERT_EQ(FrameTypeOf(select_con), FrameType::I);
  ASSERT_GT(select_con.size(), 6u);
  EXPECT_EQ(select_con[6], kTypeIdSingleCommand);
  EXPECT_EQ(static_cast<uint8_t>(select_con[8] & 0x3F), kCotActivationCon);

  auto exec_asdu = BuildSingleCommandAsdu(100, true, false, kCotActivation);
  auto exec_frame = BuildIFrame(1, 0, exec_asdu);
  boost::asio::write(sockets.peer_socket, boost::asio::buffer(exec_frame));

  auto exec_con = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "EXEC_CON");
  ASSERT_FALSE(exec_con.empty());
  EXPECT_EQ(exec_con[6], kTypeIdSingleCommand);
  EXPECT_EQ(static_cast<uint8_t>(exec_con[8] & 0x3F), kCotActivationCon);

  auto exec_term = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "EXEC_TERM");
  ASSERT_FALSE(exec_term.empty());
  EXPECT_EQ(exec_term[6], kTypeIdSingleCommand);
  EXPECT_EQ(static_cast<uint8_t>(exec_term[8] & 0x3F), kCotActivationTermination);

  auto future = cmdPromise.get_future();
  auto status = future.wait_for(std::chrono::milliseconds(500));
  ASSERT_EQ(status, std::future_status::ready);
  auto cmd = future.get();
  EXPECT_EQ(cmd.ioa, 100u);
  EXPECT_EQ(cmd.type, IEC104Proto::POINT_TYPE_SINGLE);
  EXPECT_TRUE(cmd.boolValue);

  session->Stop();
  io->stop();
}

// 验证未预置的遥控执行会返回否定确认且不回调命令。
TEST(IEC104TcpSessionTest, SingleCommandExecuteWithoutSelect) {
  auto io = std::make_shared<boost::asio::io_context>();
  auto sockets = MakeConnectedSockets(*io);

  auto config = MakeConfig("cmd-execute-only", IEC104Proto::ROLE_SERVER, 2, 2, 1, 5, 8);
  auto session = std::make_shared<IEC104::TcpSession>(*io, config, false);

  std::atomic<bool> called{false};
  session->SetCommandCallback([&](const IEC104::CommandValue&) {
    called = true;
    return IEC104::CommandResult{};
  });
  session->Start(std::move(sockets.session_socket));

  std::jthread session_thread = ModuleManager::StartModuleThread(
      IEC104LibInfo.LIB_NAME,
      [&]() { io->run(); });

  auto start_act = BuildUFrame(kUStartDtAct);
  boost::asio::write(sockets.peer_socket, boost::asio::buffer(start_act));

  auto start_con = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "STARTDT_CON");
  ASSERT_FALSE(start_con.empty());
  EXPECT_EQ(FrameTypeOf(start_con), FrameType::U);
  EXPECT_EQ(start_con[2], kUStartDtCon);

  auto exec_asdu = BuildSingleCommandAsdu(200, false, false, kCotActivation);
  auto exec_frame = BuildIFrame(0, 0, exec_asdu);
  boost::asio::write(sockets.peer_socket, boost::asio::buffer(exec_frame));

  auto exec_con = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "EXEC_CON_NEGATIVE");
  ASSERT_FALSE(exec_con.empty());
  ASSERT_EQ(FrameTypeOf(exec_con), FrameType::I);
  ASSERT_GT(exec_con.size(), 6u);
  EXPECT_EQ(exec_con[6], kTypeIdSingleCommand);
  EXPECT_EQ(static_cast<uint8_t>(exec_con[8] & 0x3F), kCotActivationCon);
  EXPECT_NE(exec_con[8] & kCotNegative, 0);

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_FALSE(called.load());

  session->Stop();
  io->stop();
}

// 验证设点执行被业务拒绝时返回否定确认且不发送执行结束。
TEST(IEC104TcpSessionTest, SetpointCommandRejectedByCallback) {
  auto io = std::make_shared<boost::asio::io_context>();
  auto sockets = MakeConnectedSockets(*io);

  auto config = MakeConfig("setpoint-reject", IEC104Proto::ROLE_SERVER, 2, 2, 1, 5, 8);
  auto session = std::make_shared<IEC104::TcpSession>(*io, config, false);

  std::promise<IEC104::CommandValue> cmdPromise;
  session->SetCommandCallback([&](const IEC104::CommandValue& cv) {
    cmdPromise.set_value(cv);
    IEC104::CommandResult result;
    result.accepted = false;
    result.reason = "测试拒绝";
    return result;
  });
  session->Start(std::move(sockets.session_socket));

  std::jthread session_thread = ModuleManager::StartModuleThread(
      IEC104LibInfo.LIB_NAME,
      [&]() { io->run(); });

  auto start_act = BuildUFrame(kUStartDtAct);
  boost::asio::write(sockets.peer_socket, boost::asio::buffer(start_act));

  auto start_con = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "STARTDT_CON");
  ASSERT_FALSE(start_con.empty());
  EXPECT_EQ(FrameTypeOf(start_con), FrameType::U);
  EXPECT_EQ(start_con[2], kUStartDtCon);

  auto exec_asdu = BuildSetpointCommandAsdu(300, 123.5F, false, kCotActivation);
  auto exec_frame = BuildIFrame(0, 0, exec_asdu);
  boost::asio::write(sockets.peer_socket, boost::asio::buffer(exec_frame));

  auto exec_con = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "SETPOINT_EXEC_CON_NEGATIVE");
  ASSERT_FALSE(exec_con.empty());
  ASSERT_EQ(FrameTypeOf(exec_con), FrameType::I);
  ASSERT_GT(exec_con.size(), 6u);
  EXPECT_EQ(exec_con[6], kTypeIdSetpointShort);
  EXPECT_EQ(static_cast<uint8_t>(exec_con[8] & 0x3F), kCotActivationCon);
  EXPECT_NE(exec_con[8] & kCotNegative, 0);
  EXPECT_FALSE(WaitReadable(sockets.peer_socket, std::chrono::milliseconds(200)));

  auto future = cmdPromise.get_future();
  auto status = future.wait_for(std::chrono::milliseconds(500));
  ASSERT_EQ(status, std::future_status::ready);
  auto cmd = future.get();
  EXPECT_EQ(cmd.ioa, 300u);
  EXPECT_EQ(cmd.type, IEC104Proto::POINT_TYPE_FLOAT);
  EXPECT_DOUBLE_EQ(cmd.doubleValue, 123.5);

  session->Stop();
  io->stop();
}

// 验证 point_with_time=false 时上送点值使用不带时标类型。
TEST(IEC104TcpSessionTest, PointWithoutTimeUsesNoTimestampTypes) {
  auto io = std::make_shared<boost::asio::io_context>();
  auto sockets = MakeConnectedSockets(*io);

  auto config = MakeConfig("point-no-time", IEC104Proto::ROLE_SERVER, 2, 2, 1, 5, 8);
  config.set_point_batch_window_ms(1);
  config.set_point_with_time(false);
  auto session = std::make_shared<IEC104::TcpSession>(*io, config, false);
  session->Start(std::move(sockets.session_socket));

  std::jthread session_thread = ModuleManager::StartModuleThread(
      IEC104LibInfo.LIB_NAME,
      [&]() { io->run(); });

  auto start_act = BuildUFrame(kUStartDtAct);
  boost::asio::write(sockets.peer_socket, boost::asio::buffer(start_act));

  auto start_con = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "启动确认");
  ASSERT_FALSE(start_con.empty());
  EXPECT_EQ(FrameTypeOf(start_con), FrameType::U);
  EXPECT_EQ(start_con[2], kUStartDtCon);

  IEC104::PointValue mv;
  mv.ioa = 1;
  mv.type = IEC104Proto::POINT_TYPE_FLOAT;
  mv.doubleValue = 1.23;
  mv.quality = 0;
  session->SendPointValue(mv, kCotSpontaneous);

  auto mv_apdu = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "遥测无时标");
  ASSERT_FALSE(mv_apdu.empty());
  ASSERT_EQ(FrameTypeOf(mv_apdu), FrameType::I);
  ASSERT_GT(mv_apdu.size(), 6u);
  EXPECT_EQ(mv_apdu[6], kTypeIdMeasuredValueShort);

  IEC104::PointValue sv;
  sv.ioa = 2;
  sv.type = IEC104Proto::POINT_TYPE_SINGLE;
  sv.boolValue = true;
  sv.quality = 0;
  session->SendPointValue(sv, kCotSpontaneous);

  auto sp_apdu = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "单点无时标");
  ASSERT_FALSE(sp_apdu.empty());
  ASSERT_EQ(FrameTypeOf(sp_apdu), FrameType::I);
  ASSERT_GT(sp_apdu.size(), 6u);
  EXPECT_EQ(sp_apdu[6], kTypeIdSinglePoint);

  session->Stop();
  io->stop();
}

// 验证 point_with_time=true 时上送点值使用带时标类型。
TEST(IEC104TcpSessionTest, PointWithTimeUsesTimestampTypes) {
  auto io = std::make_shared<boost::asio::io_context>();
  auto sockets = MakeConnectedSockets(*io);

  auto config = MakeConfig("point-with-time", IEC104Proto::ROLE_SERVER, 2, 2, 1, 5, 8);
  config.set_point_batch_window_ms(1);
  config.set_point_with_time(true);
  auto session = std::make_shared<IEC104::TcpSession>(*io, config, false);
  session->Start(std::move(sockets.session_socket));

  std::jthread session_thread = ModuleManager::StartModuleThread(
      IEC104LibInfo.LIB_NAME,
      [&]() { io->run(); });

  auto start_act = BuildUFrame(kUStartDtAct);
  boost::asio::write(sockets.peer_socket, boost::asio::buffer(start_act));

  auto start_con = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "启动确认");
  ASSERT_FALSE(start_con.empty());
  EXPECT_EQ(FrameTypeOf(start_con), FrameType::U);
  EXPECT_EQ(start_con[2], kUStartDtCon);

  IEC104::PointValue mv;
  mv.ioa = 1;
  mv.type = IEC104Proto::POINT_TYPE_FLOAT;
  mv.doubleValue = 2.34;
  mv.quality = 0;
  mv.tsMs = 1710000000000;
  session->SendPointValue(mv, kCotSpontaneous);

  auto mv_apdu = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "遥测带时标");
  ASSERT_FALSE(mv_apdu.empty());
  ASSERT_EQ(FrameTypeOf(mv_apdu), FrameType::I);
  ASSERT_GT(mv_apdu.size(), 6u);
  EXPECT_EQ(mv_apdu[6], kTypeIdMeasuredValueShortWithTime);

  IEC104::PointValue sv;
  sv.ioa = 2;
  sv.type = IEC104Proto::POINT_TYPE_SINGLE;
  sv.boolValue = true;
  sv.quality = 0;
  sv.tsMs = 1710000000000;
  session->SendPointValue(sv, kCotSpontaneous);

  auto sp_apdu = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "单点带时标");
  ASSERT_FALSE(sp_apdu.empty());
  ASSERT_EQ(FrameTypeOf(sp_apdu), FrameType::I);
  ASSERT_GT(sp_apdu.size(), 6u);
  EXPECT_EQ(sp_apdu[6], kTypeIdSinglePointWithTime);

  session->Stop();
  io->stop();
}

// 验证主站角色在服务端模式下也会发送启动帧。
TEST(IEC104TcpSessionTest, MasterRoleSendsStartDtWhenServerRole) {
  auto io = std::make_shared<boost::asio::io_context>();
  auto sockets = MakeConnectedSockets(*io);

  auto config = MakeConfig("startdt-master-server", IEC104Proto::ROLE_SERVER, 5, 2, 1, 5, 8);
  config.set_station_role(IEC104Proto::STATION_ROLE_MASTER);
  auto session = std::make_shared<IEC104::TcpSession>(*io, config, false);
  session->Start(std::move(sockets.session_socket));

  std::jthread session_thread = ModuleManager::StartModuleThread(
      IEC104LibInfo.LIB_NAME,
      [&]() { io->run(); });

  auto start_act = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "STARTDT_ACT");
  ASSERT_FALSE(start_act.empty());
  EXPECT_EQ(FrameTypeOf(start_act), FrameType::U);
  EXPECT_EQ(start_act[2], kUStartDtAct);

  session->Stop();
  io->stop();
}

// 验证从站角色在客户端模式下不会发送启动帧。
TEST(IEC104TcpSessionTest, SlaveRoleDoesNotSendStartDtWhenClientRole) {
  auto io = std::make_shared<boost::asio::io_context>();
  auto sockets = MakeConnectedSockets(*io);

  auto config = MakeConfig("startdt-slave-client", IEC104Proto::ROLE_CLIENT, 5, 2, 1, 5, 8);
  config.set_station_role(IEC104Proto::STATION_ROLE_SLAVE);
  auto session = std::make_shared<IEC104::TcpSession>(*io, config, true);
  session->Start(std::move(sockets.session_socket));

  std::jthread session_thread = ModuleManager::StartModuleThread(
      IEC104LibInfo.LIB_NAME,
      [&]() { io->run(); });

  EXPECT_FALSE(WaitReadable(sockets.peer_socket, std::chrono::milliseconds(300)));

  session->Stop();
  io->stop();
}
