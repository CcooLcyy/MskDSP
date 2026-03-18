#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "ModbusRTUSerialBus.h"

namespace {
using ModbusRTU::SerialBus;
}  // 命名空间结束

// 验证：CRC 计算结果与已知 Modbus 报文一致。
TEST(ModbusRtuSerialBusTest, ComputeCrcMatchesKnownFrame) {
  const uint8_t frame[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A};
  const uint16_t crc = SerialBus::computeCrc(frame, sizeof(frame));
  EXPECT_EQ(crc, 0xCDC5u);
}

// 验证：appendCrc 会按低字节在前的顺序追加 CRC。
TEST(ModbusRtuSerialBusTest, AppendCrcAppendsLowHighBytes) {
  std::vector<uint8_t> frame = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A};
  SerialBus::appendCrc(&frame);
  ASSERT_EQ(frame.size(), 8u);
  EXPECT_EQ(frame[6], 0xC5);
  EXPECT_EQ(frame[7], 0xCD);
}

// 验证：appendCrc 能容忍空指针输入。
TEST(ModbusRtuSerialBusTest, AppendCrcIgnoresNullPointer) {
  SerialBus::appendCrc(nullptr);
  SUCCEED();
}
