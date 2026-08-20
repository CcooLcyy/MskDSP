#include <gtest/gtest.h>

#include "IEC104PointTable.h"

namespace {
using IEC104::PointTable;

IEC104Proto::Point MakePoint(const char* tag, uint32_t ioa) {
  IEC104Proto::Point p;
  p.set_tag(tag);
  p.set_ioa(ioa);
  p.set_type(IEC104Proto::POINT_TYPE_FLOAT);
  return p;
}
}  // 命名空间结束

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
  p->set_type(IEC104Proto::POINT_TYPE_FLOAT);
  req.set_replace(true);
  auto st = table.Upsert(req.points(), req.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  IEC104Proto::UpsertPointTableRequest req2;
  auto* p2 = req2.add_points();
  p2->set_tag("A");
  p2->set_ioa(0);
  p2->set_type(IEC104Proto::POINT_TYPE_FLOAT);
  req2.set_replace(true);
  st = table.Upsert(req2.points(), req2.replace());
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  IEC104Proto::UpsertPointTableRequest req3;
  auto* p3 = req3.add_points();
  p3->set_tag("A");
  p3->set_ioa(1);
  p3->set_type(IEC104Proto::POINT_TYPE_UNSPECIFIED);
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
  p->set_type(IEC104Proto::POINT_TYPE_FLOAT);
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

// 验证：显式业务类型会随点表保存和查询，并允许业务语义与协议数据类型独立表达。
TEST(IEC104PointTableTest, KeepsExplicitBusinessType) {
  PointTable table;

  IEC104Proto::UpsertPointTableRequest req;
  auto* point = req.add_points();
  point->set_tag("setpoint");
  point->set_ioa(0x6201);
  point->set_type(IEC104Proto::POINT_TYPE_FLOAT);
  point->set_business_type(IEC104Proto::POINT_BUSINESS_TYPE_REMOTE_ADJUST);
  req.set_replace(true);

  ASSERT_TRUE(table.Upsert(req.points(), req.replace()).ok());
  const auto stored = table.FindByTag("setpoint");
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->businessType, IEC104Proto::POINT_BUSINESS_TYPE_REMOTE_ADJUST);

  IEC104Proto::PointTable out;
  table.ToProto("conn-1", &out);
  ASSERT_EQ(out.points_size(), 1);
  EXPECT_EQ(out.points(0).business_type(), IEC104Proto::POINT_BUSINESS_TYPE_REMOTE_ADJUST);
}

// 验证：旧点表未携带业务类型时，已知 IOA 区间会推导出对应业务类型，未分类地址保持未指定。
TEST(IEC104PointTableTest, InfersLegacyBusinessTypeFromIoaRange) {
  PointTable table;

  IEC104Proto::UpsertPointTableRequest req;
  auto* remoteAdjust = req.add_points();
  remoteAdjust->set_tag("legacy-setpoint");
  remoteAdjust->set_ioa(0x6201);
  remoteAdjust->set_type(IEC104Proto::POINT_TYPE_FLOAT);
  auto* remoteControl = req.add_points();
  remoteControl->set_tag("legacy-command");
  remoteControl->set_ioa(0x8000);
  remoteControl->set_type(IEC104Proto::POINT_TYPE_SINGLE);
  auto* custom = req.add_points();
  custom->set_tag("legacy-custom");
  custom->set_ioa(0xC000);
  custom->set_type(IEC104Proto::POINT_TYPE_FLOAT);
  req.set_replace(true);

  ASSERT_TRUE(table.Upsert(req.points(), req.replace()).ok());
  EXPECT_EQ(table.FindByTag("legacy-setpoint")->businessType,
            IEC104Proto::POINT_BUSINESS_TYPE_REMOTE_ADJUST);
  EXPECT_EQ(table.FindByTag("legacy-command")->businessType,
            IEC104Proto::POINT_BUSINESS_TYPE_REMOTE_CONTROL);
  EXPECT_EQ(table.FindByTag("legacy-custom")->businessType,
            IEC104Proto::POINT_BUSINESS_TYPE_UNSPECIFIED);
}

// 验证：单点类型忽略 scale/offset/deadband，并归一为默认值。
TEST(IEC104PointTableTest, SinglePointIgnoresScaleOffsetDeadband) {
  PointTable table;

  IEC104Proto::UpsertPointTableRequest req;
  auto* p = req.add_points();
  p->set_tag("S");
  p->set_ioa(10);
  p->set_type(IEC104Proto::POINT_TYPE_SINGLE);
  p->set_scale(2.0);
  p->set_offset(3.0);
  p->set_deadband(1.2);
  req.set_replace(true);

  ASSERT_TRUE(table.Upsert(req.points(), req.replace()).ok());

  auto updated = table.FindByTag("S");
  ASSERT_TRUE(updated.has_value());
  EXPECT_EQ(updated->type, IEC104Proto::POINT_TYPE_SINGLE);
  EXPECT_DOUBLE_EQ(updated->scale, 1.0);
  EXPECT_DOUBLE_EQ(updated->offset, 0.0);
  EXPECT_DOUBLE_EQ(updated->deadband, 0.0);
}

// 验证：死区为负时拒绝。
TEST(IEC104PointTableTest, RejectsNegativeDeadband) {
  PointTable table;

  IEC104Proto::UpsertPointTableRequest req;
  auto* p = req.add_points();
  p->set_tag("A");
  p->set_ioa(1);
  p->set_type(IEC104Proto::POINT_TYPE_FLOAT);
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
