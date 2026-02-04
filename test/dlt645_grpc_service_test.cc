#include <gtest/gtest.h>

#include "DLT645GrpcService.h"
#include "DLT645.h"
#include "support/FakeDataCenter.hpp"

namespace {
using DLT645Module = DLT645::DLT645;
using DLT645Service = DLT645::DLT645GrpcServiceImpl;
}  // namespace

// 验证：模块未就绪时所有 RPC 返回 FAILED_PRECONDITION。
TEST(Dlt645GrpcServiceTest, RejectsWhenModuleNotReady) {
  DLT645Service service;
  grpc::ServerContext ctx;

  DLT645Proto::UpdateConfigRequest updateReq;
  DLT645Proto::UpdateConfigResponse updateResp;
  auto st = service.UpdateConfig(&ctx, &updateReq, &updateResp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  DLT645Proto::UpsertLinkRequest linkReq;
  DLT645Proto::LinkInfo linkResp;
  st = service.UpsertLink(&ctx, &linkReq, &linkResp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  DLT645Proto::GetLinkRequest getReq;
  st = service.GetLink(&ctx, &getReq, &linkResp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  DLT645Proto::ListLinksResponse listResp;
  st = service.ListLinks(&ctx, nullptr, &listResp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  DLT645Proto::DeleteLinkRequest delReq;
  DLT645Proto::Empty empty;
  st = service.DeleteLink(&ctx, &delReq, &empty);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  DLT645Proto::StartLinkRequest startReq;
  st = service.StartLink(&ctx, &startReq, &empty);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  DLT645Proto::StopLinkRequest stopReq;
  st = service.StopLink(&ctx, &stopReq, &empty);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  DLT645Proto::UpsertPointTableRequest ptReq;
  st = service.UpsertPointTable(&ctx, &ptReq, &empty);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  DLT645Proto::GetPointTableRequest getPtReq;
  DLT645Proto::PointTable ptResp;
  st = service.GetPointTable(&ctx, &getPtReq, &ptResp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

// 验证：请求/响应为空时返回 INVALID_ARGUMENT。
TEST(Dlt645GrpcServiceTest, RejectsNullRequestOrResponse) {
  DLT645Module module;
  DLT645Service service;
  service.getDLT645(&module);

  grpc::ServerContext ctx;
  auto st = service.UpdateConfig(&ctx, nullptr, nullptr);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  DLT645Proto::UpdateConfigRequest updateReq;
  st = service.UpdateConfig(&ctx, &updateReq, nullptr);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  st = service.UpsertLink(&ctx, nullptr, nullptr);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：正常委托路径可执行。
TEST(Dlt645GrpcServiceTest, DelegatesToLinkManager) {
  FakeDataCenterState state;
  auto stub = MakeStub(&state);

  DLT645Module module;
  module.linkManager().setDataCenterStub(stub);

  DLT645Service service;
  service.getDLT645(&module);

  grpc::ServerContext ctx;

  auto updateReq = DLT645Proto::UpdateConfigRequest();
  auto *mqtt = updateReq.mutable_mqtt();
  mqtt->set_host("127.0.0.1");
  mqtt->set_port(1883);
  mqtt->set_client_id("client-1");
  DLT645Proto::UpdateConfigResponse updateResp;
  auto st = service.UpdateConfig(&ctx, &updateReq, &updateResp);
  EXPECT_TRUE(st.ok());

  DLT645Proto::GetLinkRequest getReq;
  getReq.set_conn_name("missing");
  DLT645Proto::LinkInfo linkResp;
  st = service.GetLink(&ctx, &getReq, &linkResp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);

  DLT645Proto::ListLinksResponse listResp;
  st = service.ListLinks(&ctx, nullptr, &listResp);
  EXPECT_TRUE(st.ok());
}
