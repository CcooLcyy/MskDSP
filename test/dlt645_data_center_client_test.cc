#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "DLT645DataCenterClient.h"
#include "support/FakeDataCenter.hpp"

namespace {
using DLT645::DataCenterClient;
}  // namespace

// 验证：ConnectionExists 入参为空时返回错误。
TEST(Dlt645DataCenterClientTest, ConnectionExistsRejectsEmptyName) {
  DataCenterClient client("DLT645");
  bool exists = false;
  auto st = client.ConnectionExists("", &exists);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：ConnectionExists 可查到已存在连接。
TEST(Dlt645DataCenterClientTest, ConnectionExistsFindsMatch) {
  FakeDataCenterState state;
  state.AddConnection(1, "DLT645", "conn-1");
  auto stub = MakeStub(&state);

  DataCenterClient client("DLT645");
  client.setStub(stub);

  bool exists = false;
  auto st = client.ConnectionExists("conn-1", &exists);
  EXPECT_TRUE(st.ok());
  EXPECT_TRUE(exists);
}

// 验证：GetOrCreateConnection 参数非法时返回错误。
TEST(Dlt645DataCenterClientTest, GetOrCreateConnectionRejectsInvalidArgs) {
  DataCenterClient client("DLT645");
  DataCenterProto::ConnectionInfo info;
  auto st = client.GetOrCreateConnection("", &info);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  st = client.GetOrCreateConnection("conn", nullptr);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：Publish/GetLatest 等接口参数为空时返回错误。
TEST(Dlt645DataCenterClientTest, PublishAndGetLatestValidateArgs) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  DataCenterClient client("DLT645");
  client.setStub(stub);

  auto st = client.PublishBool(0, "tag", true, DataCenterProto::QUALITY_GOOD, 0);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  st = client.PublishUInt16(1, "", 1, DataCenterProto::QUALITY_GOOD, 0);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  st = client.PublishDouble(1, "", 1.0, DataCenterProto::QUALITY_GOOD, 0);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  st = client.PublishString(1, "", "v", DataCenterProto::QUALITY_GOOD, 0);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  DataCenterProto::GetLatestResponse resp;
  st = client.GetLatest(0, {"tag"}, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  st = client.GetLatest(1, {""}, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：Publish 与 GetLatest 正常路径可执行。
TEST(Dlt645DataCenterClientTest, PublishAndGetLatestSuccess) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  DataCenterClient client("DLT645");
  client.setStub(stub);

  auto st = client.PublishBool(1, "tag", true, DataCenterProto::QUALITY_GOOD, 123);
  EXPECT_TRUE(st.ok());

  DataCenterProto::GetLatestResponse resp;
  st = client.GetLatest(1, {"tag"}, &resp);
  EXPECT_TRUE(st.ok());
  ASSERT_EQ(resp.updates_size(), 1);
  EXPECT_EQ(resp.updates(0).dst_tag(), "tag");
}
