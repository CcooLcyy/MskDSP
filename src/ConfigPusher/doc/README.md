# ConfigPusher 模块

## 简介
ConfigPusher 读取 JSONC 配置文件，自动启动 DataCenter/IEC104/ModbusRTU/DLT645/AGC/AVC/Calc，并按配置调用对应 gRPC 接口完成 IEC104/ModbusRTU/DLT645 连接与点表下发、AGC/AVC 控制组下发、Calc 计算分组下发，以及 DataCenter 连接标签注册表/路由下发。链路、控制组或计算分组的模块内功能是否进入运行态，由各模块在配置达到可运行条件后自动判定。

在 `CONFIG_PUSHER` 模式下，ConfigPusher 将 `jsonc` 视为当前进程的目标态与最终真相源，而不是增量补丁：若 SQLite 持久化配置或当前内存态中存在 `jsonc` 未声明的链路、控制组、计算分组、点表、连接标签注册表或路由，ConfigPusher 会在本次编排时将其收敛删除或覆盖，避免旧持久化内容继续生效。

ConfigPusher 更适合作为初始化配置导入与批量编排执行器，不作为上位机日常在线操作的统一入口。

## 能力清单
- 自动通过 ModuleManager 启动 DataCenter 与 IEC104/ModbusRTU/DLT645/AGC/AVC/Calc
- 解析 JSONC（支持 `//` 与 `/* */` 注释）
- 下发 IEC104 配置：UpsertLink / UpsertPointTable
- 下发 ModbusRTU 配置：UpdateConfig / UpsertLink / UpsertPointTable
- 下发 DLT645 配置：UpdateConfig / UpsertLink / UpsertPointTable
- 下发 AGC 配置：UpsertGroup
- 下发 AVC 配置：UpsertGroup
- 下发 Calc 配置：UpsertGroup
- 下发 DataCenter 配置：UpsertConnTags / UpsertRoutes（仅对已存在连接生效）
- 下发流程记录请求/响应报文日志（ModuleManager/IEC104/ModbusRTU/DLT645/AGC/AVC/Calc/DataCenter）
- 失败记录日志（当前不做重试）
- 在 `CONFIG_PUSHER` 模式下按 `jsonc` 目标态收敛：删除 `jsonc` 未声明的旧链路/控制组/计算分组，并覆盖点表/连接标签注册表/路由
- 对 `start` 字段仅保留兼容日志，不再额外调用 `StartLink/StartGroup`

## 对接文档

`ConfigPusher` 的能力矩阵、模块 RPC 映射、导入执行语义与上位机同步边界，已收束到独立文档：

- [`doc/ConfigPusher能力矩阵.md`](../../../doc/ConfigPusher能力矩阵.md)

README 这里只保留模块说明、启动方式、配置入口与基础语义；若涉及上位机模板建模、导入预检、结果展示与模块间编排语义，请优先参考上述矩阵文档。

## 接口与协议
- Protobuf：`protobuf/ConfigPusher.proto`
- gRPC Service：`ConfigPusherProto::ConfigPusherService`

## 运行与地址
- 对外 gRPC：随机选择 `0.0.0.0:<port>`（17001–17999）
- 内部 gRPC：`unix socket`：`./socket/ConfigPusher.sock`
- 运行时可通过管理器 `GetRunningModuleInfo` 查询实际地址

## 启动方式
- 默认由 ModuleManager 的 `StartModule` 启动。
- 如需随系统启动自动加载，在 `./conf/module_manager.jsonc` 的 `auto_start_modules` 中加入 `ConfigPusher`。
- 建议：自启动列表仅填写 `ConfigPusher`，其余模块由 ConfigPusher 按配置按需启动。
- `ConfigPusher` 是否在启动后立即执行配置下发，受 `./conf/module_manager.jsonc` 的 `boot_config_mode` 控制：
  - `CONFIG_PUSHER`：启动后读取 JSONC 并执行配置下发
  - `UPPER`：启动后仅提供 gRPC 服务与日志，不执行配置下发；即使 `ModuleManager` 因 SQLite 持久化配置痕迹自动启动了其他模块，`ConfigPusher` 也不会自动触发配置下发
- `boot_config_mode` 仅在 `MskDSP` 进程启动时读取一次；运行中修改配置文件不会立即生效，需重启后生效。

### CONFIG_PUSHER 目标态语义
- `jsonc` 是目标态快照，不是增量补丁；当前模块内存或 SQLite 持久化配置中未在 `jsonc` 声明的旧对象，不应继续保留为有效配置。
- ConfigPusher 在下发前会先查询模块当前对象集合；对 `jsonc` 未声明的旧链路/旧控制组/旧计算分组，会先执行清理，再继续下发目标配置。
- 对 `jsonc` 中仍保留的同名对象，若其模块内功能已在运行，ConfigPusher 会先停止模块内功能，再按 `jsonc` 覆盖目标配置，避免旧运行态阻塞收敛。
- 点表、DataCenter 连接标签注册表与路由按目标态覆盖，避免 SQLite 旧配置或旧内存态中的残留条目继续生效。
- `start` 字段仍仅用于兼容旧模板与日志说明；ConfigPusher 不会因该字段额外调用 `StartLink/StartGroup`，模块会在配置收敛完成后依据当前目标态自动判定是否启动模块内功能。

## 配置与数据
- 配置文件：
  - `./conf/configPusher/DataCenter.jsonc`
  - `./conf/configPusher/DLT645.jsonc`
  - `./conf/configPusher/agc.jsonc`
  - `./conf/configPusher/avc.jsonc`
  - `./conf/configPusher/calc.jsonc`
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
- 若需要为同一协议转换器批量生成多条连接，可通过 `{device_no}` 与 `device_nos` 做统一展开。
- IEC104 可选下发点值上送参数（`point_batch_window_ms/point_max_asdu_bytes/point_use_standard_limit/point_dedupe/point_with_time`；默认不带时标）
- IEC104 可选下发对时触发 tag（`time_sync_tag`；为空时默认 `__time_sync__`）
- IEC104 点表类型支持 `POINT_TYPE_FLOAT` 与 `POINT_TYPE_SINGLE`
- AGC 配置使用 `agc.groups[].upsert` 下发控制组；`agc.groups[].start` 为兼容保留字段，当前仅记录日志，不再额外调用 `StartGroup`
- AGC 控制组配置已不再包含 `loop`、`kp`、`deadband_kw`、`max_step_kw` 等旧闭环参数；ConfigPusher 只接受当前 `GroupConfig` 结构
- AGC 会在 `p_cmd`、成员量测或 `base_tag` 等相关输入点变化时，直接按 `p_cmd` 计算出的目标总功率进行成员分配；成员上下限、不可控成员扣减、`ABSOLUTE/DELTA` 与 `DELTA_BASE_LAST_TARGET` 等语义保持不变
- 若 JSONC 里仍保留旧 `loop` 字段，ConfigPusher 会在解析阶段直接报错，避免继续向 AGC 下发过期配置
- AVC 配置使用 `avc.groups[].upsert` 下发控制组；`avc.groups[].start` 为兼容保留字段，当前仅记录日志，不再额外调用 `StartGroup`
- AVC 的 `jsonc` 改名语义按“删除旧组 + 创建新组”处理；ConfigPusher 不提供显式 `RenameGroup` 任务
- AVC 支持 `voltage_cmd` 或 `q_total_cmd` 两类主命令输入；ConfigPusher 只负责按 `jsonc` 收敛下发，控制组是否自动进入运行态由 AVC 模块依据当前配置判定
- Calc 配置使用 `calc.groups[].upsert` 下发计算分组；`calc.groups[].start` 为兼容保留字段，当前仅记录日志，不再额外调用 `StartGroup`
- Calc 的 `jsonc` 改名语义按“删除旧组 + 创建新组”处理；ConfigPusher 不提供显式 `RenameGroup` 任务
- Calc 计算项中的 `ROUTED_INPUT` 只声明输入槽位，外部源点仍通过 DataCenter Route 绑定到 `<item_name>/left_input` 或 `<item_name>/right_input`；计算结果发布到 `<item_name>/result`
- DataCenter 配置要求连接已存在（由模块或上位机创建）；若 `point_tables/routes` 引用连接不存在，则该次 DataCenter 配置不下发。注意：这里的 `point_tables` 配置项当前仍沿用历史字段名，实际对应 DataCenter 的连接标签注册表 `ConnTags`。
- 在 `CONFIG_PUSHER` 严格目标态语义下，`point_tables` 视为 ConnTags 的完整目标集合；路由中涉及的连接与 tag 必须在 `point_tables` 中显式声明，否则会在写入 DataCenter 前校验失败，不执行 ConnTags/Routes 写入。
- 对 ConfigPusher 而言，`jsonc` 表达的是最终目标态：即使底层 gRPC 结构复用了 `replace` 字段，ConfigPusher 也会确保最终生效结果不保留 `jsonc` 未声明的旧条目
- ModbusRTU 支持双传输并存：`TRANSPORT_SERIAL` 保留本地串口直连；`TRANSPORT_MQTT_UART` 通过 `MQTTManager + uartManager` 做串口透传
- `modbus_rtu.mqtt` 为 ModbusRTU 的 MQTT 全局连接参数，字段为 `host/port/client_id/username/password/keepalive_sec/clean_session/connect_timeout_ms`
- 当 `modbus_rtu.links[].link.config.transport_type=TRANSPORT_MQTT_UART` 时，`modbus_rtu.mqtt` 必填；ConfigPusher 会先调用 `ModbusRTU.UpdateConfig`，再继续下发链路与点表
- `TRANSPORT_MQTT_UART` 链路要求配置 `serial_port/request_timeout_ms/serial_byte_timeout_ms/serial_frame_timeout_ms/serial_est_size`
- ModbusRTU 链路固定按主站方式运行
- 当 DLT645 或 ModbusRTU 需要 MQTT 时，ConfigPusher 会按需启动 `MQTTManager`
- DLT645 配置会启动 DLT645 与 MQTTManager，并先下发 MQTT 全局参数
- `iec104.links[].start`、`modbus_rtu.links[].start`、`dlt645.links[].start`、`agc.groups[].start`、`avc.groups[].start` 与 `calc.groups[].start` 当前均为兼容保留字段：ConfigPusher 仅输出兼容日志，模块会在配置达到可运行条件后自动启动模块内功能

### 设计与验收要点
- 若 SQLite 持久化配置或当前内存态中存在两条 DLT645/IEC104/ModbusRTU 链路，而本次 `jsonc` 只声明一条，则本次下发完成后最终有效链路应仅剩 `jsonc` 声明的那一条。
- 若 SQLite 持久化配置或当前内存态中存在 `jsonc` 未声明的 AGC 控制组，则该旧控制组不应继续保留为有效配置，也不应继续运行控制组功能。
- 若 SQLite 持久化配置或当前内存态中存在 `jsonc` 未声明的 AVC 控制组，则该旧控制组不应继续保留为有效配置，也不应继续运行控制组功能。
- 若 SQLite 持久化配置或当前内存态中存在 `jsonc` 未声明的 Calc 计算分组，则该旧分组不应继续保留为有效配置，也不应继续运行分组运算功能。
- 若同名链路/控制组/计算分组仍被 `jsonc` 保留，但其点表、标签、路由或分组配置内容发生变化，则最终以 `jsonc` 内容为准，旧条目不应残留。

涉及上位机页面结构、模板建模、交互校验与导入流程的统一说明，见 `doc/上位机设计指导.md`。本节以下内容仅保留 ConfigPusher 的字段语义、展开规则与校验约束。

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

### DLT645 批量设备下发（device_nos）
- 适用场景：一个协议转换器（同一 `meter_addr`）下挂多台逆变器，仅 `device_no` 不同。
- 配置入口：`dlt645.links[].device_nos`（数组）。
- 展开规则：
  - 每个 `device_no` 会展开为一条独立 DLT645 连接任务（独立 UpsertLink/UpsertPointTable；是否启动连接功能由模块自动判定）。
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

说明：
- 示例中的 `start` 字段仅用于兼容旧模板；当前版本不会因此额外调用 `StartLink/StartGroup`，模块会在配置达到可运行条件后自动启动模块内功能。

DataCenter 示例：
```jsonc
{
  "point_tables": [
    {
      "module_name": "IEC104",
      "conn_name": "line-1",
      "replace": true,
      "tags": ["P_CMD_SRC", "P_TOTAL_DST"]
    },
    {
      "module_name": "AGC",
      "conn_name": "g-1",
      "replace": true,
      "tags": ["P_CMD"]
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
AVC 示例见 `./conf/configPusher/avc.jsonc`。
Calc 示例见 `./conf/configPusher/calc.jsonc`。

## 线程与日志
- 模块内部线程统一使用 `ModuleManager::StartModuleThread(模块LibInfo.LIB_NAME, ...)` 创建，自动绑定日志模块名上下文。
- 无需在入口手动创建 `ModuleManager::LogModuleScope`，统一规则见 `src/core/ModuleManager/doc/README.md`。

## 构建产物
- 共享库：`package/module/libConfigPusher.so.<version>`（版本见 `src/ConfigPusher/cmake/LibInfo.cmake`）
