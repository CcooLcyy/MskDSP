#include <gtest/gtest.h>

#include "DLT645PointTable.h"

namespace {
using DLT645::PointTable;

DLT645Proto::Point MakePoint(const char* tag,
                             const char* di,
                             uint32_t dataLen,
                             DLT645Proto::DataType type,
                             DLT645Proto::AccessMode access) {
  DLT645Proto::Point p;
  p.set_tag(tag);
  p.set_di(di);
  p.set_data_len(dataLen);
  p.set_type(type);
  p.set_access(access);
  p.set_scale(1.0);
  p.set_offset(0.0);
  p.set_deadband(0.0);
  return p;
}

DLT645Proto::BlockItem MakeBlockItem(const char* tag,
                                     uint32_t dataLen,
                                     DLT645Proto::DataType type,
                                     DLT645Proto::AccessMode access) {
  DLT645Proto::BlockItem item;
  item.set_tag(tag);
  item.set_data_len(dataLen);
  item.set_type(type);
  item.set_access(access);
  item.set_scale(1.0);
  item.set_offset(0.0);
  item.set_deadband(0.0);
  return item;
}
}  // 命名空间结束

// 验证：数据块按顺序拼接生成 offset，trim_right_space 默认裁剪且可显式关闭。
TEST(Dlt645PointTableTest, BlockOffsetsAndTrimFlags) {
  PointTable table;

  DLT645Proto::UpsertPointTableRequest req;
  auto* block = req.add_blocks();
  block->set_block_di("040000FF");
  block->set_block_data_len(5);

  auto item1 = MakeBlockItem("A", 2, DLT645Proto::DATA_TYPE_STRING, DLT645Proto::ACCESS_READ_ONLY);
  *block->add_items() = item1;
  auto item2 = MakeBlockItem("B", 3, DLT645Proto::DATA_TYPE_STRING, DLT645Proto::ACCESS_READ_ONLY);
  item2.set_trim_right_space(false);
  *block->add_items() = item2;

  req.set_replace(true);
  ASSERT_TRUE(table.Upsert(req.points(), req.blocks(), req.replace()).ok());

  const auto& blocks = table.Blocks();
  ASSERT_EQ(blocks.size(), 1u);
  EXPECT_EQ(blocks[0].diText, "040000FF");
  ASSERT_EQ(blocks[0].items.size(), 2u);
  EXPECT_EQ(blocks[0].items[0].offset, 0u);
  EXPECT_EQ(blocks[0].items[1].offset, 2u);
  EXPECT_TRUE(blocks[0].items[0].trimRightSpace);
  EXPECT_FALSE(blocks[0].items[1].trimRightSpace);
}

// 验证：BOOL 点位支持按数据域 byte_index/bit_index 取位，同一 DI 不同 bit 可映射多个遥信。
TEST(Dlt645PointTableTest, AcceptsBoolBitPointsWithSameDi) {
  PointTable table;

  DLT645Proto::UpsertPointTableRequest req;
  auto bit0 = MakePoint("DI0", "02010100", 2, DLT645Proto::DATA_TYPE_BOOL, DLT645Proto::ACCESS_READ_ONLY);
  bit0.set_byte_index(0);
  bit0.set_bit_index(0);
  *req.add_points() = bit0;

  auto bit9 = MakePoint("DI9", "02010100", 2, DLT645Proto::DATA_TYPE_BOOL, DLT645Proto::ACCESS_READ_ONLY);
  bit9.set_byte_index(1);
  bit9.set_bit_index(1);
  *req.add_points() = bit9;
  req.set_replace(true);

  ASSERT_TRUE(table.Upsert(req.points(), req.blocks(), req.replace()).ok());

  auto stored = table.FindByTag("DI9");
  ASSERT_TRUE(stored.has_value());
  ASSERT_TRUE(stored->byteIndex.has_value());
  ASSERT_TRUE(stored->bitIndex.has_value());
  EXPECT_EQ(stored->byteIndex.value(), 1u);
  EXPECT_EQ(stored->bitIndex.value(), 1u);

  DLT645Proto::PointTable out;
  table.ToProto("conn-bit", &out);
  ASSERT_EQ(out.points_size(), 2);
  EXPECT_TRUE(out.points(0).has_bit_index());
  EXPECT_TRUE(out.points(1).has_bit_index());
}

// 验证：DLT645 BOOL bit 点位拒绝越界 bit、缺少 bit_index 的 byte_index 和同 bit 重复映射。
TEST(Dlt645PointTableTest, RejectsInvalidBoolBitPoints) {
  PointTable table;

  DLT645Proto::UpsertPointTableRequest byteWithoutBit;
  auto p1 = MakePoint("A", "02010100", 1, DLT645Proto::DATA_TYPE_BOOL, DLT645Proto::ACCESS_READ_ONLY);
  p1.set_byte_index(0);
  *byteWithoutBit.add_points() = p1;
  byteWithoutBit.set_replace(true);
  auto st = table.Upsert(byteWithoutBit.points(), byteWithoutBit.blocks(), byteWithoutBit.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  DLT645Proto::UpsertPointTableRequest bitOutOfRange;
  auto p2 = MakePoint("B", "02010100", 1, DLT645Proto::DATA_TYPE_BOOL, DLT645Proto::ACCESS_READ_ONLY);
  p2.set_bit_index(8);
  *bitOutOfRange.add_points() = p2;
  bitOutOfRange.set_replace(true);
  st = table.Upsert(bitOutOfRange.points(), bitOutOfRange.blocks(), bitOutOfRange.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  DLT645Proto::UpsertPointTableRequest duplicate;
  auto p3 = MakePoint("C", "02010100", 1, DLT645Proto::DATA_TYPE_BOOL, DLT645Proto::ACCESS_READ_ONLY);
  p3.set_bit_index(0);
  *duplicate.add_points() = p3;
  auto p4 = MakePoint("D", "02010100", 1, DLT645Proto::DATA_TYPE_BOOL, DLT645Proto::ACCESS_READ_ONLY);
  p4.set_bit_index(0);
  *duplicate.add_points() = p4;
  duplicate.set_replace(true);
  st = table.Upsert(duplicate.points(), duplicate.blocks(), duplicate.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::ALREADY_EXISTS);
}

// 验证：数据块长度与子项长度之和不一致时拒绝。
TEST(Dlt645PointTableTest, RejectsBlockLengthMismatch) {
  PointTable table;

  DLT645Proto::UpsertPointTableRequest req;
  auto* block = req.add_blocks();
  block->set_block_di("040000FF");
  block->set_block_data_len(4);
  *block->add_items() = MakeBlockItem("A", 2, DLT645Proto::DATA_TYPE_UINT16, DLT645Proto::ACCESS_READ_ONLY);
  *block->add_items() = MakeBlockItem("B", 3, DLT645Proto::DATA_TYPE_UINT16, DLT645Proto::ACCESS_READ_ONLY);
  req.set_replace(true);

  auto st = table.Upsert(req.points(), req.blocks(), req.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：同一 tag 不允许在多个数据块中重复定义。
TEST(Dlt645PointTableTest, RejectsDuplicateTagAcrossBlocks) {
  PointTable table;

  DLT645Proto::UpsertPointTableRequest req;
  auto* block1 = req.add_blocks();
  block1->set_block_di("040000FF");
  block1->set_block_data_len(2);
  *block1->add_items() = MakeBlockItem("A", 2, DLT645Proto::DATA_TYPE_UINT16, DLT645Proto::ACCESS_READ_ONLY);

  auto* block2 = req.add_blocks();
  block2->set_block_di("040001FF");
  block2->set_block_data_len(2);
  *block2->add_items() = MakeBlockItem("A", 2, DLT645Proto::DATA_TYPE_UINT16, DLT645Proto::ACCESS_READ_ONLY);
  req.set_replace(true);

  auto st = table.Upsert(req.points(), req.blocks(), req.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::ALREADY_EXISTS);
}

// 验证：数据块与单点定义不一致时拒绝。
TEST(Dlt645PointTableTest, RejectsMismatchBetweenPointAndBlock) {
  PointTable table;

  DLT645Proto::UpsertPointTableRequest req;
  *req.add_points() = MakePoint("A", "02010100", 2, DLT645Proto::DATA_TYPE_UINT16, DLT645Proto::ACCESS_READ_WRITE);

  auto* block = req.add_blocks();
  block->set_block_di("040000FF");
  block->set_block_data_len(3);
  *block->add_items() = MakeBlockItem("A", 3, DLT645Proto::DATA_TYPE_UINT16, DLT645Proto::ACCESS_READ_ONLY);
  req.set_replace(true);

  auto st = table.Upsert(req.points(), req.blocks(), req.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：读块写点允许共存（定义一致时可通过）。
TEST(Dlt645PointTableTest, AllowsReadBlockWritePointWithSameDefinition) {
  PointTable table;

  DLT645Proto::UpsertPointTableRequest req;
  *req.add_points() = MakePoint("A", "02010100", 2, DLT645Proto::DATA_TYPE_UINT16, DLT645Proto::ACCESS_READ_WRITE);

  auto* block = req.add_blocks();
  block->set_block_di("040000FF");
  block->set_block_data_len(2);
  *block->add_items() = MakeBlockItem("A", 2, DLT645Proto::DATA_TYPE_UINT16, DLT645Proto::ACCESS_READ_ONLY);
  req.set_replace(true);

  ASSERT_TRUE(table.Upsert(req.points(), req.blocks(), req.replace()).ok());
  EXPECT_TRUE(table.FindByTag("A").has_value());
  EXPECT_TRUE(table.BlockTags().find("A") != table.BlockTags().end());
}
