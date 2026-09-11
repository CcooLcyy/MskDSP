#include <gtest/gtest.h>

#include <array>
#include <thread>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include "ModbusTCPTcpBus.h"

namespace {

// 验证 Modbus TCP 读取请求包含正确的 MBAP 头、Unit ID 和功能码，并能解析响应。
TEST(ModbusTcpBusTest, ExchangesMbapReadHoldingRegisterFrame) {
  boost::asio::io_context serverIo;
  boost::asio::ip::tcp::acceptor acceptor(serverIo, {boost::asio::ip::tcp::v4(), 0});
  const auto port = acceptor.local_endpoint().port();
  std::thread server([&] {
    boost::asio::ip::tcp::socket socket(serverIo);
    acceptor.accept(socket);
    std::array<uint8_t, 12> request{};
    boost::asio::read(socket, boost::asio::buffer(request));
    EXPECT_EQ(request[0], 0x00);
    EXPECT_EQ(request[1], 0x01);
    EXPECT_EQ(request[2], 0x00);
    EXPECT_EQ(request[3], 0x00);
    EXPECT_EQ(request[4], 0x00);
    EXPECT_EQ(request[5], 0x06);
    EXPECT_EQ(request[6], 0x01);
    EXPECT_EQ(request[7], 0x03);
    EXPECT_EQ(request[8], 0x00);
    EXPECT_EQ(request[9], 0x10);
    EXPECT_EQ(request[10], 0x00);
    EXPECT_EQ(request[11], 0x01);
    const std::array<uint8_t, 9> response{0x00, 0x01, 0x00, 0x00, 0x00, 0x05, 0x01, 0x03, 0x02};
    boost::asio::write(socket, boost::asio::buffer(response));
    const std::array<uint8_t, 2> value{0x12, 0x34};
    boost::asio::write(socket, boost::asio::buffer(value));
  });

  ModbusTCPProto::TcpConfig config;
  config.set_host("127.0.0.1");
  config.set_port(port);
  config.set_unit_id(1);
  config.set_connect_timeout_ms(1000);
  config.set_request_timeout_ms(1000);
  ModbusTCP::TcpBus bus(config);
  const auto openStatus = bus.Open();
  if (!openStatus.ok()) {
    boost::system::error_code closeError;
    acceptor.close(closeError);
    server.join();
    FAIL() << openStatus.error_message();
  }
  uint16_t value = 0;
  ASSERT_TRUE(bus.ReadHoldingRegister(1, 0x0010, &value).ok());
  EXPECT_EQ(value, 0x1234);
  bus.Close();
  server.join();
}

// 验证 TCP 链路缺少主机或端口时拒绝打开，避免产生无效连接。
TEST(ModbusTcpBusTest, RejectsMissingHostAndPort) {
  ModbusTCPProto::TcpConfig config;
  ModbusTCP::TcpBus bus(config);
  const auto status = bus.Open();
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

}  // namespace
