#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "IEC61850ConfigStore.h"
#include "mskdsp/ConfigDatabase.h"

namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("mskdsp-iec61850-store-test-" + std::to_string(suffix));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

private:
  std::filesystem::path path_;
};

IEC61850Proto::PersistedConfig MakeConfig() {
  IEC61850Proto::PersistedConfig config;
  config.set_schema_version(1);
  auto* model = config.add_models();
  model->set_model_name("station-model");
  model->set_source_name("station.scd");
  model->set_document_kind(IEC61850Proto::SCL_DOCUMENT_KIND_SCD);
  auto* modelIed = model->add_ieds();
  modelIed->set_name("IED1");
  auto* accessPoint = modelIed->add_access_points();
  accessPoint->set_name("AP1");
  accessPoint->set_has_server(true);
  auto* attribute = modelIed->add_data_attributes();
  attribute->set_access_point("AP1");
  attribute->set_data_ref("IED1LD0/PTRC1.Tr.general");
  attribute->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST);
  attribute->set_basic_type("BOOLEAN");
  auto* extRef = modelIed->add_ext_refs();
  extRef->set_access_point("AP1");
  extRef->set_ied_name("IED1");
  extRef->set_source_data_ref("IED1LD0/PTRC1.Tr.general");
  extRef->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST);
  extRef->set_service_type("GOOSE");
  extRef->set_src_ld_inst("LD0");
  extRef->set_src_ln_class("LLN0");
  extRef->set_src_cb_name("gcb1");

  auto* persistedIed = config.add_ieds();
  persistedIed->set_conn_id(100);
  persistedIed->set_desired_running(true);
  auto* ied = persistedIed->mutable_config();
  ied->set_conn_name("line-1");
  ied->set_model_name("station-model");
  ied->set_ied_name("IED1");
  ied->set_access_point("AP1");
  ied->set_enable_goose(true);
  auto* channel = ied->add_channels();
  channel->set_channel(IEC61850Proto::NETWORK_CHANNEL_A);
  channel->set_enabled(true);
  channel->set_interface_name("eth0");

  auto* table = config.add_point_mappings();
  table->set_conn_name("line-1");
  auto* point = table->add_points();
  point->set_tag("TRIP");
  point->set_data_ref("IED1LD0/PTRC1.Tr.general");
  point->set_fc(IEC61850Proto::FUNCTIONAL_CONSTRAINT_ST);
  point->set_source(IEC61850Proto::POINT_SOURCE_GOOSE);
  point->set_value_type(IEC61850Proto::POINT_VALUE_TYPE_BOOL);
  return config;
}

// 验证：空数据库加载为schema_version=1的空聚合配置。
TEST(IEC61850ConfigStoreTest, LoadsEmptyConfigWhenRecordDoesNotExist) {
  TemporaryDirectory directory;
  IEC61850::ConfigStore store(directory.path() / "config.db");
  IEC61850Proto::PersistedConfig loaded;

  const auto status = store.Load(&loaded);

  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(loaded.schema_version(), 1u);
  EXPECT_EQ(loaded.models_size(), 0);
  EXPECT_EQ(loaded.ieds_size(), 0);
}

// 验证：聚合配置能够原子保存并完整重载。
TEST(IEC61850ConfigStoreTest, SavesAndLoadsAggregateConfig) {
  TemporaryDirectory directory;
  IEC61850::ConfigStore store(directory.path() / "config.db");
  const auto expected = MakeConfig();

  ASSERT_TRUE(store.Save(expected).ok());
  IEC61850Proto::PersistedConfig loaded;
  const auto status = store.Load(&loaded);

  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(loaded.SerializeAsString(), expected.SerializeAsString());
  EXPECT_EQ(loaded.models(0).ieds(0).data_attributes(0).access_point(),
            "AP1");
}

// 验证：保存旧单Server模型时自动补齐对象AP归属，加载后不再依赖隐式推断。
TEST(IEC61850ConfigStoreTest, NormalizesLegacySingleServerAccessPointOwnership) {
  TemporaryDirectory directory;
  IEC61850::ConfigStore store(directory.path() / "config.db");
  auto legacy = MakeConfig();
  legacy.mutable_models(0)
      ->mutable_ieds(0)
      ->mutable_data_attributes(0)
      ->clear_access_point();

  ASSERT_TRUE(store.Save(legacy).ok());
  IEC61850Proto::PersistedConfig loaded;
  const auto status = store.Load(&loaded);

  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(loaded.models(0).ieds(0).data_attributes(0).access_point(),
            "AP1");
}

// 验证：旧模型存在多个Server AP时不猜测空归属，保存前要求重新导入。
TEST(IEC61850ConfigStoreTest, RejectsLegacyUnscopedMultipleServerModel) {
  TemporaryDirectory directory;
  IEC61850::ConfigStore store(directory.path() / "config.db");
  auto legacy = MakeConfig();
  auto* modelIed = legacy.mutable_models(0)->mutable_ieds(0);
  auto* secondAccessPoint = modelIed->add_access_points();
  secondAccessPoint->set_name("AP2");
  secondAccessPoint->set_has_server(true);
  modelIed->mutable_data_attributes(0)->clear_access_point();

  const auto status = store.Save(legacy);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_NE(status.error_message().find("重新导入SCL"), std::string::npos);
}

// 验证：非法聚合配置在写入SQLite前被拒绝。
TEST(IEC61850ConfigStoreTest, RejectsInvalidConfigBeforeSave) {
  TemporaryDirectory directory;
  IEC61850::ConfigStore store(directory.path() / "config.db");
  auto invalid = MakeConfig();
  invalid.mutable_ieds(0)->mutable_config()->set_model_name("missing");

  const auto status = store.Save(invalid);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  IEC61850Proto::PersistedConfig loaded;
  ASSERT_TRUE(store.Load(&loaded).ok());
  EXPECT_TRUE(loaded.ieds().empty());
}

// 验证：Clear删除SQLite配置键，不保留会触发模块自动启动的空blob。
TEST(IEC61850ConfigStoreTest, ClearRemovesManagedConfigTrace) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "config.db";
  IEC61850::ConfigStore store(database);
  ASSERT_TRUE(store.Save(MakeConfig()).ok());

  ASSERT_TRUE(store.Clear().ok());
  mskdsp::ConfigDatabase db(database);
  bool found = true;
  ASSERT_TRUE(db.HasAnyBlob("IEC61850", {"config"}, &found).ok());
  EXPECT_FALSE(found);
}

}  // namespace
