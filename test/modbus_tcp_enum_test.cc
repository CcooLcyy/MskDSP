#include <gtest/gtest.h>

#include <array>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

#include "ModbusTCPLinkManager.h"
#include "support/FakeDataCenter.hpp"

namespace {
// 非阻塞模拟器保证链路配置失败时测试线程仍可停止，不依赖固定端口。
class EnumTcpServer {
public:
  EnumTcpServer() : acceptor_(io_, {boost::asio::ip::tcp::v4(), 0}) {
    acceptor_.non_blocking(true);
    thread_ = std::jthread([this](std::stop_token stop) {
      boost::asio::ip::tcp::socket socket(io_);
      boost::system::error_code error;
      while (!stop.stop_requested()) {
        acceptor_.accept(socket, error);
        if (!error)
          break;
        if (error != boost::asio::error::would_block && error != boost::asio::error::try_again)
          return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if (stop.stop_requested())
        return;
      socket.non_blocking(true);
      while (!stop.stop_requested()) {
        std::array<uint8_t, 12> request{};
        size_t offset = 0;
        while (offset < request.size() && !stop.stop_requested()) {
          const auto count = socket.read_some(boost::asio::buffer(request.data() + offset, request.size() - offset), error);
          if (!error)
            offset += count;
          else if (error == boost::asio::error::would_block || error == boost::asio::error::try_again)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          else
            return;
        }
        if (stop.stop_requested())
          return;
        const bool twoWords = request[11] == 2;
        std::vector<uint8_t> response{request[0], request[1], 0, 0, 0, static_cast<uint8_t>(twoWords ? 7 : 5), request[6], request[7], static_cast<uint8_t>(twoWords ? 4 : 2)};
        if (twoWords)
          response.insert(response.end(), {0x78, 0x56, 0x34, 0x12});
        else
          response.insert(response.end(), {0x34, 0x12});
        // 单帧很小；临时恢复阻塞写入，客户端读取失败仍受请求超时约束。
        socket.non_blocking(false);
        boost::asio::write(socket, boost::asio::buffer(response), error);
        if (error)
          return;
        socket.non_blocking(true);
      }
    });
  }
  ~EnumTcpServer() {
    thread_.request_stop();
  }
  uint16_t port() const {
    return acceptor_.local_endpoint().port();
  }

private:
  boost::asio::io_context io_;
  boost::asio::ip::tcp::acceptor acceptor_;
  std::jthread thread_;
};
}  // 命名空间结束

// 验证：TCP 的 0x03/0x04 枚举读取统一按归一化无符号 raw 相等，发布正确 BOOL 与品质。
TEST(ModbusTcpEnumTest, PollsNormalizedEnumBool) {
  for (uint32_t count : {1u, 2u}) {
    EnumTcpServer server;
    FakeDataCenterState state;
    const auto db = std::filesystem::current_path() / ("tcp-enum-" + std::to_string(count) + ".db");
    ModbusTCP::LinkManager mgr(db);
    mgr.setDataCenterStub(MakeStub(&state));
    ModbusTCPProto::UpsertLinkRequest link;
    auto *config = link.mutable_config();
    config->set_conn_name("tcp-enum");
    config->set_poll_interval_ms(5);
    config->mutable_tcp()->set_host("127.0.0.1");
    config->mutable_tcp()->set_port(server.port());
    config->mutable_tcp()->set_unit_id(1);
    config->mutable_tcp()->set_request_timeout_ms(200);
    ModbusTCPProto::LinkInfo info;
    ASSERT_TRUE(mgr.UpsertLink(link, &info).ok());
    ModbusTCPProto::UpsertPointTableRequest table;
    table.set_conn_name("tcp-enum");
    table.set_replace(true);
    for (bool matches : {true, false}) {
      auto *point = table.add_points();
      point->set_tag(matches ? "match" : "other");
      point->set_function(count == 1 ? ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS : ModbusRTUProto::FUNCTION_READ_INPUT_REGISTERS);
      point->set_address(10);
      point->set_type(ModbusRTUProto::DATA_TYPE_BOOL);
      point->set_reg_count(count);
      point->set_byte_order(ModbusRTUProto::BYTE_ORDER_BA);
      point->set_word_order(ModbusRTUProto::WORD_ORDER_LH);
      point->set_enum_value(matches ? (count == 1 ? 0x1234u : 0x12345678u) : 7u);
    }
    ASSERT_TRUE(mgr.UpsertPointTable(table).ok());
    ASSERT_TRUE(mgr.StartLink("tcp-enum").ok());
    for (const char *tag : {"match", "other"}) {
      ASSERT_TRUE(state.WaitForPublishCount(info.conn_id(), tag, 1, std::chrono::seconds(2)));
      const auto updates = state.GetPublishedUpdates(info.conn_id(), tag);
      ASSERT_FALSE(updates.empty());
      ASSERT_TRUE(updates[0].value().has_bool_value());
      EXPECT_EQ(updates[0].value().bool_value(), std::string(tag) == "match");
      EXPECT_EQ(updates[0].quality(), DataCenterProto::QUALITY_GOOD);
    }
    ASSERT_TRUE(mgr.StopLink("tcp-enum").ok());
  }
}
