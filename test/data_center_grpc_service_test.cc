#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>

#include "DataCenterGrpcService.h"
#include "Logger.h"

namespace {
class ScopedTempDir {
public:
  ScopedTempDir() {
    auto base = std::filesystem::current_path();
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    path_ = base / ("data_center_grpc_service_test_tmp_" + std::to_string(ts));
    std::filesystem::create_directories(path_);
  }

  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path& path() const { return path_; }

private:
  std::filesystem::path path_;
};

class ScopedCwd {
public:
  explicit ScopedCwd(const std::filesystem::path& newCwd) :
    old_(std::filesystem::current_path()) {
    std::filesystem::current_path(newCwd);
  }

  ~ScopedCwd() { std::filesystem::current_path(old_); }

  ScopedCwd(const ScopedCwd&) = delete;
  ScopedCwd& operator=(const ScopedCwd&) = delete;

private:
  std::filesystem::path old_;
};

class DataCenterGrpcServiceTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Keep Logger output stable (avoid per-test CWD affecting log destination).
    ModuleManager::Logger::init("./log", "data_center_grpc_service_test.log");

    tmpDir_ = std::make_unique<ScopedTempDir>();
    cwd_ = std::make_unique<ScopedCwd>(tmpDir_->path());

    service_ = std::make_unique<DataCenter::DataCenterGrpcServiceImpl>();

    grpc::ServerBuilder builder;
    builder.RegisterService(service_.get());
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port_);
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);
    ASSERT_GT(port_, 0);

    const auto addr = std::string("127.0.0.1:") + std::to_string(port_);
    channel_ = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
    ASSERT_NE(channel_, nullptr);
    ASSERT_TRUE(channel_->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(2)));

    stub_ = DataCenterProto::DataCenterService::NewStub(channel_);
    ASSERT_NE(stub_, nullptr);
  }

  void TearDown() override {
    if (server_) {
      server_->Shutdown();
    }
    server_.reset();
    service_.reset();
    stub_.reset();
    channel_.reset();
    cwd_.reset();
    tmpDir_.reset();
  }

  DataCenterProto::ConnectionInfo GetOrCreateConnection(std::string moduleName, std::string connName) {
    grpc::ClientContext ctx;
    DataCenterProto::GetOrCreateConnectionRequest req;
    req.mutable_key()->set_module_name(std::move(moduleName));
    req.mutable_key()->set_conn_name(std::move(connName));
    DataCenterProto::ConnectionInfo resp;
    auto status = stub_->GetOrCreateConnection(&ctx, req, &resp);
    EXPECT_TRUE(status.ok()) << status.error_message();
    return resp;
  }

  void UpsertConnTags(uint32_t connId, std::initializer_list<const char*> tags) {
    grpc::ClientContext ctx;
    DataCenterProto::UpsertConnTagsRequest req;
    req.set_conn_id(connId);
    req.set_replace(true);
    for (const auto* tag : tags) {
      req.add_tags(tag);
    }
    DataCenterProto::Empty resp;
    auto status = stub_->UpsertConnTags(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
  }

  void UpsertRoutes(std::initializer_list<std::tuple<uint32_t, const char*, uint32_t, const char*>> routes) {
    grpc::ClientContext ctx;
    DataCenterProto::UpsertRoutesRequest req;
    req.set_replace(true);
    for (const auto& [srcConnId, srcTag, dstConnId, dstTag] : routes) {
      auto* route = req.add_routes();
      route->mutable_src()->set_conn_id(srcConnId);
      route->mutable_src()->set_tag(srcTag);
      route->mutable_dst()->set_conn_id(dstConnId);
      route->mutable_dst()->set_tag(dstTag);
    }
    DataCenterProto::Empty resp;
    auto status = stub_->UpsertRoutes(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
  }

  std::unique_ptr<ScopedTempDir> tmpDir_;
  std::unique_ptr<ScopedCwd> cwd_;
  std::unique_ptr<DataCenter::DataCenterGrpcServiceImpl> service_;
  std::unique_ptr<grpc::Server> server_;
  int port_{0};
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<DataCenterProto::DataCenterService::Stub> stub_;
};
}  // namespace

// 验证：连接生命周期（创建/重命名/删除）与 UpsertConnection 的冲突/未分配错误能正确返回。
TEST_F(DataCenterGrpcServiceTest, ConnectionLifecycleAndUpsertConnectionValidations) {
  const auto conn1 = GetOrCreateConnection("ModbusRTU", "mb-1");
  const auto conn2 = GetOrCreateConnection("IEC104", "104-1");
  ASSERT_NE(conn1.conn_id(), 0u);
  ASSERT_NE(conn2.conn_id(), 0u);

  {
    grpc::ClientContext ctx;
    DataCenterProto::Empty req;
    DataCenterProto::ListConnectionsResponse resp;
    ASSERT_TRUE(stub_->ListConnections(&ctx, req, &resp).ok());
    EXPECT_EQ(resp.conns_size(), 2);
  }

  {
    grpc::ClientContext ctx;
    DataCenterProto::RenameConnectionRequest req;
    req.mutable_old_key()->set_module_name("ModbusRTU");
    req.mutable_old_key()->set_conn_name("mb-1");
    req.mutable_new_key()->set_module_name("ModbusRTU");
    req.mutable_new_key()->set_conn_name("mb-renamed");
    DataCenterProto::ConnectionInfo resp;
    auto status = stub_->RenameConnection(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(resp.conn_id(), conn1.conn_id());
    EXPECT_EQ(resp.conn_name(), "mb-renamed");
  }

  {
    grpc::ClientContext ctx;
    DataCenterProto::UpsertConnectionRequest req;
    req.mutable_conn()->set_conn_id(999);
    req.mutable_conn()->set_module_name("X");
    req.mutable_conn()->set_conn_name("Y");
    DataCenterProto::Empty resp;
    auto status = stub_->UpsertConnection(&ctx, req, &resp);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
  }

  {
    grpc::ClientContext ctx;
    DataCenterProto::UpsertConnectionRequest req;
    req.mutable_conn()->set_conn_id(conn1.conn_id());
    req.mutable_conn()->set_module_name("IEC104");
    req.mutable_conn()->set_conn_name("104-1");
    DataCenterProto::Empty resp;
    auto status = stub_->UpsertConnection(&ctx, req, &resp);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ALREADY_EXISTS);
  }

  {
    grpc::ClientContext ctx;
    DataCenterProto::DeleteConnectionRequest req;
    req.mutable_key()->set_module_name("IEC104");
    req.mutable_key()->set_conn_name("104-1");
    DataCenterProto::Empty resp;
    auto status = stub_->DeleteConnection(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
  }

  {
    grpc::ClientContext ctx;
    DataCenterProto::Empty req;
    DataCenterProto::ListConnectionsResponse resp;
    ASSERT_TRUE(stub_->ListConnections(&ctx, req, &resp).ok());
    EXPECT_EQ(resp.conns_size(), 1);
  }
}

// 验证：Subscribe(snapshot=true) 会先推送快照；并按 tags 过滤实时更新；DeleteConnection 会关闭订阅流（best-effort）。
TEST_F(DataCenterGrpcServiceTest, PublishSubscribeSnapshotFilteringAndDeleteConnectionClosesStream) {
  const auto src = GetOrCreateConnection("ModbusRTU", "src");
  const auto dst = GetOrCreateConnection("IEC104", "dst");

  UpsertConnTags(src.conn_id(), {"A", "C"});
  UpsertConnTags(dst.conn_id(), {"B", "D"});

  {
    grpc::ClientContext ctx;
    DataCenterProto::GetConnTagsRequest req;
    req.set_conn_id(dst.conn_id());
    DataCenterProto::ConnTags resp;
    auto status = stub_->GetConnTags(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(resp.conn_id(), dst.conn_id());
  }

  UpsertRoutes({
      {src.conn_id(), "A", dst.conn_id(), "B"},
      {src.conn_id(), "C", dst.conn_id(), "D"},
  });

  {
    grpc::ClientContext ctx;
    DataCenterProto::ListRoutesRequest req;
    req.set_dst_conn_id(dst.conn_id());
    DataCenterProto::ListRoutesResponse resp;
    auto status = stub_->ListRoutes(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(resp.routes_size(), 2);
  }

  {
    grpc::ClientContext ctx;
    DataCenterProto::PublishRequest req;
    req.set_conn_id(src.conn_id());
    req.set_tag("A");
    req.mutable_value()->set_int_value(10);
    DataCenterProto::Empty resp;
    ASSERT_TRUE(stub_->Publish(&ctx, req, &resp).ok());
  }
  {
    grpc::ClientContext ctx;
    DataCenterProto::PublishRequest req;
    req.set_conn_id(src.conn_id());
    req.set_tag("C");
    req.mutable_value()->set_int_value(30);
    DataCenterProto::Empty resp;
    ASSERT_TRUE(stub_->Publish(&ctx, req, &resp).ok());
  }

  {
    grpc::ClientContext ctx;
    DataCenterProto::GetLatestRequest req;
    req.set_conn_id(dst.conn_id());
    DataCenterProto::GetLatestResponse resp;
    auto status = stub_->GetLatest(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    ASSERT_EQ(resp.updates_size(), 2);
    EXPECT_EQ(resp.updates(0).dst_tag(), "B");
    EXPECT_EQ(resp.updates(0).value().int_value(), 10);
    EXPECT_EQ(resp.updates(1).dst_tag(), "D");
    EXPECT_EQ(resp.updates(1).value().int_value(), 30);
  }

  grpc::ClientContext subCtx;
  subCtx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
  DataCenterProto::SubscribeRequest subReq;
  subReq.set_conn_id(dst.conn_id());
  subReq.add_tags("B");
  subReq.set_snapshot(true);
  auto reader = stub_->Subscribe(&subCtx, subReq);

  DataCenterProto::PointUpdate snap;
  ASSERT_TRUE(reader->Read(&snap));
  EXPECT_EQ(snap.dst_conn_id(), dst.conn_id());
  EXPECT_EQ(snap.dst_tag(), "B");
  EXPECT_EQ(snap.value().int_value(), 10);

  {
    grpc::ClientContext ctx;
    DataCenterProto::BatchPublishRequest req;
    auto* p1 = req.add_points();
    p1->set_conn_id(src.conn_id());
    p1->set_tag("A");
    p1->mutable_value()->set_int_value(20);
    auto* p2 = req.add_points();
    p2->set_conn_id(src.conn_id());
    p2->set_tag("C");
    p2->mutable_value()->set_int_value(40);
    DataCenterProto::Empty resp;
    ASSERT_TRUE(stub_->BatchPublish(&ctx, req, &resp).ok());
  }

  DataCenterProto::PointUpdate update;
  ASSERT_TRUE(reader->Read(&update));
  EXPECT_EQ(update.dst_conn_id(), dst.conn_id());
  EXPECT_EQ(update.dst_tag(), "B");
  EXPECT_EQ(update.value().int_value(), 20);

  {
    grpc::ClientContext ctx;
    DataCenterProto::DeleteConnectionRequest req;
    req.mutable_key()->set_module_name("IEC104");
    req.mutable_key()->set_conn_name("dst");
    DataCenterProto::Empty resp;
    auto status = stub_->DeleteConnection(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
  }

  DataCenterProto::PointUpdate ignored;
  EXPECT_FALSE(reader->Read(&ignored));
  auto finishStatus = reader->Finish();
  EXPECT_TRUE(finishStatus.ok()) << finishStatus.error_message();
}

// 验证：DeleteRoutes 可删除指定路由，且不会再产生最新值缓存。
TEST_F(DataCenterGrpcServiceTest, DeleteRoutesRemovesRouteAndStopsLatestUpdates) {
  const auto src = GetOrCreateConnection("ModbusRTU", "src");
  const auto dst = GetOrCreateConnection("IEC104", "dst");

  UpsertRoutes({
      {src.conn_id(), "A", dst.conn_id(), "B"},
  });

  {
    grpc::ClientContext ctx;
    DataCenterProto::ListRoutesRequest req;
    req.set_dst_conn_id(dst.conn_id());
    DataCenterProto::ListRoutesResponse resp;
    ASSERT_TRUE(stub_->ListRoutes(&ctx, req, &resp).ok());
    ASSERT_EQ(resp.routes_size(), 1);
  }

  {
    grpc::ClientContext ctx;
    DataCenterProto::DeleteRoutesRequest req;
    auto* route = req.add_routes();
    route->mutable_src()->set_conn_id(src.conn_id());
    route->mutable_src()->set_tag("A");
    route->mutable_dst()->set_conn_id(dst.conn_id());
    route->mutable_dst()->set_tag("B");
    DataCenterProto::Empty resp;
    ASSERT_TRUE(stub_->DeleteRoutes(&ctx, req, &resp).ok());
  }

  {
    grpc::ClientContext ctx;
    DataCenterProto::ListRoutesRequest req;
    req.set_dst_conn_id(dst.conn_id());
    DataCenterProto::ListRoutesResponse resp;
    ASSERT_TRUE(stub_->ListRoutes(&ctx, req, &resp).ok());
    EXPECT_EQ(resp.routes_size(), 0);
  }

  {
    grpc::ClientContext ctx;
    DataCenterProto::PublishRequest req;
    req.set_conn_id(src.conn_id());
    req.set_tag("A");
    req.mutable_value()->set_int_value(123);
    DataCenterProto::Empty resp;
    ASSERT_TRUE(stub_->Publish(&ctx, req, &resp).ok());
  }

  {
    grpc::ClientContext ctx;
    DataCenterProto::GetLatestRequest req;
    req.set_conn_id(dst.conn_id());
    DataCenterProto::GetLatestResponse resp;
    ASSERT_TRUE(stub_->GetLatest(&ctx, req, &resp).ok());
    EXPECT_EQ(resp.updates_size(), 0);
  }
}

// 验证：Subscribe 对非法参数返回 INVALID_ARGUMENT。
TEST_F(DataCenterGrpcServiceTest, SubscribeRejectsInvalidArguments) {
  {
    grpc::ClientContext ctx;
    DataCenterProto::SubscribeRequest req;
    req.set_conn_id(0);
    req.set_snapshot(true);
    auto reader = stub_->Subscribe(&ctx, req);
    auto status = reader->Finish();
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  }

  {
    grpc::ClientContext ctx;
    DataCenterProto::SubscribeRequest req;
    req.set_conn_id(1);
    req.add_tags("");
    auto reader = stub_->Subscribe(&ctx, req);
    auto status = reader->Finish();
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  }
}
