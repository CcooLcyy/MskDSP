#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "ModbusRTUSerialBus.h"

namespace {
using ModbusRTU::SerialBus;
}  // namespace

// Verifies CRC computation matches a known Modbus frame.
TEST(ModbusRtuSerialBusTest, ComputeCrcMatchesKnownFrame) {
  const uint8_t frame[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A};
  const uint16_t crc = SerialBus::computeCrc(frame, sizeof(frame));
  EXPECT_EQ(crc, 0xCDC5u);
}

// Verifies appendCrc appends CRC low byte first.
TEST(ModbusRtuSerialBusTest, AppendCrcAppendsLowHighBytes) {
  std::vector<uint8_t> frame = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A};
  SerialBus::appendCrc(&frame);
  ASSERT_EQ(frame.size(), 8u);
  EXPECT_EQ(frame[6], 0xC5);
  EXPECT_EQ(frame[7], 0xCD);
}

// Verifies appendCrc tolerates null input.
TEST(ModbusRtuSerialBusTest, AppendCrcIgnoresNullPointer) {
  SerialBus::appendCrc(nullptr);
  SUCCEED();
}
