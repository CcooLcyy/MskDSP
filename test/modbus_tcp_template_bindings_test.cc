#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "ModbusTCPLinkManager.h"
#include "support/FakeDataCenter.hpp"

namespace {
class ModbusTcpTemplateBindingsTest : public testing::Test {
protected:
  void SetUp() override {
    directory = std::filesystem::current_path() / ("tcp-template-" + std::to_string(++counter));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
  }
  void TearDown() override {
    std::filesystem::remove_all(directory);
  }
  ModbusTCPProto::UpsertLinkRequest linkRequest() const {
    ModbusTCPProto::UpsertLinkRequest request;
    request.mutable_config()->set_conn_name("template");
    request.mutable_config()->mutable_tcp()->set_host("127.0.0.1");
    request.mutable_config()->mutable_tcp()->set_connect_timeout_ms(10);
    return request;
  }
  ModbusTCPProto::UpsertPointTableRequest pointRequest(uint64_t revision) const {
    ModbusTCPProto::UpsertPointTableRequest request;
    request.set_conn_name("template");
    request.set_replace(true);
    request.set_expected_revision(revision);
    request.set_template_bindings_json(R"({"schema_version":1,"applications":[]})");
    auto *point = request.add_points();
    point->set_tag("power");
    point->set_function(ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS);
    point->set_type(ModbusRTUProto::DATA_TYPE_UINT16);
    point->set_address(1);
    return request;
  }
  inline static unsigned counter = 0;
  std::filesystem::path directory;
};
}  // 命名空间结束

// 验证：TCP 新连接有正数版本，来源保存保持功能停止，旧客户端保存保留来源并使旧 CAS 失效。
TEST_F(ModbusTcpTemplateBindingsTest, CasAndLegacyWritesPreserveBindings) {
  FakeDataCenterState state;
  ModbusTCP::LinkManager manager(directory / "config.db");
  manager.setDataCenterStub(MakeStub(&state));
  ModbusTCPProto::LinkInfo info;
  ASSERT_TRUE(manager.UpsertLink(linkRequest(), &info).ok());
  ModbusTCPProto::PointTable table;
  ASSERT_TRUE(manager.GetPointTable("template", &table).ok());
  ASSERT_GT(table.revision(), 0);
  auto request = pointRequest(table.revision());
  ASSERT_TRUE(manager.UpsertPointTable(request).ok());
  EXPECT_EQ(manager.UpsertPointTable(request).error_code(), grpc::StatusCode::ABORTED);
  ASSERT_TRUE(manager.GetLink("template", &info).ok());
  EXPECT_EQ(info.state(), ModbusTCPProto::LINK_STATE_STOPPED);
  ASSERT_TRUE(manager.GetPointTable("template", &table).ok());
  const auto version = table.revision();
  const auto bindings = table.template_bindings_json();
  request.clear_expected_revision();
  request.clear_template_bindings_json();
  ASSERT_TRUE(manager.UpsertPointTable(request).ok());
  ASSERT_TRUE(manager.GetPointTable("template", &table).ok());
  EXPECT_GT(table.revision(), version);
  EXPECT_EQ(table.template_bindings_json(), bindings);
  const auto renamedRevision = table.revision();
  ModbusTCPProto::RenameLinkRequest rename;
  rename.set_old_conn_name("template");
  rename.set_new_conn_name("renamed");
  ASSERT_TRUE(manager.RenameLink(rename, &info).ok());
  ASSERT_TRUE(manager.GetPointTable("renamed", &table).ok());
  EXPECT_EQ(table.template_bindings_json(), bindings);
  EXPECT_EQ(table.revision(), renamedRevision);
}

// 验证：TCP 落盘失败不修改实际点表、版本或来源；空点表的来源及版本经恢复后仍存在。
TEST_F(ModbusTcpTemplateBindingsTest, PersistenceFailureAndEmptyRestore) {
  FakeDataCenterState state;
  const auto db = directory / "config.db";
  ModbusTCPProto::PointTable persisted;
  {
    ModbusTCP::LinkManager manager(db);
    manager.setDataCenterStub(MakeStub(&state));
    ModbusTCPProto::LinkInfo info;
    ASSERT_TRUE(manager.UpsertLink(linkRequest(), &info).ok());
    ASSERT_TRUE(manager.GetPointTable("template", &persisted).ok());
    auto request = pointRequest(persisted.revision());
    request.clear_points();
    ASSERT_TRUE(manager.UpsertPointTable(request).ok());
    ASSERT_TRUE(manager.GetPointTable("template", &persisted).ok());
  }
  ModbusTCP::LinkManager manager(db);
  manager.setDataCenterStub(MakeStub(&state));
  manager.LoadPersistedConfig();
  ModbusTCPProto::PointTable before;
  ASSERT_TRUE(manager.GetPointTable("template", &before).ok());
  EXPECT_EQ(before.SerializeAsString(), persisted.SerializeAsString());
  std::filesystem::remove(db);
  std::filesystem::create_directory(db);
  auto noTagStub = MakeStub(&state);
  EXPECT_CALL(*noTagStub, UpsertConnTags(testing::_, testing::_, testing::_)).Times(0);
  manager.setDataCenterStub(noTagStub);
  EXPECT_FALSE(manager.UpsertPointTable(pointRequest(before.revision())).ok());
  ModbusTCPProto::PointTable after;
  ASSERT_TRUE(manager.GetPointTable("template", &after).ok());
  EXPECT_EQ(after.SerializeAsString(), before.SerializeAsString());
}

// 验证：TCP 连接配置 CAS 和点表完整配置比较拒绝过期快照。
TEST_F(ModbusTcpTemplateBindingsTest, RejectsStaleConfigSnapshots) {
  FakeDataCenterState state;
  ModbusTCP::LinkManager manager(directory / "config.db");
  manager.setDataCenterStub(MakeStub(&state));
  ModbusTCPProto::LinkInfo info;
  ASSERT_TRUE(manager.UpsertLink(linkRequest(), &info).ok());
  const auto initial = info.config();
  auto update = linkRequest();
  *update.mutable_expected_config() = initial;
  update.mutable_config()->set_poll_interval_ms(initial.poll_interval_ms() + 100);
  ASSERT_TRUE(manager.UpsertLink(update, &info).ok());
  EXPECT_EQ(manager.UpsertLink(update, &info).error_code(), grpc::StatusCode::ABORTED);
  ModbusTCPProto::PointTable table;
  ASSERT_TRUE(manager.GetPointTable("template", &table).ok());
  auto request = pointRequest(table.revision());
  *request.mutable_expected_link_config() = initial;
  EXPECT_EQ(manager.UpsertPointTable(request).error_code(), grpc::StatusCode::ABORTED);
}

// 验证：TCP 点表版本推进后，旧版本连接配置回滚被拒绝，保留已扩展配置。
TEST_F(ModbusTcpTemplateBindingsTest, PointTableRevisionGuardsConfigRollback) {
  FakeDataCenterState state;
  ModbusTCP::LinkManager manager(directory / "config.db");
  manager.setDataCenterStub(MakeStub(&state));
  ModbusTCPProto::LinkInfo info;
  ASSERT_TRUE(manager.UpsertLink(linkRequest(), &info).ok());
  const auto initialConfig = info.config();
  ModbusTCPProto::PointTable table;
  ASSERT_TRUE(manager.GetPointTable("template", &table).ok());
  const auto initialRevision = table.revision();
  auto expanded = linkRequest();
  *expanded.mutable_expected_config() = initialConfig;
  expanded.set_expected_point_table_revision(initialRevision);
  expanded.mutable_config()->set_poll_interval_ms(initialConfig.poll_interval_ms() + 100);
  ASSERT_TRUE(manager.UpsertLink(expanded, &info).ok());
  const auto expandedConfig = info.config();
  ASSERT_TRUE(manager.UpsertPointTable(pointRequest(initialRevision)).ok());
  ModbusTCPProto::UpsertLinkRequest rollback;
  *rollback.mutable_config() = initialConfig;
  *rollback.mutable_expected_config() = expandedConfig;
  rollback.set_expected_point_table_revision(initialRevision);
  EXPECT_EQ(manager.UpsertLink(rollback, &info).error_code(), grpc::StatusCode::ABORTED);
  ASSERT_TRUE(manager.GetLink("template", &info).ok());
  EXPECT_EQ(info.config().SerializeAsString(), expandedConfig.SerializeAsString());
}

// 验证：TCP 标签同步失败时回滚磁盘候选点表和来源，保持内存旧版本。
TEST_F(ModbusTcpTemplateBindingsTest, DataCenterFailureRestoresDiskSnapshot) {
  FakeDataCenterState state;
  const auto db = directory / "config.db";
  ModbusTCP::LinkManager manager(db);
  auto stub = MakeStub(&state);
  manager.setDataCenterStub(stub);
  ModbusTCPProto::LinkInfo info;
  ASSERT_TRUE(manager.UpsertLink(linkRequest(), &info).ok());
  ModbusTCPProto::PointTable before;
  ASSERT_TRUE(manager.GetPointTable("template", &before).ok());
  EXPECT_CALL(*stub, UpsertConnTags(testing::_, testing::_, testing::_))
      .WillOnce(testing::Return(grpc::Status(grpc::StatusCode::UNAVAILABLE, "测试标签同步失败")));
  EXPECT_EQ(manager.UpsertPointTable(pointRequest(before.revision())).error_code(), grpc::StatusCode::UNAVAILABLE);
  ModbusTCPProto::PointTable after;
  ASSERT_TRUE(manager.GetPointTable("template", &after).ok());
  EXPECT_EQ(before.SerializeAsString(), after.SerializeAsString());
  ModbusTCP::PointTableStore store(db);
  ModbusTCPProto::PointTablesConfig disk;
  ASSERT_TRUE(store.Load(&disk).ok());
  ASSERT_EQ(disk.point_tables_size(), 1);
  EXPECT_EQ(disk.point_tables(0).SerializeAsString(), before.SerializeAsString());
}
