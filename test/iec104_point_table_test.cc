#include <gtest/gtest.h>

#include "IEC104PointTable.h"

namespace {
using IEC104::PointTable;

IEC104Proto::TelemetryPoint MakePoint(const char* tag, uint32_t ioa) {
  IEC104Proto::TelemetryPoint p;
  p.set_tag(tag);
  p.set_ioa(ioa);
  p.set_type(IEC104Proto::TELEMETRY_TYPE_FLOAT);
  return p;
}
}  // namespace

// 验证：点表 replace 更新与双向查询（tag->ioa、ioa->tag）以及稳定输出顺序。
TEST(IEC104PointTableTest, ReplaceAndLookup) {
  PointTable table;

  IEC104Proto::UpsertPointTableRequest req;
  *req.add_points() = MakePoint("A", 100);
  *req.add_points() = MakePoint("B", 101);
  req.set_replace(true);

  ASSERT_TRUE(table.Upsert(req.points(), req.replace()).ok());

  auto a = table.FindByTag("A");
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->ioa, 100u);

  auto b = table.FindByIoa(101);
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(b->tag, "B");

  IEC104Proto::PointTable out;
  table.ToProto("conn-1", &out);
  EXPECT_EQ(out.conn_name(), "conn-1");
  ASSERT_EQ(out.points_size(), 2);
  EXPECT_EQ(out.points(0).tag(), "A");
  EXPECT_EQ(out.points(1).tag(), "B");
}

// 验证：点表拒绝非法点（tag/ioa/type 必填校验）。
TEST(IEC104PointTableTest, RejectsInvalidPoint) {
  PointTable table;

  IEC104Proto::UpsertPointTableRequest req;
  auto* p = req.add_points();
  p->set_tag("");
  p->set_ioa(1);
  p->set_type(IEC104Proto::TELEMETRY_TYPE_FLOAT);
  req.set_replace(true);
  auto st = table.Upsert(req.points(), req.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  IEC104Proto::UpsertPointTableRequest req2;
  auto* p2 = req2.add_points();
  p2->set_tag("A");
  p2->set_ioa(0);
  p2->set_type(IEC104Proto::TELEMETRY_TYPE_FLOAT);
  req2.set_replace(true);
  st = table.Upsert(req2.points(), req2.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  IEC104Proto::UpsertPointTableRequest req3;
  auto* p3 = req3.add_points();
  p3->set_tag("A");
  p3->set_ioa(1);
  p3->set_type(IEC104Proto::TELEMETRY_TYPE_UNSPECIFIED);
  req3.set_replace(true);
  st = table.Upsert(req3.points(), req3.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：scale=0 会规范为 1，死区能写入与序列化。
TEST(IEC104PointTableTest, NormalizesScaleAndKeepsDeadband) {
  PointTable table;

  IEC104Proto::UpsertPointTableRequest req;
  auto* p = req.add_points();
  p->set_tag("A");
  p->set_ioa(1);
  p->set_type(IEC104Proto::TELEMETRY_TYPE_FLOAT);
  p->set_scale(0.0);
  p->set_offset(-2.0);
  p->set_deadband(0.5);
  req.set_replace(true);

  ASSERT_TRUE(table.Upsert(req.points(), req.replace()).ok());

  auto updated = table.FindByTag("A");
  ASSERT_TRUE(updated.has_value());
  EXPECT_DOUBLE_EQ(updated->scale, 1.0);
  EXPECT_DOUBLE_EQ(updated->offset, -2.0);
  EXPECT_DOUBLE_EQ(updated->deadband, 0.5);

  IEC104Proto::PointTable out;
  table.ToProto("conn-1", &out);
  ASSERT_EQ(out.points_size(), 1);
  EXPECT_DOUBLE_EQ(out.points(0).scale(), 1.0);
  EXPECT_DOUBLE_EQ(out.points(0).deadband(), 0.5);
}

// 验证：死区为负时拒绝。
TEST(IEC104PointTableTest, RejectsNegativeDeadband) {
  PointTable table;

  IEC104Proto::UpsertPointTableRequest req;
  auto* p = req.add_points();
  p->set_tag("A");
  p->set_ioa(1);
  p->set_type(IEC104Proto::TELEMETRY_TYPE_FLOAT);
  p->set_deadband(-0.5);
  req.set_replace(true);

  auto st = table.Upsert(req.points(), req.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：点表拒绝冲突映射（同 tag 不同 ioa、同 ioa 不同 tag）。
TEST(IEC104PointTableTest, RejectsConflictingMappings) {
  PointTable table;

  IEC104Proto::UpsertPointTableRequest req;
  *req.add_points() = MakePoint("A", 100);
  req.set_replace(true);
  ASSERT_TRUE(table.Upsert(req.points(), req.replace()).ok());

  IEC104Proto::UpsertPointTableRequest req2;
  *req2.add_points() = MakePoint("A", 200);
  req2.set_replace(false);
  auto st = table.Upsert(req2.points(), req2.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::ALREADY_EXISTS);

  IEC104Proto::UpsertPointTableRequest req3;
  *req3.add_points() = MakePoint("B", 100);
  req3.set_replace(false);
  st = table.Upsert(req3.points(), req3.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::ALREADY_EXISTS);
}
