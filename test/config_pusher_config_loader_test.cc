#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "AGC.pb.h"
#include "ConfigPusherConfigLoader.h"
#include "Logger.h"
#include "ModbusRTU.pb.h"

namespace {
class ScopedTempDir {
public:
  ScopedTempDir() {
    auto base = std::filesystem::current_path();
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    path_ = base / ("config_pusher_config_loader_test_tmp_" + std::to_string(ts));
    std::filesystem::create_directories(path_);
  }

  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

void InitLoggerOnce() {
  static bool inited = false;
  if (!inited) {
    ModuleManager::Logger::init("./log", "config_pusher_config_loader_test.log");
    inited = true;
  }
}

void WriteFile(const std::filesystem::path &path, const std::string &content) {
  std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
  ofs << content;
}
}  // namespace

// 验证：加载配置时支持 JSONC 注释并转换 ModbusRTU 十六进制功能码。
TEST(ConfigPusherConfigLoaderTest, LoadConfigWithCommentsAndHexFunction) {
  InitLoggerOnce();
  ScopedTempDir dir;
  const auto path = dir.path() / "modbus_rtu.jsonc";
  const std::string content = R"json(
{
  // 配置注释
  "modbus_rtu": {
    "links": [
      {
        "point_table": {
          "points": [
            { "tag": "A", "function": "0x01", "address": 1, "type": "DATA_TYPE_BOOL" },
            { "tag": "B", "function": "0x03", "address": 2, "type": "DATA_TYPE_UINT16" },
            { "tag": "C", "function": "0x04", "address": 3, "type": "DATA_TYPE_UINT16" }
          ]
        }
      }
    ]
  }
}
)json";
  WriteFile(path, content);

  auto loaded = ConfigPusher::LoadConfigFile(path);
  ASSERT_TRUE(loaded.has_value());
  const auto &modbus = loaded->modbus_rtu();
  ASSERT_EQ(modbus.links_size(), 1);
  const auto &task = modbus.links(0);
  ASSERT_TRUE(task.has_point_table());
  const auto &pointTable = task.point_table();
  ASSERT_EQ(pointTable.points_size(), 3);
  EXPECT_EQ(pointTable.points(0).function(), ModbusRTUProto::FUNCTION_READ_COILS);
  EXPECT_EQ(pointTable.points(1).function(), ModbusRTUProto::FUNCTION_READ_HOLDING_REGISTERS);
  EXPECT_EQ(pointTable.points(2).function(), ModbusRTUProto::FUNCTION_READ_INPUT_REGISTERS);
}

// 验证：加载 DataCenter 配置时支持 JSONC 注释并解析点表与路由。
TEST(ConfigPusherConfigLoaderTest, LoadDataCenterConfigWithComments) {
  InitLoggerOnce();
  ScopedTempDir dir;
  const auto path = dir.path() / "DataCenter.jsonc";
  const std::string content = R"json(
{
  /* DataCenter 配置注释 */
  "point_tables": [
    {
      "module_name": "IEC104",
      "conn_name": "line-1",
      "tags": ["P_CMD_SRC"],
      "replace": true
    }
  ],
  "routes": {
    "replace": true,
    "routes": [
      {
        "src": { "module_name": "IEC104", "conn_name": "line-1", "tag": "P_CMD_SRC" },
        "dst": { "module_name": "AGC", "conn_name": "g-1", "tag": "P_CMD" }
      }
    ]
  }
}
)json";
  WriteFile(path, content);

  auto loaded = ConfigPusher::LoadDataCenterConfigFile(path);
  ASSERT_TRUE(loaded.has_value());
  ASSERT_EQ(loaded->point_tables_size(), 1);
  const auto &table = loaded->point_tables(0);
  EXPECT_EQ(table.module_name(), "IEC104");
  EXPECT_EQ(table.conn_name(), "line-1");
  ASSERT_EQ(table.tags_size(), 1);
  EXPECT_EQ(table.tags(0), "P_CMD_SRC");
  ASSERT_TRUE(loaded->has_routes());
  const auto &routes = loaded->routes();
  ASSERT_EQ(routes.routes_size(), 1);
  const auto &route = routes.routes(0);
  EXPECT_EQ(route.src().module_name(), "IEC104");
  EXPECT_EQ(route.dst().module_name(), "AGC");
}

// 验证：加载 AGC 配置时可解析控制组任务与 UpsertGroup 字段。
TEST(ConfigPusherConfigLoaderTest, LoadAgcConfigFile) {
  InitLoggerOnce();
  ScopedTempDir dir;
  const auto path = dir.path() / "agc.jsonc";
  const std::string content = R"json(
{
  "agc": {
    "groups": [
      {
        "upsert": {
          "create_only": false,
          "config": {
            "group_name": "g-1",
            "p_cmd": {
              "signal": { "tag": "P_CMD", "unit": "kW" },
              "mode": "VALUE_MODE_ABSOLUTE"
            },
            "members": [
              {
                "member_name": "inv-1",
                "controllable": true,
                "p_meas": { "tag": "INV1_P_MEAS", "unit": "kW" },
                "p_set": { "signal": { "tag": "INV1_P_SET", "unit": "kW" }, "mode": "VALUE_MODE_ABSOLUTE" }
              }
            ]
          }
        },
        "start": true
      }
    ]
  }
}
)json";
  WriteFile(path, content);

  auto loaded = ConfigPusher::LoadConfigFile(path);
  ASSERT_TRUE(loaded.has_value());
  ASSERT_TRUE(loaded->has_agc());
  ASSERT_EQ(loaded->agc().groups_size(), 1);
  const auto &task = loaded->agc().groups(0);
  ASSERT_TRUE(task.has_upsert());
  EXPECT_FALSE(task.upsert().create_only());
  ASSERT_TRUE(task.upsert().has_config());
  EXPECT_EQ(task.upsert().config().group_name(), "g-1");
  ASSERT_TRUE(task.upsert().config().has_p_cmd());
  EXPECT_EQ(task.upsert().config().p_cmd().signal().tag(), "P_CMD");
  EXPECT_EQ(task.upsert().config().p_cmd().mode(), AGCProto::VALUE_MODE_ABSOLUTE);
  ASSERT_EQ(task.upsert().config().members_size(), 1);
  EXPECT_TRUE(task.start());
}

// 验证：加载 DLT645 配置时支持解析 device_nos 批量设备序号字段。
TEST(ConfigPusherConfigLoaderTest, LoadDlt645ConfigWithDeviceNos) {
  InitLoggerOnce();
  ScopedTempDir dir;
  const auto path = dir.path() / "DLT645.jsonc";
  const std::string content = R"json(
{
  "dlt645": {
    "mqtt": {
      "host": "127.0.0.1",
      "port": 1883,
      "client_id": "dlt645"
    },
    "links": [
      {
        "link": {
          "config": {
            "conn_name": "conv_{device_no}",
            "protocol_variant": "DLT645PCD",
            "meter_addr": "202601200001",
            "comm_mode": "COMM_MODE_LORA"
          }
        },
        "device_nos": ["01", "0A"],
        "start": false
      }
    ]
  }
}
)json";
  WriteFile(path, content);

  auto loaded = ConfigPusher::LoadConfigFile(path);
  ASSERT_TRUE(loaded.has_value());
  ASSERT_TRUE(loaded->has_dlt645());
  ASSERT_EQ(loaded->dlt645().links_size(), 1);
  const auto &task = loaded->dlt645().links(0);
  ASSERT_EQ(task.device_nos_size(), 2);
  EXPECT_EQ(task.device_nos(0), "01");
  EXPECT_EQ(task.device_nos(1), "0A");
  EXPECT_EQ(task.link().config().conn_name(), "conv_{device_no}");
}
