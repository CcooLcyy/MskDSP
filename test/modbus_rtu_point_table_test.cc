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

ModbusRTUProto::Point MakeCoilPointWithDefault(const char* tag, uint32_t address, bool value) {
  auto p = MakePoint(tag, ModbusRTUProto::FUNCTION_READ_COILS, address, ModbusRTUProto::DATA_TYPE_BOOL);
  p.set_default_bool(value);
  return p;
}

ModbusRTUProto::Point MakeRegisterPointWithDefault(const char* tag, uint32_t address, uint16_t value) {
  auto p = MakePoint(tag, ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS, address, ModbusRTUProto::DATA_TYPE_UINT16);
  p.set_default_uint16(value);
  return p;
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

// 验证：default_value 能写入点表，并可按地址查找/序列化。
TEST(ModbusRtuPointTableTest, AcceptsDefaultValuesAndFindByAddress) {
  PointTable table;

  ModbusRTUProto::UpsertPointTableRequest req;
  *req.add_points() = MakeCoilPointWithDefault("A", 10, true);
  *req.add_points() = MakeRegisterPointWithDefault("B", 20, 1234);
  req.set_replace(true);

  ASSERT_TRUE(table.Upsert(req.points(), req.replace()).ok());

  auto coil = table.FindByAddress(ModbusRTUProto::FUNCTION_READ_COILS, 10);
  ASSERT_TRUE(coil.has_value());
  EXPECT_EQ(coil->tag, "A");
  ASSERT_TRUE(coil->defaultBool.has_value());
  EXPECT_TRUE(coil->defaultBool.value());

  auto reg = table.FindByAddress(ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS, 20);
  ASSERT_TRUE(reg.has_value());
  EXPECT_EQ(reg->tag, "B");
  ASSERT_TRUE(reg->defaultUInt16.has_value());
  EXPECT_EQ(reg->defaultUInt16.value(), 1234);

  ModbusRTUProto::PointTable out;
  table.ToProto("conn-1", &out);
  ASSERT_EQ(out.points_size(), 2);
  EXPECT_EQ(out.points(0).default_value_case(), ModbusRTUProto::Point::kDefaultBool);
  EXPECT_EQ(out.points(1).default_value_case(), ModbusRTUProto::Point::kDefaultUint16);
}

// 验证：default_value 与数据类型不匹配时拒绝。
TEST(ModbusRtuPointTableTest, RejectsDefaultValueTypeMismatch) {
  PointTable table;

  ModbusRTUProto::UpsertPointTableRequest req;
  auto p1 = MakePoint("A", ModbusRTUProto::FUNCTION_READ_COILS, 1, ModbusRTUProto::DATA_TYPE_BOOL);
  p1.set_default_uint16(10);
  *req.add_points() = p1;
  req.set_replace(true);
  auto st = table.Upsert(req.points(), req.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  ModbusRTUProto::UpsertPointTableRequest req2;
  auto p2 = MakePoint("B", ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS, 2, ModbusRTUProto::DATA_TYPE_UINT16);
  p2.set_default_bool(true);
  *req2.add_points() = p2;
  req2.set_replace(true);
  st = table.Upsert(req2.points(), req2.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：default_uint16 超过 65535 时拒绝。
TEST(ModbusRtuPointTableTest, RejectsDefaultUint16OutOfRange) {
  PointTable table;

  ModbusRTUProto::UpsertPointTableRequest req;
  auto p = MakePoint("A", ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS, 1, ModbusRTUProto::DATA_TYPE_UINT16);
  p.set_default_uint16(70000);
  *req.add_points() = p;
  req.set_replace(true);

  auto st = table.Upsert(req.points(), req.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}
