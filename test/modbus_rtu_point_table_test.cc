#include <gtest/gtest.h>

#include "ModbusRTUPointTable.h"

namespace {
using ModbusRTU::PointTable;

ModbusRTUProto::Point MakePoint(const char* tag,
                                ModbusRTUProto::FunctionCode function,
                                uint32_t address,
                                ModbusRTUProto::DataType type) {
  ModbusRTUProto::Point p;
  p.set_tag(tag);
  p.set_function(function);
  p.set_address(address);
  p.set_type(type);
  return p;
}

ModbusRTUProto::Point MakeUint32RegisterPoint(const char* tag, uint32_t address) {
  auto p = MakePoint(tag, ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS, address, ModbusRTUProto::DATA_TYPE_UINT32);
  p.set_reg_count(2);
  return p;
}

ModbusRTUProto::Point MakeInputRegisterPoint(const char* tag, uint32_t address, ModbusRTUProto::DataType type) {
  return MakePoint(tag, ModbusRTUProto::FUNCTION_READ_INPUT_REGISTERS, address, type);
}
}  // namespace

// 验证：点表 replace 更新与 tag 查询、ToProto 输出排序。
TEST(ModbusRtuPointTableTest, ReplaceAndLookup) {
  PointTable table;

  ModbusRTUProto::UpsertPointTableRequest req;
  *req.add_points() = MakePoint("A", ModbusRTUProto::FUNCTION_READ_COILS, 1, ModbusRTUProto::DATA_TYPE_BOOL);
  *req.add_points() = MakePoint("B", ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS, 2, ModbusRTUProto::DATA_TYPE_UINT16);
  req.set_replace(true);

  ASSERT_TRUE(table.Upsert(req.points(), req.replace()).ok());

  auto a = table.FindByTag("A");
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->address, 1u);
  EXPECT_EQ(a->function, ModbusRTUProto::FUNCTION_READ_COILS);

  ModbusRTUProto::PointTable out;
  table.ToProto("conn-1", &out);
  EXPECT_EQ(out.conn_name(), "conn-1");
  ASSERT_EQ(out.points_size(), 2);
  EXPECT_EQ(out.points(0).tag(), "A");
  EXPECT_EQ(out.points(1).tag(), "B");
}

// 验证：点表拒绝非法点（tag/function/type/address 与函数类型匹配）。
TEST(ModbusRtuPointTableTest, RejectsInvalidPoint) {
  PointTable table;

  ModbusRTUProto::UpsertPointTableRequest req;
  *req.add_points() = MakePoint("", ModbusRTUProto::FUNCTION_READ_COILS, 1, ModbusRTUProto::DATA_TYPE_BOOL);
  req.set_replace(true);
  auto st = table.Upsert(req.points(), req.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  ModbusRTUProto::UpsertPointTableRequest req2;
  *req2.add_points() = MakePoint("A", ModbusRTUProto::FUNCTION_UNSPECIFIED, 1, ModbusRTUProto::DATA_TYPE_BOOL);
  req2.set_replace(true);
  st = table.Upsert(req2.points(), req2.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  ModbusRTUProto::UpsertPointTableRequest req3;
  *req3.add_points() = MakePoint("A", ModbusRTUProto::FUNCTION_READ_COILS, 1, ModbusRTUProto::DATA_TYPE_UNSPECIFIED);
  req3.set_replace(true);
  st = table.Upsert(req3.points(), req3.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  ModbusRTUProto::UpsertPointTableRequest req4;
  *req4.add_points() = MakePoint("A", ModbusRTUProto::FUNCTION_READ_COILS, 70000, ModbusRTUProto::DATA_TYPE_BOOL);
  req4.set_replace(true);
  st = table.Upsert(req4.points(), req4.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  ModbusRTUProto::UpsertPointTableRequest req5;
  *req5.add_points() = MakePoint("A", ModbusRTUProto::FUNCTION_READ_COILS, 1, ModbusRTUProto::DATA_TYPE_UINT16);
  req5.set_replace(true);
  st = table.Upsert(req5.points(), req5.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  ModbusRTUProto::UpsertPointTableRequest req6;
  *req6.add_points() = MakePoint("A", ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS, 1, ModbusRTUProto::DATA_TYPE_BOOL);
  req6.set_replace(true);
  st = table.Upsert(req6.points(), req6.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  ModbusRTUProto::UpsertPointTableRequest req7;
  *req7.add_points() = MakeInputRegisterPoint("C", 3, ModbusRTUProto::DATA_TYPE_BOOL);
  req7.set_replace(true);
  st = table.Upsert(req7.points(), req7.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：点表拒绝冲突映射（同 tag 不同地址、同地址不同 tag）。
TEST(ModbusRtuPointTableTest, RejectsConflictingMappings) {
  PointTable table;

  ModbusRTUProto::UpsertPointTableRequest req;
  *req.add_points() = MakePoint("A", ModbusRTUProto::FUNCTION_READ_COILS, 1, ModbusRTUProto::DATA_TYPE_BOOL);
  req.set_replace(true);
  ASSERT_TRUE(table.Upsert(req.points(), req.replace()).ok());

  ModbusRTUProto::UpsertPointTableRequest req2;
  *req2.add_points() = MakePoint("A", ModbusRTUProto::FUNCTION_READ_COILS, 2, ModbusRTUProto::DATA_TYPE_BOOL);
  req2.set_replace(false);
  auto st = table.Upsert(req2.points(), req2.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::ALREADY_EXISTS);

  ModbusRTUProto::UpsertPointTableRequest req3;
  *req3.add_points() = MakePoint("B", ModbusRTUProto::FUNCTION_READ_COILS, 1, ModbusRTUProto::DATA_TYPE_BOOL);
  req3.set_replace(false);
  st = table.Upsert(req3.points(), req3.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::ALREADY_EXISTS);
}

// 验证：点表支持按功能码与地址查找，并保持序列化后的点信息。
TEST(ModbusRtuPointTableTest, FindsConfiguredPointsByAddress) {
  PointTable table;

  ModbusRTUProto::UpsertPointTableRequest req;
  *req.add_points() = MakePoint("A", ModbusRTUProto::FUNCTION_READ_COILS, 10, ModbusRTUProto::DATA_TYPE_BOOL);
  *req.add_points() = MakePoint("B", ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS, 20, ModbusRTUProto::DATA_TYPE_UINT16);
  req.set_replace(true);

  ASSERT_TRUE(table.Upsert(req.points(), req.replace()).ok());

  auto coil = table.FindByAddress(ModbusRTUProto::FUNCTION_READ_COILS, 10);
  ASSERT_TRUE(coil.has_value());
  EXPECT_EQ(coil->tag, "A");

  auto reg = table.FindByAddress(ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS, 20);
  ASSERT_TRUE(reg.has_value());
  EXPECT_EQ(reg->tag, "B");

  ModbusRTUProto::PointTable out;
  table.ToProto("conn-1", &out);
  ASSERT_EQ(out.points_size(), 2);
  EXPECT_EQ(out.points(0).tag(), "A");
  EXPECT_EQ(out.points(1).tag(), "B");
}

// 验证：replace=false 时支持合并并更新已有点的 scale/offset。
TEST(ModbusRtuPointTableTest, UpsertMergeUpdatesExistingFields) {
  PointTable table;

  ModbusRTUProto::UpsertPointTableRequest first;
  auto p1 = MakePoint("A", ModbusRTUProto::FUNCTION_READ_COILS, 1, ModbusRTUProto::DATA_TYPE_BOOL);
  p1.set_scale(1.0);
  p1.set_offset(2.0);
  *first.add_points() = p1;
  first.set_replace(true);
  ASSERT_TRUE(table.Upsert(first.points(), first.replace()).ok());

  ModbusRTUProto::UpsertPointTableRequest second;
  auto p2 = MakePoint("A", ModbusRTUProto::FUNCTION_READ_COILS, 1, ModbusRTUProto::DATA_TYPE_BOOL);
  p2.set_scale(2.5);
  p2.set_offset(-1.0);
  *second.add_points() = p2;
  *second.add_points() = MakePoint("C",
                                   ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS,
                                   2,
                                   ModbusRTUProto::DATA_TYPE_UINT16);
  second.set_replace(false);

  ASSERT_TRUE(table.Upsert(second.points(), second.replace()).ok());

  auto updated = table.FindByTag("A");
  ASSERT_TRUE(updated.has_value());
  EXPECT_DOUBLE_EQ(updated->scale, 2.5);
  EXPECT_DOUBLE_EQ(updated->offset, -1.0);

  auto tags = table.Tags();
  ASSERT_EQ(tags.size(), 2u);
  EXPECT_EQ(tags[0], "A");
  EXPECT_EQ(tags[1], "C");
}

// 验证：scale=0 会规范为 1，死区能写入与序列化。
TEST(ModbusRtuPointTableTest, NormalizesScaleAndKeepsDeadband) {
  PointTable table;

  ModbusRTUProto::UpsertPointTableRequest req;
  auto p = MakePoint("A", ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS, 1, ModbusRTUProto::DATA_TYPE_UINT16);
  p.set_scale(0.0);
  p.set_offset(1.5);
  p.set_deadband(2.0);
  *req.add_points() = p;
  req.set_replace(true);

  ASSERT_TRUE(table.Upsert(req.points(), req.replace()).ok());

  auto updated = table.FindByTag("A");
  ASSERT_TRUE(updated.has_value());
  EXPECT_DOUBLE_EQ(updated->scale, 1.0);
  EXPECT_DOUBLE_EQ(updated->offset, 1.5);
  EXPECT_DOUBLE_EQ(updated->deadband, 2.0);

  ModbusRTUProto::PointTable out;
  table.ToProto("conn-1", &out);
  ASSERT_EQ(out.points_size(), 1);
  EXPECT_DOUBLE_EQ(out.points(0).scale(), 1.0);
  EXPECT_DOUBLE_EQ(out.points(0).deadband(), 2.0);
}

// 验证：死区为负时拒绝。
TEST(ModbusRtuPointTableTest, RejectsNegativeDeadband) {
  PointTable table;

  ModbusRTUProto::UpsertPointTableRequest req;
  auto p = MakePoint("A", ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS, 1, ModbusRTUProto::DATA_TYPE_UINT16);
  p.set_deadband(-0.1);
  *req.add_points() = p;
  req.set_replace(true);

  auto st = table.Upsert(req.points(), req.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：replace=true 会清理已有点表，只保留新的点集合。
TEST(ModbusRtuPointTableTest, ReplaceClearsExistingPoints) {
  PointTable table;

  ModbusRTUProto::UpsertPointTableRequest first;
  *first.add_points() = MakePoint("B", ModbusRTUProto::FUNCTION_READ_COILS, 1, ModbusRTUProto::DATA_TYPE_BOOL);
  *first.add_points() = MakePoint("A", ModbusRTUProto::FUNCTION_READ_COILS, 2, ModbusRTUProto::DATA_TYPE_BOOL);
  first.set_replace(true);
  ASSERT_TRUE(table.Upsert(first.points(), first.replace()).ok());

  ModbusRTUProto::UpsertPointTableRequest second;
  *second.add_points() = MakePoint("C", ModbusRTUProto::FUNCTION_READ_COILS, 3, ModbusRTUProto::DATA_TYPE_BOOL);
  second.set_replace(true);
  ASSERT_TRUE(table.Upsert(second.points(), second.replace()).ok());

  EXPECT_FALSE(table.FindByTag("A").has_value());
  EXPECT_FALSE(table.FindByTag("B").has_value());
  ASSERT_TRUE(table.FindByTag("C").has_value());
}

// 验证：UINT32 点位支持双寄存器映射，并可按寄存器地址查找。
TEST(ModbusRtuPointTableTest, AcceptsUint32PointAndRegisterLookup) {
  PointTable table;

  ModbusRTUProto::UpsertPointTableRequest req;
  auto p = MakeUint32RegisterPoint("P32", 100);
  p.set_word_order(ModbusRTUProto::WORD_ORDER_LH);
  p.set_byte_order(ModbusRTUProto::BYTE_ORDER_BA);
  *req.add_points() = p;
  req.set_replace(true);

  ASSERT_TRUE(table.Upsert(req.points(), req.replace()).ok());

  auto first = table.FindRegisterByAddress(ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS, 100);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->point.tag, "P32");
  EXPECT_EQ(first->wordIndex, 0u);

  auto second = table.FindRegisterByAddress(ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS, 101);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->point.tag, "P32");
  EXPECT_EQ(second->wordIndex, 1u);

  auto stored = table.FindByTag("P32");
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->regCount, 2u);
  EXPECT_EQ(stored->wordOrder, ModbusRTUProto::WORD_ORDER_LH);
  EXPECT_EQ(stored->byteOrder, ModbusRTUProto::BYTE_ORDER_BA);

  ModbusRTUProto::PointTable out;
  table.ToProto("conn-1", &out);
  ASSERT_EQ(out.points_size(), 1);
  EXPECT_EQ(out.points(0).type(), ModbusRTUProto::DATA_TYPE_UINT32);
  EXPECT_EQ(out.points(0).reg_count(), 2u);
  EXPECT_EQ(out.points(0).word_order(), ModbusRTUProto::WORD_ORDER_LH);
  EXPECT_EQ(out.points(0).byte_order(), ModbusRTUProto::BYTE_ORDER_BA);
}

// 验证：输入寄存器点位支持 UINT16/UINT32，并按独立功能码地址查找。
TEST(ModbusRtuPointTableTest, AcceptsInputRegisterPoints) {
  PointTable table;

  ModbusRTUProto::UpsertPointTableRequest req;
  auto p16 = MakeInputRegisterPoint("IR16", 400, ModbusRTUProto::DATA_TYPE_UINT16);
  *req.add_points() = p16;

  auto p32 = MakeInputRegisterPoint("IR32", 500, ModbusRTUProto::DATA_TYPE_UINT32);
  p32.set_reg_count(2);
  *req.add_points() = p32;
  req.set_replace(true);

  ASSERT_TRUE(table.Upsert(req.points(), req.replace()).ok());

  auto p16Stored = table.FindByAddress(ModbusRTUProto::FUNCTION_READ_INPUT_REGISTERS, 400);
  ASSERT_TRUE(p16Stored.has_value());
  EXPECT_EQ(p16Stored->tag, "IR16");

  auto p32First = table.FindRegisterByAddress(ModbusRTUProto::FUNCTION_READ_INPUT_REGISTERS, 500);
  ASSERT_TRUE(p32First.has_value());
  EXPECT_EQ(p32First->point.tag, "IR32");
  EXPECT_EQ(p32First->wordIndex, 0u);

  auto p32Second = table.FindRegisterByAddress(ModbusRTUProto::FUNCTION_READ_INPUT_REGISTERS, 501);
  ASSERT_TRUE(p32Second.has_value());
  EXPECT_EQ(p32Second->point.tag, "IR32");
  EXPECT_EQ(p32Second->wordIndex, 1u);
}

// 验证：寄存器点位支持 INT16/INT32，并保留寄存器配置参数。
TEST(ModbusRtuPointTableTest, AcceptsSignedRegisterPoints) {
  PointTable table;

  ModbusRTUProto::UpsertPointTableRequest req;
  auto p16 = MakePoint("S16", ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS, 600, ModbusRTUProto::DATA_TYPE_INT16);
  p16.set_byte_order(ModbusRTUProto::BYTE_ORDER_BA);
  *req.add_points() = p16;

  auto p32 = MakePoint("S32", ModbusRTUProto::FUNCTION_READ_INPUT_REGISTERS, 700, ModbusRTUProto::DATA_TYPE_INT32);
  p32.set_reg_count(2);
  p32.set_word_order(ModbusRTUProto::WORD_ORDER_LH);
  p32.set_byte_order(ModbusRTUProto::BYTE_ORDER_BA);
  *req.add_points() = p32;
  req.set_replace(true);

  ASSERT_TRUE(table.Upsert(req.points(), req.replace()).ok());

  auto stored16 = table.FindByTag("S16");
  ASSERT_TRUE(stored16.has_value());
  EXPECT_EQ(stored16->type, ModbusRTUProto::DATA_TYPE_INT16);
  EXPECT_EQ(stored16->regCount, 1u);
  EXPECT_EQ(stored16->byteOrder, ModbusRTUProto::BYTE_ORDER_BA);

  auto stored32 = table.FindByTag("S32");
  ASSERT_TRUE(stored32.has_value());
  EXPECT_EQ(stored32->type, ModbusRTUProto::DATA_TYPE_INT32);
  EXPECT_EQ(stored32->regCount, 2u);
  EXPECT_EQ(stored32->wordOrder, ModbusRTUProto::WORD_ORDER_LH);
  EXPECT_EQ(stored32->byteOrder, ModbusRTUProto::BYTE_ORDER_BA);

  auto first = table.FindRegisterByAddress(ModbusRTUProto::FUNCTION_READ_INPUT_REGISTERS, 700);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->point.tag, "S32");
  EXPECT_EQ(first->wordIndex, 0u);

  auto second = table.FindRegisterByAddress(ModbusRTUProto::FUNCTION_READ_INPUT_REGISTERS, 701);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->point.tag, "S32");
  EXPECT_EQ(second->wordIndex, 1u);

  ModbusRTUProto::PointTable out;
  table.ToProto("conn-signed", &out);
  ASSERT_EQ(out.points_size(), 2);
  EXPECT_EQ(out.points(0).type(), ModbusRTUProto::DATA_TYPE_INT16);
  EXPECT_EQ(out.points(0).reg_count(), 1u);
  EXPECT_EQ(out.points(0).byte_order(), ModbusRTUProto::BYTE_ORDER_BA);
  EXPECT_EQ(out.points(1).type(), ModbusRTUProto::DATA_TYPE_INT32);
  EXPECT_EQ(out.points(1).reg_count(), 2u);
  EXPECT_EQ(out.points(1).word_order(), ModbusRTUProto::WORD_ORDER_LH);
  EXPECT_EQ(out.points(1).byte_order(), ModbusRTUProto::BYTE_ORDER_BA);
}

// 验证：UINT16/UINT32 默认 reg_count 与字节/字序能被规范化。
TEST(ModbusRtuPointTableTest, NormalizesRegisterDefaultsForUintTypes) {
  PointTable table;

  ModbusRTUProto::UpsertPointTableRequest req;
  auto p32 = MakePoint("U32", ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS, 200, ModbusRTUProto::DATA_TYPE_UINT32);
  p32.set_reg_count(0);
  p32.set_word_order(ModbusRTUProto::WORD_ORDER_UNSPECIFIED);
  p32.set_byte_order(ModbusRTUProto::BYTE_ORDER_UNSPECIFIED);
  *req.add_points() = p32;

  auto p16 = MakePoint("U16", ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS, 300, ModbusRTUProto::DATA_TYPE_UINT16);
  p16.set_reg_count(0);
  p16.set_byte_order(ModbusRTUProto::BYTE_ORDER_UNSPECIFIED);
  *req.add_points() = p16;
  req.set_replace(true);

  ASSERT_TRUE(table.Upsert(req.points(), req.replace()).ok());

  auto stored32 = table.FindByTag("U32");
  ASSERT_TRUE(stored32.has_value());
  EXPECT_EQ(stored32->regCount, 2u);
  EXPECT_EQ(stored32->wordOrder, ModbusRTUProto::WORD_ORDER_HL);
  EXPECT_EQ(stored32->byteOrder, ModbusRTUProto::BYTE_ORDER_AB);

  auto stored16 = table.FindByTag("U16");
  ASSERT_TRUE(stored16.has_value());
  EXPECT_EQ(stored16->regCount, 1u);
  EXPECT_EQ(stored16->byteOrder, ModbusRTUProto::BYTE_ORDER_AB);
}

// 验证：非法 reg_count 或字序/字节序会被拒绝。
TEST(ModbusRtuPointTableTest, RejectsInvalidRegisterConfig) {
  PointTable table;

  ModbusRTUProto::UpsertPointTableRequest req1;
  auto p1 = MakePoint("A", ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS, 1, ModbusRTUProto::DATA_TYPE_UINT16);
  p1.set_reg_count(2);
  *req1.add_points() = p1;
  req1.set_replace(true);
  auto st = table.Upsert(req1.points(), req1.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  ModbusRTUProto::UpsertPointTableRequest req2;
  auto p2 = MakePoint("B", ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS, 10, ModbusRTUProto::DATA_TYPE_UINT32);
  p2.set_reg_count(1);
  *req2.add_points() = p2;
  req2.set_replace(true);
  st = table.Upsert(req2.points(), req2.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  ModbusRTUProto::UpsertPointTableRequest req3;
  auto p3 = MakePoint("C", ModbusRTUProto::FUNCTION_READ_COILS, 2, ModbusRTUProto::DATA_TYPE_BOOL);
  p3.set_reg_count(2);
  *req3.add_points() = p3;
  req3.set_replace(true);
  st = table.Upsert(req3.points(), req3.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  ModbusRTUProto::UpsertPointTableRequest req4;
  auto p4 = MakePoint("D", ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS, 3, ModbusRTUProto::DATA_TYPE_UINT16);
  p4.set_reg_count(3);
  *req4.add_points() = p4;
  req4.set_replace(true);
  st = table.Upsert(req4.points(), req4.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  ModbusRTUProto::UpsertPointTableRequest req5;
  auto p5 = MakePoint("E", ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS, 4, ModbusRTUProto::DATA_TYPE_UINT32);
  p5.set_reg_count(2);
  p5.set_word_order(static_cast<ModbusRTUProto::WordOrder>(99));
  *req5.add_points() = p5;
  req5.set_replace(true);
  st = table.Upsert(req5.points(), req5.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  ModbusRTUProto::UpsertPointTableRequest req6;
  auto p6 = MakePoint("F", ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS, 5, ModbusRTUProto::DATA_TYPE_UINT16);
  p6.set_reg_count(1);
  p6.set_byte_order(static_cast<ModbusRTUProto::ByteOrder>(99));
  *req6.add_points() = p6;
  req6.set_replace(true);
  st = table.Upsert(req6.points(), req6.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}
