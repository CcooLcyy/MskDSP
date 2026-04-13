#include <gtest/gtest.h>

#include "IEC104GrpcService.h"
#include "IEC104.h"
#include "support/FakeDataCenter.hpp"

namespace {
using IEC104Module = IEC104::IEC104;
using IEC104Service = IEC104::IEC104GrpcServiceImpl;
}  // 命名空间结束

// 验证：IEC104 服务未就绪时返回 FAILED_PRECONDITION。
TEST(IEC104GrpcServiceTest, RejectsWhenModuleNotReady) {
  IEC104Service service;
  grpc::ServerContext ctx;

  IEC104Proto::UpsertLinkRequest linkReq;
  IEC104Proto::LinkInfo linkResp;
  auto st = service.UpsertLink(&ctx, &linkReq, &linkResp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  IEC104Proto::RenameLinkRequest renameReq;
  st = service.RenameLink(&ctx, &renameReq, &linkResp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  IEC104Proto::GetLinkRequest getReq;
  st = service.GetLink(&ctx, &getReq, &linkResp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  IEC104Proto::ListLinksResponse listResp;
  st = service.ListLinks(&ctx, nullptr, &listResp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证：请求/响应为空时返回 INVALID_ARGUMENT。
TEST(IEC104GrpcServiceTest, RejectsNullRequestOrResponse) {
  IEC104Module module;
  IEC104Service service;
  service.getIEC104(&module);

  grpc::ServerContext ctx;
  auto st = service.UpsertLink(&ctx, nullptr, nullptr);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  IEC104Proto::UpsertLinkRequest linkReq;
  st = service.UpsertLink(&ctx, &linkReq, nullptr);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  st = service.RenameLink(&ctx, nullptr, nullptr);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：正常委托路径可执行。
TEST(IEC104GrpcServiceTest, DelegatesToLinkManager) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  IEC104Module module;
  module.linkManager().setDataCenterStub(stub);

  IEC104Service service;
  service.getIEC104(&module);

  grpc::ServerContext ctx;

  IEC104Proto::GetLinkRequest getReq;
  getReq.set_conn_name("missing");
  IEC104Proto::LinkInfo linkResp;
  auto st = service.GetLink(&ctx, &getReq, &linkResp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);

  IEC104Proto::ListLinksResponse listResp;
  st = service.ListLinks(&ctx, nullptr, &listResp);
  EXPECT_TRUE(st.ok());
}
