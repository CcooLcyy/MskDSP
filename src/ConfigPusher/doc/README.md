# ConfigPusher 模块

## 简介
ConfigPusher 读取 JSONC 配置文件，自动启动 DataCenter/IEC104/ModbusRTU/DLT645/AGC，并按配置调用对应 gRPC 接口完成 IEC104/ModbusRTU/DLT645 连接与点表下发、AGC 控制组下发，以及 DataCenter 连接标签注册表/路由下发。

## 能力清单
- 自动通过 ModuleManager 启动 DataCenter 与 IEC104/ModbusRTU/DLT645/AGC
- 解析 JSONC（支持 `//` 与 `/* */` 注释）
- 下发 IEC104 配置：UpsertLink / UpsertPointTable
- 下发 ModbusRTU 配置：UpdateConfig / UpsertLink / UpsertPointTable
- 下发 DLT645 配置：UpdateConfig / UpsertLink / UpsertPointTable / StartLink
- 下发 AGC 配置：UpsertGroup / StartGroup
- 下发 DataCenter 配置：UpsertConnTags / UpsertRoutes（仅对已存在连接生效）
- 下发流程记录请求/响应报文日志（ModuleManager/IEC104/ModbusRTU/DLT645/AGC/DataCenter）
- 失败记录日志（当前不做重试）

## 接口与协议
- Protobuf：`protobuf/ConfigPusher.proto`
- gRPC Service：`ConfigPusherProto::ConfigPusherService`

## 运行与地址
- 对外 gRPC：随机选择 `0.0.0.0:<port>`（7001–7999）
- 内部 gRPC：`unix socket`：`./socket/ConfigPusher.sock`
- 运行时可通过管理器 `GetRunningModuleInfo` 查询实际地址

## 启动方式
- 默认由 ModuleManager 的 `StartModule` 启动。
- 如需随系统启动自动加载，在 `./conf/module_manager.jsonc` 的 `auto_start_modules` 中加入 `ConfigPusher`。
- 建议：自启动列表仅填写 `ConfigPusher`，其余模块由 ConfigPusher 按配置按需启动。
- `ConfigPusher` 是否在启动后立即执行配置下发，受 `./conf/module_manager.jsonc` 的 `boot_config_mode` 控制：
  - `CONFIG_PUSHER`：启动后读取 JSONC 并执行配置下发
  - `UPPER`：启动后仅提供 gRPC 服务与日志，不执行配置下发
- `boot_config_mode` 仅在 `MskDSP` 进程启动时读取一次；运行中修改配置文件不会立即生效，需重启后生效。

## 配置与数据
- 配置文件：
  - `./conf/configPusher/DataCenter.jsonc`
  - `./conf/configPusher/DLT645.jsonc`
  - `./conf/configPusher/agc.jsonc`
  - `./conf/configPusher/iec104.jsonc`
  - `./conf/configPusher/modbus_rtu.jsonc`
- 使用 Protobuf JSON 映射：枚举需写全名（例如 `ROLE_SERVER`、`POINT_TYPE_FLOAT`、`FUNCTION_READ_COILS`）
- `modbus_rtu.jsonc` 的 `function` 支持十六进制字符串（`0x01`/`0x03`/`0x04`/`0x06`/`0x10`），解析时会自动转换为枚举值
- `point_table.conn_name` 可省略（默认使用 `link.config.conn_name`）
- DLT645 的 `point_table` 支持“仅 points”“仅 blocks”或“points + blocks”三种形式；只要存在任一有效点表内容，ConfigPusher 就会下发对应请求
- DLT645 支持 `device_nos` 批量设备序号展开：一条任务可展开为多条链路下发
- DLT645 `device_nos` 展开规则：
  - 仅用于 `DLT645PCD`
  - 每项需为 2 位十六进制字符串（如 `01`、`0A`）
  - `link.config.conn_name` 包含 `{device_no}` 时替换占位符；不包含时自动追加 `_{device_no}`
  - `point_table.conn_name` 为空时自动使用展开后的连接名
- 上位机对接建议：对同一协议转换器维护一份公共链路模板 + `device_nos` 列表，通过 `{device_no}` 生成连接名，避免为每个逆变器重复维护整份配置
- IEC104 可选下发点值上送参数（`point_batch_window_ms/point_max_asdu_bytes/point_use_standard_limit/point_dedupe/point_with_time`；默认不带时标）
- IEC104 可选下发对时触发 tag（`time_sync_tag`；为空时默认 `__time_sync__`）
- IEC104 点表类型支持 `POINT_TYPE_FLOAT` 与 `POINT_TYPE_SINGLE`
- AGC 配置使用 `agc.groups[].upsert` 下发控制组；`agc.groups[].start` 为兼容保留字段，当前仅记录日志，不再额外调用 `StartGroup`
- AGC 控制组配置已不再包含 `loop`、`kp`、`deadband_kw`、`max_step_kw` 等旧闭环参数；ConfigPusher 只接受当前 `GroupConfig` 结构
- AGC 会在 `p_cmd`、成员量测或 `base_tag` 等相关输入点变化时，直接按 `p_cmd` 计算出的目标总功率进行成员分配；成员上下限、不可控成员扣减、`ABSOLUTE/DELTA` 与 `DELTA_BASE_LAST_TARGET` 等语义保持不变
- 若 JSONC 里仍保留旧 `loop` 字段，ConfigPusher 会在解析阶段直接报错，避免继续向 AGC 下发过期配置
- DataCenter 配置要求连接已存在（由模块或上位机创建）；若 `point_tables/routes` 引用连接不存在，则该次 DataCenter 配置不下发。注意：这里的 `point_tables` 配置项当前仍沿用历史字段名，实际对应 DataCenter 的连接标签注册表 `ConnTags`。
- `replace=true` 表示覆盖配置；`replace=false` 表示增量追加
- ModbusRTU 支持双传输并存：`TRANSPORT_SERIAL` 保留本地串口直连；`TRANSPORT_MQTT_UART` 通过 `MQTTManager + uartManager` 做串口透传
- `modbus_rtu.mqtt` 为 ModbusRTU 的 MQTT 全局连接参数，字段为 `host/port/client_id/username/password/keepalive_sec/clean_session/connect_timeout_ms`
- 当 `modbus_rtu.links[].link.config.transport_type=TRANSPORT_MQTT_UART` 时，`modbus_rtu.mqtt` 必填；ConfigPusher 会先调用 `ModbusRTU.UpdateConfig`，再继续下发链路与点表
- `TRANSPORT_MQTT_UART` 链路要求配置 `serial_port/request_timeout_ms/serial_byte_timeout_ms/serial_frame_timeout_ms/serial_est_size`
- ModbusRTU 链路固定按主站方式运行
- 当 DLT645 或 ModbusRTU 需要 MQTT 时，ConfigPusher 会按需启动 `MQTTManager`
- DLT645 配置会启动 DLT645 与 MQTTManager，并先下发 MQTT 全局参数

### ModbusRTU MQTT 串口透传配置
- 适用场景：需要通过远端 `uartManager` 操作串口，但同时保留本地串口直连能力。
- 顶层 MQTT 参数位置：`modbus_rtu.mqtt`
- 链路传输类型：`modbus_rtu.links[].link.config.transport_type = TRANSPORT_MQTT_UART`
- 远端串口标识：`serial_port`，例如 `RS485-1`
- 远端串口参数仍写在 `serial` 中，但此时 `serial.device` 会被忽略
- 下发顺序固定为：
  1. `UpdateConfig`
  2. `UpsertLink`
  3. `UpsertPointTable`
  4. 模块依据当前配置自动判定是否启动连接功能（`start` 仅保留兼容日志）

ModbusRTU MQTT 配置示例：
```jsonc
{
  "modbus_rtu": {
    "mqtt": {
      "host": "127.0.0.1",
      "port": 1883,
      "client_id": "dlt645",
      "username": "",
      "password": "",
      "keepalive_sec": 30,
      "clean_session": true,
      "connect_timeout_ms": 3000
    },
    "links": [
      {
        "link": {
          "config": {
            "conn_name": "modbus-remote-1",
            "transport_type": "TRANSPORT_MQTT_UART",
            "serial": {
              "baud_rate": 9600,
              "data_bits": 8,
              "parity": "PARITY_NONE",
              "stop_bits": "STOP_BITS_ONE"
            },
            "serial_port": "RS485-1",
            "request_timeout_ms": 3000,
            "serial_byte_timeout_ms": 100,
            "serial_frame_timeout_ms": 100,
            "serial_est_size": 256,
            "device_id": 1,
            "poll_interval_ms": 1000
          }
        },
        "point_table": {
          "replace": true,
          "points": [
            {
              "tag": "Ua",
              "function": "0x03",
              "address": 0,
              "type": "DATA_TYPE_UINT16"
            }
          ]
        },
        "start": true
      }
    ]
  }
}
```

### 上位机对接建议（ModbusRTU MQTT）
- 在界面上将 `transport_type` 作为显式选项，避免把本地串口与远端串口透传混为一类。
- 当用户选择 `TRANSPORT_MQTT_UART` 时，直接联动显示顶层 `mqtt` 配置区与链路级 `serial_port` 配置区。
- 在配置校验阶段直接提示：
  - 缺少 `modbus_rtu.mqtt`
  - 缺少 `serial_port`
- 启停文案建议写成“启动连接功能/停止连接功能”，避免与“启动模块”混淆。

### DLT645 批量设备下发（device_nos）
- 适用场景：一个协议转换器（同一 `meter_addr`）下挂多台逆变器，仅 `device_no` 不同。
- 配置入口：`dlt645.links[].device_nos`（数组）。
- 展开规则：
  - 每个 `device_no` 会展开为一条独立 DLT645 连接任务（独立 UpsertLink/UpsertPointTable/StartLink）。
  - 除 `device_no` 与 `conn_name` 外，其余配置保持一致。
  - `conn_name` 包含 `{device_no}` 时按值替换；不包含时自动追加 `_{device_no}`。
  - `point_table.conn_name` 为空时，默认使用展开后的连接名。
- 约束：
  - 仅支持 `DLT645PCD`。
  - `device_no` 必须为 2 位十六进制字符串（例如 `01`、`0A`）。
  - 展开后连接名必须唯一，否则该任务下发失败。

DLT645 批量下发示例：
```jsonc
{
  "dlt645": {
    "links": [
      {
        "link": {
          "config": {
            "conn_name": "convA_{device_no}",
            "protocol_variant": "DLT645PCD",
            "meter_addr": "202601200001",
            "comm_mode": "COMM_MODE_LORA"
          }
        },
        "point_table": {
          "replace": true,
          "points": [
            {
              "tag": "有功功率设定值",
              "di": "06010801",
              "data_len": 4,
              "type": "DATA_TYPE_BCD",
              "access": "ACCESS_WRITE_ONLY"
            }
          ]
        },
        "device_nos": ["01", "02", "03"],
        "start": true
      }
    ]
  }
}
```

### 上位机对接建议（DLT645 批量设备）
- 建议上位机提供“公共模板 + 设备序号列表”建模，避免为每台逆变器维护整份重复 JSON。
- 建议在界面保存并展示“展开预览”（最终连接名列表），防止连接名冲突。
- 建议默认使用 `conn_name` 占位符 `{device_no}`，便于问题定位与日志检索。
- 建议在配置校验阶段直接提示以下错误：
  - `device_no` 非 2 位十六进制；
  - 非 `DLT645PCD` 却配置 `device_nos`；
  - 展开后连接名重复。

### 上位机对接建议（AGC）
- AGC 界面与配置模板中不要再展示 `loop`、`kp`、`deadband_kw`、`max_step_kw` 等旧参数。
- 建议将 AGC 总目标描述为“相关输入变化时直接按目标总功率分配”，避免继续使用“分步逼近”“步长限制”“固定周期轮询”之类旧文案。
- 若成员设定值使用 `VALUE_MODE_DELTA + DELTA_BASE_LAST_TARGET`，建议在界面上明确提示其基准是 AGC 记忆的上一轮期望目标，而不是旧闭环输出步长。
- 建议在配置校验阶段直接提示“存在已废弃的 AGC loop 字段”，避免用户把旧版模板继续下发到 ConfigPusher。

DataCenter 示例：
```jsonc
{
  "point_tables": [
    {
      "module_name": "IEC104",
      "conn_name": "line-1",
      "replace": true,
      "tags": ["P_CMD_SRC", "P_TOTAL_DST"]
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
```

IEC104 示例：
```jsonc
{
  "iec104": {
    "links": [
      {
        "link": {
          "create_only": true,
          "config": {
            "conn_name": "line-1",
            "role": "ROLE_SERVER",
            "local": { "ip": "0.0.0.0", "port": 2404 },
            "remote": { "ip": "", "port": 0 },
            "ca": 1,
            "oa": 1,
            "apci": { "k": 12, "w": 8, "t0": 30, "t1": 15, "t2": 10, "t3": 20 },
            "point_batch_window_ms": 20,
            "point_max_asdu_bytes": 240,
            "point_use_standard_limit": false,
            "point_dedupe": true,
            "point_with_time": false,
            "time_sync_tag": "__time_sync__"
          }
        },
        "point_table": {
          "replace": true,
          "points": [
            { "tag": "Ua", "ioa": 100, "type": "POINT_TYPE_FLOAT" }
          ]
        },
        "start": true
      }
    ]
  }
}
```
ModbusRTU 示例见 `./conf/configPusher/modbus_rtu.jsonc`。
AGC 示例见 `./conf/configPusher/agc.jsonc`。

## 线程与日志
- 模块内部线程统一使用 `ModuleManager::StartModuleThread(模块LibInfo.LIB_NAME, ...)` 创建，自动绑定日志模块名上下文。
- 无需在入口手动创建 `ModuleManager::LogModuleScope`，统一规则见 `src/core/ModuleManager/doc/README.md`。

## 构建产物
- 共享库：`package/module/libConfigPusher.so.<version>`（版本见 `src/ConfigPusher/cmake/LibInfo.cmake`）
