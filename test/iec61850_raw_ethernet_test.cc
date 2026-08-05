#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

#include "IEC61850RawEthernet.h"

namespace {

IEC61850::RawEthernetFilter MakeFilter(bool vlanTagged = false) {
  IEC61850::RawEthernetFilter filter;
  filter.destinationMac = {0x01, 0x0c, 0xcd, 0x01, 0x00, 0x01};
  filter.etherType = 0x88b8;
  filter.appId = 0x1001;
  filter.vlanTagged = vlanTagged;
  filter.vlanId = vlanTagged ? 7 : 0;
  if (vlanTagged) {
    filter.vlanPriority = 4;
  }
  return filter;
}

std::vector<std::uint8_t> MakeFrame(bool vlanTagged = false) {
  const std::size_t headerSize = vlanTagged ? 18 : 14;
  std::vector<std::uint8_t> frame(headerSize + 8, 0);
  const std::array<std::uint8_t, 6> destination = {
      0x01, 0x0c, 0xcd, 0x01, 0x00, 0x01};
  std::copy(destination.begin(), destination.end(), frame.begin());
  frame[6] = 0x10;
  frame[7] = 0x11;
  frame[8] = 0x12;
  frame[9] = 0x13;
  frame[10] = 0x14;
  frame[11] = 0x15;
  if (vlanTagged) {
    frame[12] = 0x81;
    frame[13] = 0x00;
    frame[14] = 0x80;
    frame[15] = 0x07;
    frame[16] = 0x88;
    frame[17] = 0xb8;
    frame[18] = 0x10;
    frame[19] = 0x01;
  } else {
    frame[12] = 0x88;
    frame[13] = 0xb8;
    frame[14] = 0x10;
    frame[15] = 0x01;
  }
  const auto payloadOffset = vlanTagged ? 18 : 14;
  frame[payloadOffset + 2] = 0xaa;
  frame[payloadOffset + 3] = 0xbb;
  return frame;
}

}  // namespace

// 验证常见的冒号分隔MAC地址能够被解析为六字节二进制地址。
TEST(IEC61850RawEthernetTest, ParsesColonSeparatedMac) {
  const auto parsed = IEC61850::ParseRawMac("01:0c:cd:01:00:01");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed,
            (std::array<std::uint8_t, 6>{0x01, 0x0c, 0xcd, 0x01, 0x00,
                                         0x01}));
}

// 验证不带分隔符的SCL MAC地址仍然保持兼容。
TEST(IEC61850RawEthernetTest, ParsesCompactMac) {
  const auto parsed = IEC61850::ParseRawMac("010ccd010001");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ((*parsed)[0], 0x01);
  EXPECT_EQ((*parsed)[5], 0x01);
}

// 验证混用分隔符、重复分隔符和截断地址均被拒绝。
TEST(IEC61850RawEthernetTest, RejectsMalformedMacSeparators) {
  EXPECT_FALSE(IEC61850::ParseRawMac("01-0c:cd-01:00-01").has_value());
  EXPECT_FALSE(IEC61850::ParseRawMac("01::0c:cd:01:00:01").has_value());
  EXPECT_FALSE(IEC61850::ParseRawMac("01:0c:cd:01:00").has_value());
}

// 验证未标记GOOSE帧能够通过目的MAC、EtherType和APPID过滤。
TEST(IEC61850RawEthernetTest, DecodesMatchingUntaggedFrame) {
  const auto frame = MakeFrame();
  IEC61850::RawEthernetFrameView view;
  const auto status = IEC61850::DecodeRawEthernetFrame(frame, MakeFilter(),
                                                        &view);

  EXPECT_TRUE(status.ok());
  ASSERT_EQ(view.payload.size(), 8u);
  EXPECT_EQ(view.appId, 0x1001);
  EXPECT_EQ(view.etherType, 0x88b8);
  EXPECT_FALSE(view.vlanTagged);
  EXPECT_EQ(view.payload[0], 0x10);
  EXPECT_EQ(view.payload[1], 0x01);
  EXPECT_EQ(view.payload[2], 0xaa);
}

// 验证802.1Q VLAN ID会参与过滤并保留在接收视图中。
TEST(IEC61850RawEthernetTest, DecodesMatchingTaggedFrame) {
  const auto frame = MakeFrame(true);
  IEC61850::RawEthernetFrameView view;
  const auto status = IEC61850::DecodeRawEthernetFrame(
      frame, MakeFilter(true), &view);

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(view.vlanId, 7);
  EXPECT_EQ(view.vlanPriority, 4);
  EXPECT_TRUE(view.vlanTagged);
  EXPECT_EQ(view.sourceMac[0], 0x10);
  EXPECT_EQ(view.payload[2], 0xaa);
}

// 验证不匹配的APPID只被过滤，不被当成协议错误。
TEST(IEC61850RawEthernetTest, IgnoresNonMatchingAppId) {
  auto frame = MakeFrame();
  frame[14] = 0x10;
  frame[15] = 0x02;
  IEC61850::RawEthernetFrameView view;

  const auto status = IEC61850::DecodeRawEthernetFrame(frame, MakeFilter(),
                                                        &view);

  EXPECT_TRUE(status.ok());
  EXPECT_TRUE(view.payload.empty());
}

// 验证截断报文返回资源超限而不是交给上层继续解码。
TEST(IEC61850RawEthernetTest, RejectsTruncatedFrame) {
  const std::array<std::uint8_t, 10> frame{};
  IEC61850::RawEthernetFrameView view;

  const auto status = IEC61850::DecodeRawEthernetFrame(frame, MakeFilter(),
                                                        &view);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED);
  EXPECT_TRUE(view.payload.empty());
}

// 验证空视图指针被明确拒绝。
TEST(IEC61850RawEthernetTest, RejectsNullView) {
  const auto frame = MakeFrame();

  const auto status = IEC61850::DecodeRawEthernetFrame(frame, MakeFilter(),
                                                        nullptr);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证指定网卡上的AF_PACKET套接字能够真实发送并接收匹配的组播帧。
TEST(IEC61850RawEthernetTest, LiveSocketLoopbackWhenInterfaceConfigured) {
  const char* configuredInterface = std::getenv("IEC61850_LIVE_INTERFACE");
  if (configuredInterface == nullptr || configuredInterface[0] == '\0') {
    GTEST_SKIP() << "未设置IEC61850_LIVE_INTERFACE，跳过真实网卡验收";
  }

  const auto filter = MakeFilter();
  IEC61850::RawEthernetSocket receiver;
  IEC61850::RawEthernetSocket sender;
  ASSERT_TRUE(receiver.Open(configuredInterface, filter).ok());
  ASSERT_TRUE(sender.Open(configuredInterface, filter).ok());

  const auto sourceMac = sender.localMac();
  const std::array<std::uint8_t, 6> zeroMac{};
  ASSERT_NE(sourceMac, zeroMac);
  std::vector<std::uint8_t> frame(14 + 8, 0);
  std::copy(filter.destinationMac.begin(), filter.destinationMac.end(),
            frame.begin());
  std::copy(sourceMac.begin(), sourceMac.end(), frame.begin() + 6);
  frame[12] = 0x88;
  frame[13] = 0xb8;
  frame[14] = static_cast<std::uint8_t>(filter.appId >> 8);
  frame[15] = static_cast<std::uint8_t>(filter.appId & 0xff);
  frame[16] = 0x00;
  frame[17] = 0x08;
  frame[18] = 0x00;
  frame[19] = 0x00;
  frame[20] = 0x00;
  frame[21] = 0x00;

  ASSERT_TRUE(sender.Send(frame).ok());
  std::array<std::uint8_t, 256> storage{};
  IEC61850::RawEthernetFrameView view;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(1000);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto status = receiver.Receive(storage, &view);
    ASSERT_TRUE(status.ok()) << status.error_message();
    if (!view.payload.empty()) {
      EXPECT_EQ(view.appId, filter.appId);
      EXPECT_EQ(view.etherType, filter.etherType);
      EXPECT_EQ(view.payload[0], static_cast<std::uint8_t>(filter.appId >> 8));
      EXPECT_EQ(view.payload[1], static_cast<std::uint8_t>(filter.appId & 0xff));
      EXPECT_GT(view.kernelTimestampNs, 0);
      EXPECT_NE(view.timestampSource,
                IEC61850::RawEthernetTimestampSource::NONE);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  FAIL() << "真实网卡未在1秒内收到发送的AF_PACKET组播帧";
}
