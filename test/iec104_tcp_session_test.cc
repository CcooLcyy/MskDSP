#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
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
constexpr uint8_t kTypeIdDoubleCommand = 46;
constexpr uint8_t kTypeIdSetpointShort = 50;
constexpr uint8_t kTypeIdTimeSyncCmd = 103;
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

std::vector<uint8_t> BuildSFrame(uint16_t recv_seq) {
  std::vector<uint8_t> apdu(6);
  apdu[0] = kApduStart;
  apdu[1] = 0x04;
  apdu[2] = 0x01;
  apdu[3] = 0x00;
  WriteSeq(&apdu, 4, recv_seq);
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

std::vector<uint8_t> BuildDoubleCommandAsdu(uint32_t ioa, uint8_t value, bool select, uint8_t cause) {
  std::vector<uint8_t> asdu;
  asdu.reserve(10);
  asdu.emplace_back(kTypeIdDoubleCommand);
  asdu.emplace_back(0x01);
  asdu.emplace_back(static_cast<uint8_t>(cause & 0x3F));
  asdu.emplace_back(0x00);
  asdu.emplace_back(0x01);
  asdu.emplace_back(0x00);
  asdu.emplace_back(static_cast<uint8_t>(ioa & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((ioa >> 8) & 0xFF));
  asdu.emplace_back(static_cast<uint8_t>((ioa >> 16) & 0xFF));
  uint8_t dco = static_cast<uint8_t>(value & 0x03);
  if (select) {
    dco |= kScoSelectMask;
  }
  asdu.emplace_back(dco);
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

std::vector<uint8_t> BuildTimeSyncAsdu(uint8_t cause) {
  // 2024-01-01 00:00:00.000，使用有效的 CP56Time2a 作为固定测试输入。
  return {kTypeIdTimeSyncCmd,
          0x01,
          static_cast<uint8_t>(cause & 0x3F),
          0x01,  // OA
          0x01,  // CA low
          0x00,  // CA high
          0x00, 0x00, 0x00,  // IOA
          0x00, 0x00,  // milliseconds
          0x00,        // minute
          0x00,        // hour
          0x21,
          0x01,
          0x18};
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
  session->SetCommandExecutionModeCallback([](uint32_t) {
    return IEC104Proto::COMMAND_EXECUTION_MODE_SELECT_EXECUTE;
  });

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

// 验证选择执行点位收到未预置的 S/E=0 时返回否定确认且不触发业务回调。
TEST(IEC104TcpSessionTest, SelectExecuteRejectsExecuteWithoutSelect) {
  auto io = std::make_shared<boost::asio::io_context>();
  auto sockets = MakeConnectedSockets(*io);

  auto config = MakeConfig("cmd-execute-only", IEC104Proto::ROLE_SERVER, 2, 2, 1, 5, 8);
  auto session = std::make_shared<IEC104::TcpSession>(*io, config, false);
  session->SetCommandExecutionModeCallback([](uint32_t) {
    return IEC104Proto::COMMAND_EXECUTION_MODE_SELECT_EXECUTE;
  });

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
  EXPECT_FALSE(WaitReadable(sockets.peer_socket, std::chrono::milliseconds(200)));

  session->Stop();
  io->stop();
}

// 验证直接执行点位收到裸 S/E=0 单点遥控时执行并返回肯定确认。
TEST(IEC104TcpSessionTest, SingleCommandDirectExecute) {
  auto io = std::make_shared<boost::asio::io_context>();
  auto sockets = MakeConnectedSockets(*io);

  auto config = MakeConfig("single-direct-execute", IEC104Proto::ROLE_SERVER, 2, 2, 1, 5, 8);
  auto session = std::make_shared<IEC104::TcpSession>(*io, config, false);
  session->SetCommandExecutionModeCallback([](uint32_t) {
    return IEC104Proto::COMMAND_EXECUTION_MODE_DIRECT;
  });

  std::atomic<int> callCount{0};
  session->SetCommandCallback([&](const IEC104::CommandValue&) {
    ++callCount;
    return IEC104::CommandResult{};
  });
  session->Start(std::move(sockets.session_socket));

  std::jthread session_thread = ModuleManager::StartModuleThread(
      IEC104LibInfo.LIB_NAME,
      [&]() { io->run(); });

  boost::asio::write(sockets.peer_socket, boost::asio::buffer(BuildUFrame(kUStartDtAct)));
  ASSERT_FALSE(ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "STARTDT_CON").empty());

  boost::asio::write(sockets.peer_socket,
                     boost::asio::buffer(BuildIFrame(0, 0, BuildSingleCommandAsdu(201, false, false, kCotActivation))));
  auto execCon = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "SINGLE_DIRECT_CON");
  auto execTerm = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "SINGLE_DIRECT_TERM");
  ASSERT_GE(execCon.size(), 16u);
  ASSERT_GE(execTerm.size(), 16u);
  EXPECT_EQ(execCon[6], kTypeIdSingleCommand);
  EXPECT_EQ(execCon[8] & kCotNegative, 0);
  EXPECT_EQ(static_cast<uint8_t>(execTerm[8] & 0x3F), kCotActivationTermination);
  EXPECT_EQ(callCount.load(), 1);

  session->Stop();
  io->stop();
}

// 验证双点遥控预置后执行会保留 Type ID 和 DCO 状态并回调命令。
TEST(IEC104TcpSessionTest, DoubleCommandSelectExecute) {
  auto io = std::make_shared<boost::asio::io_context>();
  auto sockets = MakeConnectedSockets(*io);

  auto config = MakeConfig("double-select-execute", IEC104Proto::ROLE_SERVER, 2, 2, 1, 5, 8);
  auto session = std::make_shared<IEC104::TcpSession>(*io, config, false);
  session->SetCommandExecutionModeCallback([](uint32_t) {
    return IEC104Proto::COMMAND_EXECUTION_MODE_SELECT_EXECUTE;
  });

  std::promise<IEC104::CommandValue> cmdPromise;
  session->SetCommandCallback([&](const IEC104::CommandValue& cv) {
    cmdPromise.set_value(cv);
    return IEC104::CommandResult{};
  });
  session->Start(std::move(sockets.session_socket));

  std::jthread session_thread = ModuleManager::StartModuleThread(
      IEC104LibInfo.LIB_NAME,
      [&]() { io->run(); });

  boost::asio::write(sockets.peer_socket, boost::asio::buffer(BuildUFrame(kUStartDtAct)));
  ASSERT_FALSE(ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "STARTDT_CON").empty());

  boost::asio::write(sockets.peer_socket,
                     boost::asio::buffer(BuildIFrame(0, 0, BuildDoubleCommandAsdu(300, 2, true, kCotActivation))));
  auto selectCon = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "DOUBLE_SELECT_CON");
  ASSERT_GE(selectCon.size(), 16u);
  EXPECT_EQ(selectCon[6], kTypeIdDoubleCommand);
  EXPECT_EQ(selectCon[15] & 0x83, 0x82);

  boost::asio::write(sockets.peer_socket,
                     boost::asio::buffer(BuildIFrame(1, 0, BuildDoubleCommandAsdu(300, 2, false, kCotActivation))));
  auto execCon = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "DOUBLE_EXEC_CON");
  ASSERT_GE(execCon.size(), 16u);
  EXPECT_EQ(execCon[6], kTypeIdDoubleCommand);
  EXPECT_EQ(execCon[15] & 0x83, 0x02);
  auto execTerm = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "DOUBLE_EXEC_TERM");
  ASSERT_GE(execTerm.size(), 16u);
  EXPECT_EQ(execTerm[6], kTypeIdDoubleCommand);

  auto future = cmdPromise.get_future();
  ASSERT_EQ(future.wait_for(std::chrono::milliseconds(500)), std::future_status::ready);
  const auto command = future.get();
  EXPECT_EQ(command.ioa, 300u);
  EXPECT_EQ(command.type, IEC104Proto::POINT_TYPE_SINGLE);
  EXPECT_EQ(command.remoteControlType, IEC104Proto::REMOTE_CONTROL_TYPE_DOUBLE);
  EXPECT_EQ(command.controlValue, 2);
  EXPECT_TRUE(command.boolValue);

  session->Stop();
  io->stop();
}

// 验证未预置的 S/E=0 双点遥控按直接执行处理并回调 DCO 状态。
TEST(IEC104TcpSessionTest, DoubleCommandDirectExecute) {
  auto io = std::make_shared<boost::asio::io_context>();
  auto sockets = MakeConnectedSockets(*io);

  auto config = MakeConfig("double-direct-execute", IEC104Proto::ROLE_SERVER, 2, 2, 1, 5, 8);
  auto session = std::make_shared<IEC104::TcpSession>(*io, config, false);
  session->SetCommandExecutionModeCallback([](uint32_t) {
    return IEC104Proto::COMMAND_EXECUTION_MODE_DIRECT;
  });

  std::promise<IEC104::CommandValue> cmdPromise;
  session->SetCommandCallback([&](const IEC104::CommandValue& cv) {
    cmdPromise.set_value(cv);
    return IEC104::CommandResult{};
  });
  session->Start(std::move(sockets.session_socket));

  std::jthread session_thread = ModuleManager::StartModuleThread(
      IEC104LibInfo.LIB_NAME,
      [&]() { io->run(); });

  boost::asio::write(sockets.peer_socket, boost::asio::buffer(BuildUFrame(kUStartDtAct)));
  ASSERT_FALSE(ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "STARTDT_CON").empty());

  boost::asio::write(sockets.peer_socket,
                     boost::asio::buffer(BuildIFrame(0, 0, BuildDoubleCommandAsdu(302, 1, false, kCotActivation))));
  auto execCon = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "DOUBLE_DIRECT_CON");
  auto execTerm = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "DOUBLE_DIRECT_TERM");
  ASSERT_GE(execCon.size(), 16u);
  ASSERT_GE(execTerm.size(), 16u);
  EXPECT_EQ(execCon[6], kTypeIdDoubleCommand);
  EXPECT_EQ(execCon[8] & kCotNegative, 0);
  EXPECT_EQ(execCon[15] & 0x83, 0x01);
  EXPECT_EQ(static_cast<uint8_t>(execTerm[8] & 0x3F), kCotActivationTermination);

  auto future = cmdPromise.get_future();
  ASSERT_EQ(future.wait_for(std::chrono::milliseconds(500)), std::future_status::ready);
  const auto command = future.get();
  EXPECT_EQ(command.remoteControlType, IEC104Proto::REMOTE_CONTROL_TYPE_DOUBLE);
  EXPECT_EQ(command.controlValue, 1);
  EXPECT_FALSE(command.boolValue);

  session->Stop();
  io->stop();
}

// 验证直接执行点位也允许完整的选择后执行流程，并最终只回调一次业务命令。
TEST(IEC104TcpSessionTest, DirectExecuteAcceptsSelectExecuteFlow) {
  auto io = std::make_shared<boost::asio::io_context>();
  auto sockets = MakeConnectedSockets(*io);

  auto config = MakeConfig("direct-select-flow", IEC104Proto::ROLE_SERVER, 2, 2, 1, 5, 8);
  auto session = std::make_shared<IEC104::TcpSession>(*io, config, false);
  session->SetCommandExecutionModeCallback([](uint32_t) {
    return IEC104Proto::COMMAND_EXECUTION_MODE_DIRECT;
  });

  std::atomic<int> callCount{0};
  session->SetCommandCallback([&](const IEC104::CommandValue&) {
    ++callCount;
    return IEC104::CommandResult{};
  });
  session->Start(std::move(sockets.session_socket));

  std::jthread session_thread = ModuleManager::StartModuleThread(
      IEC104LibInfo.LIB_NAME,
      [&]() { io->run(); });

  boost::asio::write(sockets.peer_socket, boost::asio::buffer(BuildUFrame(kUStartDtAct)));
  ASSERT_FALSE(ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "STARTDT_CON").empty());

  boost::asio::write(sockets.peer_socket,
                     boost::asio::buffer(BuildIFrame(0, 0, BuildSingleCommandAsdu(304, true, true, kCotActivation))));
  auto selectCon = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "DIRECT_SELECT_CON");
  ASSERT_GE(selectCon.size(), 16u);
  EXPECT_EQ(selectCon[6], kTypeIdSingleCommand);
  EXPECT_EQ(selectCon[8] & kCotNegative, 0);
  EXPECT_EQ(selectCon[15] & 0x81, 0x81);

  boost::asio::write(sockets.peer_socket,
                     boost::asio::buffer(BuildIFrame(1, 0, BuildSingleCommandAsdu(304, true, false, kCotActivation))));
  auto execCon = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "DIRECT_EXEC_CON");
  auto execTerm = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "DIRECT_EXEC_TERM");
  ASSERT_GE(execCon.size(), 16u);
  ASSERT_GE(execTerm.size(), 16u);
  EXPECT_EQ(execCon[6], kTypeIdSingleCommand);
  EXPECT_EQ(execCon[8] & kCotNegative, 0);
  EXPECT_EQ(execTerm[6], kTypeIdSingleCommand);
  EXPECT_EQ(static_cast<uint8_t>(execTerm[8] & 0x3F), kCotActivationTermination);
  EXPECT_EQ(callCount.load(), 1);

  session->Stop();
  io->stop();
}

// 验证双点遥控拒绝 DCO=0/3 非法状态且不会触发业务回调。
TEST(IEC104TcpSessionTest, DoubleCommandRejectsInvalidState) {
  auto io = std::make_shared<boost::asio::io_context>();
  auto sockets = MakeConnectedSockets(*io);

  auto config = MakeConfig("double-invalid-state", IEC104Proto::ROLE_SERVER, 2, 2, 1, 5, 8);
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

  boost::asio::write(sockets.peer_socket, boost::asio::buffer(BuildUFrame(kUStartDtAct)));
  ASSERT_FALSE(ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "STARTDT_CON").empty());

  boost::asio::write(sockets.peer_socket,
                     boost::asio::buffer(BuildIFrame(0, 0, BuildDoubleCommandAsdu(301, 3, false, kCotActivation))));
  auto negativeCon = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "DOUBLE_INVALID_CON");
  ASSERT_GE(negativeCon.size(), 16u);
  EXPECT_EQ(negativeCon[6], kTypeIdDoubleCommand);
  EXPECT_NE(negativeCon[8] & kCotNegative, 0);
  EXPECT_EQ(negativeCon[15] & 0x03, 0x03);
  EXPECT_FALSE(called.load());

  session->Stop();
  io->stop();
}

// 验证已预置遥控存在时，值不一致的执行帧被否定且不会降级为直接执行。
TEST(IEC104TcpSessionTest, RemoteControlRejectsMismatchedSelectedValue) {
  auto io = std::make_shared<boost::asio::io_context>();
  auto sockets = MakeConnectedSockets(*io);

  auto config = MakeConfig("remote-control-mismatch", IEC104Proto::ROLE_SERVER, 2, 2, 1, 5, 8);
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

  boost::asio::write(sockets.peer_socket, boost::asio::buffer(BuildUFrame(kUStartDtAct)));
  ASSERT_FALSE(ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "STARTDT_CON").empty());
  boost::asio::write(sockets.peer_socket,
                     boost::asio::buffer(BuildIFrame(0, 0, BuildSingleCommandAsdu(303, true, true, kCotActivation))));
  ASSERT_FALSE(ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "SELECT_CON").empty());

  boost::asio::write(sockets.peer_socket,
                     boost::asio::buffer(BuildIFrame(1, 0, BuildSingleCommandAsdu(303, false, false, kCotActivation))));
  auto negativeCon = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "MISMATCH_CON");
  ASSERT_GE(negativeCon.size(), 16u);
  EXPECT_NE(negativeCon[8] & kCotNegative, 0);
  EXPECT_FALSE(called.load());

  session->Stop();
  io->stop();
}

// 验证主站按遥控类型和执行方式编码直接双点命令及单点选择执行命令。
TEST(IEC104TcpSessionTest, SendsConfiguredRemoteControlCommands) {
  auto io = std::make_shared<boost::asio::io_context>();
  auto sockets = MakeConnectedSockets(*io);

  auto config = MakeConfig("send-remote-control", IEC104Proto::ROLE_CLIENT, 2, 2, 1, 5, 8);
  auto session = std::make_shared<IEC104::TcpSession>(*io, config, true);
  session->Start(std::move(sockets.session_socket));

  std::jthread session_thread = ModuleManager::StartModuleThread(
      IEC104LibInfo.LIB_NAME,
      [&]() { io->run(); });

  ASSERT_FALSE(ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "STARTDT_ACT").empty());
  boost::asio::write(sockets.peer_socket, boost::asio::buffer(BuildUFrame(kUStartDtCon)));
  ASSERT_FALSE(ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "INTERROGATION").empty());

  session->SendRemoteControl(400,
                             IEC104Proto::REMOTE_CONTROL_TYPE_DOUBLE,
                             1,
                             IEC104Proto::COMMAND_EXECUTION_MODE_DIRECT);
  auto direct = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "DOUBLE_DIRECT");
  ASSERT_GE(direct.size(), 16u);
  EXPECT_EQ(direct[6], kTypeIdDoubleCommand);
  EXPECT_EQ(direct[15] & 0x83, 0x01);

  session->SendRemoteControl(401,
                             IEC104Proto::REMOTE_CONTROL_TYPE_SINGLE,
                             1,
                             IEC104Proto::COMMAND_EXECUTION_MODE_SELECT_EXECUTE);
  auto select = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "SINGLE_SELECT");
  ASSERT_GE(select.size(), 16u);
  EXPECT_EQ(select[6], kTypeIdSingleCommand);
  EXPECT_EQ(select[15] & 0x81, 0x81);
  EXPECT_FALSE(WaitReadable(sockets.peer_socket, std::chrono::milliseconds(200)));

  auto selectCon = BuildSingleCommandAsdu(401, true, true, kCotActivationCon);
  boost::asio::write(sockets.peer_socket, boost::asio::buffer(BuildIFrame(0, 0, selectCon)));
  auto execute = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "SINGLE_EXECUTE");
  ASSERT_GE(execute.size(), 16u);
  EXPECT_EQ(execute[6], kTypeIdSingleCommand);
  EXPECT_EQ(execute[15] & 0x81, 0x01);

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

// 验证：SOE 恢复不按 IOA 去重、始终使用带时标单点报文，并由 I/S 帧中的 N(R) 累计确认。
TEST(IEC104TcpSessionTest, ReplaysSoeInOrderAndAcknowledgesByReceiveSequence) {
  auto io = std::make_shared<boost::asio::io_context>();
  auto sockets = MakeConnectedSockets(*io);

  auto config = MakeConfig("soe-replay-ack", IEC104Proto::ROLE_SERVER, 2, 2, 1, 5, 8);
  config.set_point_dedupe(true);
  config.set_point_with_time(false);
  auto session = std::make_shared<IEC104::TcpSession>(*io, config, false);

  std::atomic<int> replayCount{0};
  session->SetSoeReplayProvider([&]() {
    replayCount.fetch_add(1);
    IEC104::SoeEvent first;
    first.eventSequence = 101;
    first.value.ioa = 9;
    first.value.type = IEC104Proto::POINT_TYPE_SINGLE;
    first.value.boolValue = true;
    first.value.quality = 0;
    first.value.tsMs = 1710000000001;

    IEC104::SoeEvent second;
    second.eventSequence = 102;
    second.value.ioa = 9;
    second.value.type = IEC104Proto::POINT_TYPE_SINGLE;
    second.value.boolValue = false;
    second.value.quality = 0x80;
    second.value.tsMs = 1710000000002;
    return std::vector<IEC104::SoeEvent>{first, second};
  });

  std::promise<std::vector<uint64_t>> firstAckPromise;
  std::promise<std::vector<uint64_t>> secondAckPromise;
  std::atomic<int> ackCallbackCount{0};
  session->SetSoeAcknowledgedCallback([&](const std::vector<uint64_t> &sequences) {
    const int index = ackCallbackCount.fetch_add(1);
    if (index == 0) {
      firstAckPromise.set_value(sequences);
    } else if (index == 1) {
      secondAckPromise.set_value(sequences);
    }
  });
  auto firstAckFuture = firstAckPromise.get_future();
  auto secondAckFuture = secondAckPromise.get_future();

  session->Start(std::move(sockets.session_socket));
  std::jthread sessionThread = ModuleManager::StartModuleThread(
      IEC104LibInfo.LIB_NAME,
      [&]() { io->run(); });

  IEC104::SoeEvent beforeStartDt;
  beforeStartDt.eventSequence = 102;
  beforeStartDt.value.ioa = 9;
  beforeStartDt.value.type = IEC104Proto::POINT_TYPE_SINGLE;
  beforeStartDt.value.boolValue = false;
  beforeStartDt.value.tsMs = 1710000000002;
  session->SendSoe(beforeStartDt);
  std::promise<void> beforeStartDtHandled;
  auto beforeStartDtFuture = beforeStartDtHandled.get_future();
  boost::asio::post(*io, [&]() { beforeStartDtHandled.set_value(); });
  ASSERT_EQ(beforeStartDtFuture.wait_for(std::chrono::milliseconds(1000)), std::future_status::ready);

  auto startAct = BuildUFrame(kUStartDtAct);
  boost::asio::write(sockets.peer_socket, boost::asio::buffer(startAct));
  auto startCon = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "SOE 启动确认");
  ASSERT_FALSE(startCon.empty());
  EXPECT_EQ(FrameTypeOf(startCon), FrameType::U);
  EXPECT_EQ(startCon[2], kUStartDtCon);

  auto first = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "第一条 SOE");
  auto second = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "第二条 SOE");
  ASSERT_FALSE(first.empty());
  ASSERT_FALSE(second.empty());
  ASSERT_GT(first.size(), 15u);
  ASSERT_GT(second.size(), 15u);
  EXPECT_EQ(FrameTypeOf(first), FrameType::I);
  EXPECT_EQ(FrameTypeOf(second), FrameType::I);
  EXPECT_EQ(ParseSeq(first, 2), 0u);
  EXPECT_EQ(ParseSeq(second, 2), 1u);
  EXPECT_EQ(first[6], kTypeIdSinglePointWithTime);
  EXPECT_EQ(second[6], kTypeIdSinglePointWithTime);
  EXPECT_EQ(first[15] & 0x01, 0x01);
  EXPECT_EQ(second[15] & 0x01, 0x00);
  EXPECT_EQ(second[15] & 0xF0, 0x80);

  auto firstAck = BuildSFrame(1);
  boost::asio::write(sockets.peer_socket, boost::asio::buffer(firstAck));
  ASSERT_EQ(firstAckFuture.wait_for(std::chrono::milliseconds(1000)), std::future_status::ready);
  EXPECT_EQ(firstAckFuture.get(), std::vector<uint64_t>({101}));

  const std::vector<uint8_t> dummyAsdu = {0xFF, 0x01, 0x00, 0x00, 0x00, 0x00};
  auto secondAck = BuildIFrame(0, 2, dummyAsdu);
  boost::asio::write(sockets.peer_socket, boost::asio::buffer(secondAck));
  ASSERT_EQ(secondAckFuture.wait_for(std::chrono::milliseconds(1000)), std::future_status::ready);
  EXPECT_EQ(secondAckFuture.get(), std::vector<uint64_t>({102}));

  boost::asio::write(sockets.peer_socket, boost::asio::buffer(startAct));
  auto repeatedStartCon = ReadApduWithTimeout(
      sockets.peer_socket, std::chrono::milliseconds(2000), "重复 SOE 启动确认");
  ASSERT_FALSE(repeatedStartCon.empty());
  EXPECT_EQ(FrameTypeOf(repeatedStartCon), FrameType::U);
  EXPECT_EQ(repeatedStartCon[2], kUStartDtCon);
  EXPECT_FALSE(WaitReadable(sockets.peer_socket, std::chrono::milliseconds(200)));
  EXPECT_EQ(replayCount.load(), 1);
  EXPECT_EQ(ackCallbackCount.load(), 2);

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

// 验证：对时业务拒绝时只返回带负号的激活确认，不发送激活终止。
TEST(IEC104TcpSessionTest, TimeSyncRejectionReturnsNegativeConfirmation) {
  auto io = std::make_shared<boost::asio::io_context>();
  auto sockets = MakeConnectedSockets(*io);

  auto config = MakeConfig("time-sync-rejected", IEC104Proto::ROLE_SERVER, 5, 2, 1, 5, 8);
  config.set_station_role(IEC104Proto::STATION_ROLE_SLAVE);
  auto session = std::make_shared<IEC104::TcpSession>(*io, config, false);
  session->SetTimeSyncCallback([](int64_t) { return false; });
  session->Start(std::move(sockets.session_socket));

  std::jthread session_thread = ModuleManager::StartModuleThread(
      IEC104LibInfo.LIB_NAME,
      [&]() { io->run(); });

  boost::asio::write(sockets.peer_socket, boost::asio::buffer(BuildUFrame(kUStartDtAct)));
  auto start_con = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "对时启动确认");
  ASSERT_FALSE(start_con.empty());
  ASSERT_EQ(FrameTypeOf(start_con), FrameType::U);
  EXPECT_EQ(start_con[2], kUStartDtCon);

  auto request = BuildIFrame(0, 0, BuildTimeSyncAsdu(kCotActivation));
  boost::asio::write(sockets.peer_socket, boost::asio::buffer(request));

  auto confirmation = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "对时负确认");
  ASSERT_FALSE(confirmation.empty());
  ASSERT_EQ(FrameTypeOf(confirmation), FrameType::I);
  ASSERT_GE(confirmation.size(), 16u);
  EXPECT_EQ(confirmation[6], kTypeIdTimeSyncCmd);
  EXPECT_EQ(static_cast<uint8_t>(confirmation[8] & 0x3F), kCotActivationCon);
  EXPECT_NE(static_cast<uint8_t>(confirmation[8] & kCotNegative), 0);
  EXPECT_FALSE(WaitReadable(sockets.peer_socket, std::chrono::milliseconds(200)));

  session->Stop();
  io->stop();
}

// 验证：对时业务接受时返回激活确认和激活终止两个正确认。
TEST(IEC104TcpSessionTest, TimeSyncAcceptanceReturnsConfirmationAndTermination) {
  auto io = std::make_shared<boost::asio::io_context>();
  auto sockets = MakeConnectedSockets(*io);

  auto config = MakeConfig("time-sync-accepted", IEC104Proto::ROLE_SERVER, 5, 2, 1, 5, 8);
  config.set_station_role(IEC104Proto::STATION_ROLE_SLAVE);
  auto session = std::make_shared<IEC104::TcpSession>(*io, config, false);
  session->SetTimeSyncCallback([](int64_t tsMs) { return tsMs > 0; });
  session->Start(std::move(sockets.session_socket));

  std::jthread session_thread = ModuleManager::StartModuleThread(
      IEC104LibInfo.LIB_NAME,
      [&]() { io->run(); });

  boost::asio::write(sockets.peer_socket, boost::asio::buffer(BuildUFrame(kUStartDtAct)));
  auto start_con = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "对时启动确认");
  ASSERT_FALSE(start_con.empty());
  ASSERT_EQ(FrameTypeOf(start_con), FrameType::U);
  EXPECT_EQ(start_con[2], kUStartDtCon);

  auto request = BuildIFrame(0, 0, BuildTimeSyncAsdu(kCotActivation));
  boost::asio::write(sockets.peer_socket, boost::asio::buffer(request));

  auto confirmation = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "对时激活确认");
  ASSERT_FALSE(confirmation.empty());
  ASSERT_EQ(FrameTypeOf(confirmation), FrameType::I);
  ASSERT_GE(confirmation.size(), 16u);
  EXPECT_EQ(confirmation[6], kTypeIdTimeSyncCmd);
  EXPECT_EQ(static_cast<uint8_t>(confirmation[8] & 0x3F), kCotActivationCon);
  EXPECT_EQ(static_cast<uint8_t>(confirmation[8] & kCotNegative), 0);

  auto termination = ReadApduWithTimeout(sockets.peer_socket, std::chrono::milliseconds(2000), "对时激活终止");
  ASSERT_FALSE(termination.empty());
  ASSERT_EQ(FrameTypeOf(termination), FrameType::I);
  ASSERT_GE(termination.size(), 16u);
  EXPECT_EQ(termination[6], kTypeIdTimeSyncCmd);
  EXPECT_EQ(static_cast<uint8_t>(termination[8] & 0x3F), kCotActivationTermination);
  EXPECT_EQ(static_cast<uint8_t>(termination[8] & kCotNegative), 0);

  session->Stop();
  io->stop();
}
