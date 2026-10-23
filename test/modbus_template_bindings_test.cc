#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>

#include "ModbusRTULinkManager.h"
#include "mskdsp/ModbusTemplateBindings.hpp"
#include "support/FakeDataCenter.hpp"

namespace {
constexpr const char *kBindings = R"({"schema_version":1,"applications":[{"application_id":"a","platform_id":"https://platform","template_id":"t","revision":"1","template_snapshot":{},"prefix":"","address_base":1,"mappings":[{"source_point_id":"p","target_tag":"power","baseline":{"tag":"power"},"source_revision":"1","source_snapshot":{"id":"p"}}]}]})";

class ModbusTemplateBindingsTest : public testing::Test {
protected:
  void SetUp() override {
    path = std::filesystem::current_path() / ("modbus-template-" + std::to_string(++counter));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
  }
  void TearDown() override {
    std::filesystem::remove_all(path);
  }
  ModbusRTUProto::UpsertLinkRequest linkRequest() const {
    ModbusRTUProto::UpsertLinkRequest request;
    auto *config = request.mutable_config();
    config->set_conn_name("template");
    config->set_device_id(1);
    config->mutable_serial()->set_device("/dev/template-test");
    return request;
  }
  ModbusRTUProto::UpsertPointTableRequest pointRequest(uint64_t revision) const {
    ModbusRTUProto::UpsertPointTableRequest request;
    request.set_conn_name("template");
    request.set_replace(true);
    request.set_expected_revision(revision);
    request.set_template_bindings_json(kBindings);
    auto *point = request.add_points();
    point->set_tag("power");
    point->set_function(ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS);
    point->set_type(ModbusRTUProto::DATA_TYPE_UINT16);
    point->set_address(1);
    return request;
  }
  inline static unsigned counter = 0;
  std::filesystem::path path;
};
}  // 命名空间结束

// 验证：来源 JSON 拒绝无界、损坏结构及重复应用身份，允许清空来源。
TEST_F(ModbusTemplateBindingsTest, ValidatesBoundedBindings) {
  EXPECT_TRUE(mskdsp::ValidateTemplateBindings(kBindings).ok());
  EXPECT_EQ(mskdsp::ValidateTemplateBindings(kBindings, 2).error_code(), grpc::StatusCode::ABORTED);
  auto renamed = boost::json::parse(kBindings);
  auto &mapping = renamed.as_object().at("applications").as_array().front().as_object().at("mappings").as_array().front().as_object();
  mapping.at("target_tag") = "现场重命名";
  EXPECT_TRUE(mskdsp::ValidateTemplateBindings(boost::json::serialize(renamed)).ok());
  EXPECT_TRUE(mskdsp::ValidateTemplateBindings("").ok());
  for (const std::string value : {"[]", "{}", "{\"schema_version\":2,\"applications\":[]}", "{\"schema_version\":1,\"applications\":[{}]}", "{invalid"}) {
    EXPECT_EQ(mskdsp::ValidateTemplateBindings(value).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  }
  EXPECT_EQ(mskdsp::ValidateTemplateBindings(std::string(32u * 1024u * 1024u + 1, ' ')).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  auto duplicate = boost::json::parse(kBindings);
  auto &applications = duplicate.as_object().at("applications").as_array();
  applications.push_back(applications.front());
  EXPECT_EQ(mskdsp::ValidateTemplateBindings(boost::json::serialize(duplicate)).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 验证：同时提交相同预期版本的两个请求，仅一个能成功并推进一次版本。
TEST_F(ModbusTemplateBindingsTest, ConcurrentCasAcceptsExactlyOneWriter) {
  FakeDataCenterState state;
  ModbusRTU::LinkManager manager("ModbusRTU", path / "config.db");
  manager.setDataCenterStub(MakeStub(&state));
  ModbusRTUProto::LinkInfo info;
  ASSERT_TRUE(manager.UpsertLink(linkRequest(), &info).ok());
  ModbusRTUProto::PointTable table;
  ASSERT_TRUE(manager.GetPointTable("template", &table).ok());
  const auto revision = table.revision();
  const auto request = pointRequest(revision);
  grpc::Status firstStatus, secondStatus;
  std::jthread first([&] { firstStatus = manager.UpsertPointTable(request); });
  std::jthread second([&] { secondStatus = manager.UpsertPointTable(request); });
  first.join();
  second.join();
  EXPECT_NE(firstStatus.ok(), secondStatus.ok());
  EXPECT_EQ((firstStatus.ok() ? secondStatus : firstStatus).error_code(), grpc::StatusCode::ABORTED);
  ASSERT_TRUE(manager.GetPointTable("template", &table).ok());
  EXPECT_EQ(table.revision(), revision + 1);
}

// 验证：旧格式持久化点表缺少版本时，加载会迁移到正数版本并保留点位。
TEST_F(ModbusTemplateBindingsTest, MigratesLegacyPersistedRevision) {
  ModbusRTU::ModbusRTUPointTableStore store(path / "legacy.db");
  ModbusRTUProto::PointTablesConfig config;
  auto *table = config.add_point_tables();
  table->set_conn_name("legacy");
  ASSERT_TRUE(store.Save(config).ok());
  ModbusRTUProto::PointTablesConfig restored;
  ASSERT_TRUE(store.Load(&restored).ok());
  ASSERT_EQ(restored.point_tables_size(), 1);
  EXPECT_GT(restored.point_tables(0).revision(), 0);
}

// 验证：新连接版本为正数，CAS 冲突不更新点表；旧客户端保留来源，改名及恢复保留元数据。
TEST_F(ModbusTemplateBindingsTest, CasLegacyRenameAndRestore) {
  FakeDataCenterState state;
  const auto db = path / "config.db";
  uint64_t revision = 0;
  {
    ModbusRTU::LinkManager manager("ModbusRTU", db);
    manager.setDataCenterStub(MakeStub(&state));
    ModbusRTUProto::LinkInfo info;
    ASSERT_TRUE(manager.UpsertLink(linkRequest(), &info).ok());
    ModbusRTUProto::PointTable table;
    ASSERT_TRUE(manager.GetPointTable("template", &table).ok());
    ASSERT_GT(table.revision(), 0);
    auto request = pointRequest(table.revision());
    ASSERT_TRUE(manager.UpsertPointTable(request).ok());
    EXPECT_EQ(manager.UpsertPointTable(request).error_code(), grpc::StatusCode::ABORTED);
    ASSERT_TRUE(manager.GetPointTable("template", &table).ok());
    EXPECT_EQ(table.template_bindings_json(), kBindings);
    request.clear_expected_revision();
    request.clear_template_bindings_json();
    ASSERT_TRUE(manager.UpsertPointTable(request).ok());
    ASSERT_TRUE(manager.RenameLink("template", "renamed", &info).ok());
    ASSERT_TRUE(manager.GetPointTable("renamed", &table).ok());
    EXPECT_EQ(table.template_bindings_json(), kBindings);
    revision = table.revision();
  }
  ModbusRTU::LinkManager restored("ModbusRTU", db);
  restored.setDataCenterStub(MakeStub(&state));
  restored.LoadPersistedConfig();
  ModbusRTUProto::PointTable table;
  ASSERT_TRUE(restored.GetPointTable("renamed", &table).ok());
  EXPECT_EQ(table.revision(), revision);
  EXPECT_EQ(table.template_bindings_json(), kBindings);
}

// 验证：落盘失败不推进内存点表、版本及来源，来源保存必须携带非零 CAS 并完整替换。
TEST_F(ModbusTemplateBindingsTest, PersistenceFailureDoesNotCommit) {
  FakeDataCenterState state;
  const auto db = path / "config.db";
  ModbusRTU::LinkManager manager("ModbusRTU", db);
  manager.setDataCenterStub(MakeStub(&state));
  ModbusRTUProto::LinkInfo info;
  ASSERT_TRUE(manager.UpsertLink(linkRequest(), &info).ok());
  ModbusRTUProto::PointTable before;
  ASSERT_TRUE(manager.GetPointTable("template", &before).ok());
  auto request = pointRequest(0);
  EXPECT_EQ(manager.UpsertPointTable(request).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  request.set_expected_revision(before.revision());
  request.set_replace(false);
  EXPECT_EQ(manager.UpsertPointTable(request).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  request.set_replace(true);
  std::filesystem::remove(db);
  std::filesystem::create_directory(db);
  // 磁盘失败必须在任何标签替换之前返回，避免清理既有路由。
  auto noTagStub = MakeStub(&state);
  EXPECT_CALL(*noTagStub, UpsertConnTags(testing::_, testing::_, testing::_)).Times(0);
  manager.setDataCenterStub(noTagStub);
  EXPECT_FALSE(manager.UpsertPointTable(request).ok());
  ModbusRTUProto::PointTable after;
  ASSERT_TRUE(manager.GetPointTable("template", &after).ok());
  EXPECT_EQ(after.SerializeAsString(), before.SerializeAsString());
}

// 验证：连接配置 CAS 以及点表中的完整配置快照都拒绝并发配置变更。
TEST_F(ModbusTemplateBindingsTest, ConfigCasRejectsStaleSnapshots) {
  FakeDataCenterState state;
  ModbusRTU::LinkManager manager("ModbusRTU", path / "config.db");
  manager.setDataCenterStub(MakeStub(&state));
  ModbusRTUProto::LinkInfo info;
  ASSERT_TRUE(manager.UpsertLink(linkRequest(), &info).ok());
  const auto initial = info.config();
  auto update = linkRequest();
  *update.mutable_expected_config() = initial;
  update.mutable_config()->set_poll_interval_ms(initial.poll_interval_ms() + 100);
  ASSERT_TRUE(manager.UpsertLink(update, &info).ok());
  EXPECT_EQ(manager.UpsertLink(update, &info).error_code(), grpc::StatusCode::ABORTED);
  ModbusRTUProto::PointTable table;
  ASSERT_TRUE(manager.GetPointTable("template", &table).ok());
  auto points = pointRequest(table.revision());
  *points.mutable_expected_link_config() = initial;
  EXPECT_EQ(manager.UpsertPointTable(points).error_code(), grpc::StatusCode::ABORTED);
  update.mutable_config()->set_conn_name("missing");
  EXPECT_EQ(manager.UpsertLink(update, &info).error_code(), grpc::StatusCode::ABORTED);
}

// 验证：点表已提交后，即使连接配置仍等于预期，旧点表版本也会阻止读取计划回滚。
TEST_F(ModbusTemplateBindingsTest, PointTableRevisionGuardsConfigRollback) {
  FakeDataCenterState state;
  ModbusRTU::LinkManager manager("ModbusRTU", path / "config.db");
  manager.setDataCenterStub(MakeStub(&state));
  ModbusRTUProto::LinkInfo info;
  ASSERT_TRUE(manager.UpsertLink(linkRequest(), &info).ok());
  const auto initialConfig = info.config();
  ModbusRTUProto::PointTable table;
  ASSERT_TRUE(manager.GetPointTable("template", &table).ok());
  const auto initialRevision = table.revision();
  auto expanded = linkRequest();
  *expanded.mutable_expected_config() = initialConfig;
  expanded.set_expected_point_table_revision(initialRevision);
  expanded.mutable_config()->set_poll_interval_ms(initialConfig.poll_interval_ms() + 100);
  ASSERT_TRUE(manager.UpsertLink(expanded, &info).ok());
  const auto expandedConfig = info.config();
  ASSERT_TRUE(manager.UpsertPointTable(pointRequest(initialRevision)).ok());
  ModbusRTUProto::UpsertLinkRequest rollback;
  *rollback.mutable_config() = initialConfig;
  *rollback.mutable_expected_config() = expandedConfig;
  rollback.set_expected_point_table_revision(initialRevision);
  EXPECT_EQ(manager.UpsertLink(rollback, &info).error_code(), grpc::StatusCode::ABORTED);
  ASSERT_TRUE(manager.GetLink("template", &info).ok());
  EXPECT_EQ(info.config().SerializeAsString(), expandedConfig.SerializeAsString());
}

// 验证：标签同步失败后磁盘回滚到旧点表，内存版本、来源及磁盘内容均保持原状。
TEST_F(ModbusTemplateBindingsTest, DataCenterFailureRestoresDiskSnapshot) {
  FakeDataCenterState state;
  const auto db = path / "config.db";
  ModbusRTU::LinkManager manager("ModbusRTU", db);
  auto stub = MakeStub(&state);
  manager.setDataCenterStub(stub);
  ModbusRTUProto::LinkInfo info;
  ASSERT_TRUE(manager.UpsertLink(linkRequest(), &info).ok());
  ModbusRTUProto::PointTable before;
  ASSERT_TRUE(manager.GetPointTable("template", &before).ok());
  EXPECT_CALL(*stub, UpsertConnTags(testing::_, testing::_, testing::_))
      .WillOnce(testing::Return(grpc::Status(grpc::StatusCode::UNAVAILABLE, "测试标签同步失败")));
  EXPECT_EQ(manager.UpsertPointTable(pointRequest(before.revision())).error_code(), grpc::StatusCode::UNAVAILABLE);
  ModbusRTUProto::PointTable after;
  ASSERT_TRUE(manager.GetPointTable("template", &after).ok());
  EXPECT_EQ(before.SerializeAsString(), after.SerializeAsString());
  ModbusRTU::ModbusRTUPointTableStore store(db);
  ModbusRTUProto::PointTablesConfig disk;
  ASSERT_TRUE(store.Load(&disk).ok());
  ASSERT_EQ(disk.point_tables_size(), 1);
  EXPECT_EQ(disk.point_tables(0).SerializeAsString(), before.SerializeAsString());
}

// 验证：版本达到 uint64 上限时拒绝保存，不能溢出为零并使旧客户端误判能力。
TEST_F(ModbusTemplateBindingsTest, RejectsRevisionOverflow) {
  FakeDataCenterState state;
  const auto db = path / "config.db";
  {
    ModbusRTU::LinkManager manager("ModbusRTU", db);
    manager.setDataCenterStub(MakeStub(&state));
    ModbusRTUProto::LinkInfo info;
    ASSERT_TRUE(manager.UpsertLink(linkRequest(), &info).ok());
  }
  ModbusRTU::ModbusRTUPointTableStore store(db);
  ModbusRTUProto::PointTablesConfig config;
  auto *table = config.add_point_tables();
  table->set_conn_name("template");
  table->set_revision(std::numeric_limits<uint64_t>::max());
  ASSERT_TRUE(store.Save(config).ok());
  ModbusRTU::LinkManager manager("ModbusRTU", db);
  manager.setDataCenterStub(MakeStub(&state));
  manager.LoadPersistedConfig();
  EXPECT_EQ(manager.UpsertPointTable(pointRequest(table->revision())).error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED);
}

// 验证：显式空来源可随空点表原子保存，重启后仍保留新版本和空来源。
TEST_F(ModbusTemplateBindingsTest, ClearsBindingsAndPersistsEmptyTable) {
  FakeDataCenterState state;
  const auto db = path / "config.db";
  ModbusRTUProto::PointTable expected;
  {
    ModbusRTU::LinkManager manager("ModbusRTU", db);
    manager.setDataCenterStub(MakeStub(&state));
    ModbusRTUProto::LinkInfo info;
    ASSERT_TRUE(manager.UpsertLink(linkRequest(), &info).ok());
    ASSERT_TRUE(manager.GetPointTable("template", &expected).ok());
    ASSERT_TRUE(manager.UpsertPointTable(pointRequest(expected.revision())).ok());
    ASSERT_TRUE(manager.GetPointTable("template", &expected).ok());
    auto clear = pointRequest(expected.revision());
    clear.clear_points();
    clear.set_template_bindings_json("");
    ASSERT_TRUE(manager.UpsertPointTable(clear).ok());
    ASSERT_TRUE(manager.GetPointTable("template", &expected).ok());
    EXPECT_EQ(expected.points_size(), 0);
    EXPECT_TRUE(expected.template_bindings_json().empty());
  }
  ModbusRTU::LinkManager manager("ModbusRTU", db);
  manager.setDataCenterStub(MakeStub(&state));
  manager.LoadPersistedConfig();
  ModbusRTUProto::PointTable actual;
  ASSERT_TRUE(manager.GetPointTable("template", &actual).ok());
  EXPECT_EQ(actual.SerializeAsString(), expected.SerializeAsString());
}
