# DLT645 模块

## 简介
DLT645 模块负责管理 DLT645 协议链路与点表，按设备维度支持标准版与 PCD 版配置。

## 能力清单
- 设备级协议变体选择：DLT645std / DLT645PCD。
- 设备级点表配置：tag/di/data_len/type/access/scale/offset/deadband。
- 支持数据块读取：按数据块 DI 读整块数据并拆分发布子点位。
- 传输方式类型标识（默认 MQTT_UART，串口后续补充）。
- 点表下发时同步 DataCenter 连接与标签。

## 接口与协议
- Protobuf：`protobuf/DLT645.proto`
- gRPC Service：`DLT645Proto::DLT645Service`

## 运行与地址
- 对外 gRPC：随机选择 `0.0.0.0:<port>`（7001–7999）
- 内部 gRPC：`unix socket`：`./socket/DLT645.sock`
- 运行时可通过管理器 `GetRunningModuleInfo` 查询实际地址

## 配置与数据
- 配置来源：由 ConfigPusher 通过 gRPC 下发。
- 示例文件：`package/conf/configPusher/DLT645.jsonc`（仅示例）。
- MQTT 连接参数由 DLT645 在调用 MQTTManager 时携带，配置位于 `dlt645.mqtt`。

### 配置结构
顶层采用 `links` 组织方式，结构与 Modbus/IEC104 类似：

- `mqtt`
  - `host/port/client_id/username/password/keepalive_sec/clean_session/connect_timeout_ms`：MQTT 连接参数，全局复用。
- `link.config`
  - `conn_name`：链接名称（模块内唯一）。
  - `protocol_variant`：`DLT645std` 或 `DLT645PCD`。
  - `meter_addr`：表计地址；PCD 模式下为协议转换器地址。
  - `device_no`：PCD 专用，十六进制字符串（例如 `0A` 表示第 10 个设备）。
  - `transport_type`：传输类型；为空时默认 `TRANSPORT_MQTT_UART`。
  - `comm_mode`：通信方式：`COMM_MODE_LORA`/`COMM_MODE_CARRIER`/`COMM_MODE_SERIAL`。
  - `poll_interval_ms`：轮询周期（毫秒，0 使用默认值）。
  - `request_timeout_ms`：请求超时（毫秒，0 使用默认值）。
- `point_table`
  - `replace`：是否全量替换点表。
  - `points[]`：点位定义。
  - `blocks[]`：数据块定义（按 items 顺序拼接）。

### 点表字段说明
- `tag`：与 DataCenter 对应的点名。
- `di`：4 字节数据标识，使用 8 位十六进制字符串表示；配置为人读顺序（高字节在前），模块发送时按字节逆序（低字节在前）。
- `data_len`：数据域字节数（不包含 DI 或设备序号）。
- `type`：数据类型（例如 `DATA_TYPE_UINT16/UINT32/FLOAT/STRING/BCD/BOOL`）。
- `access`：读写属性（`ACCESS_READ_ONLY/ACCESS_WRITE_ONLY/ACCESS_READ_WRITE`）。
- `scale/offset`：工程量换算 `value = raw * scale + offset`（`scale=0` 视为 1）。
- `deadband`：工程量单位，`<=0` 不过滤；BOOL 忽略 `scale/offset/deadband`。

### 数据块字段说明
- `block_di`：数据块 DI，8 位十六进制字符串；配置为人读顺序（高字节在前），模块发送时按字节逆序（低字节在前）。
- `block_data_len`：数据块数据域总长度（不包含 DI 或设备序号）。
- `items[]`：子项定义，按表格顺序拼接；`data_len` 之和必须等于 `block_data_len`。
- `trim_right_space`：ASCII 字段右侧空格裁剪（未配置默认裁剪）。

### 读块写点规则
- 同一 `tag` 同时出现在 `points` 与 `blocks.items` 时：
  - 读：优先使用数据块（单点读会被跳过）。
  - 写：仍允许使用单点配置（需 `ACCESS_WRITE_ONLY/ACCESS_READ_WRITE`）。
  - 要求：`data_len/type/scale/offset/deadband` 必须一致，否则拒绝配置。

### 配置示例
```jsonc
{
  "dlt645": {
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
            "conn_name": "表计-01",
            "protocol_variant": "DLT645PCD",
            "meter_addr": "123456789012",
            "device_no": "0A",
            "transport_type": "TRANSPORT_MQTT_UART",
            "comm_mode": "COMM_MODE_LORA",
            "poll_interval_ms": 1000,
            "request_timeout_ms": 3000
          }
        },
        "point_table": {
          "replace": true,
          "points": [
            {
              "tag": "A相电压",
              "di": "00010000",
              "data_len": 2,
              "type": "DATA_TYPE_UINT16",
              "access": "ACCESS_READ_ONLY",
              "scale": 1.0,
              "offset": 0.0,
              "deadband": 0.0
            }
          ],
          "blocks": [
            {
              // 电压数据块：DI=0201FF00，总长度 9（A/B/C 三相各 3 字节 BCD）
              "block_di": "0201FF00",
              "block_data_len": 9,
              "items": [
                {
                  "tag": "A相电压",
                  "data_len": 3,
                  "type": "DATA_TYPE_BCD",
                  "access": "ACCESS_READ_ONLY",
                  "scale": 0.01,
                  "offset": 0.0,
                  "deadband": 0.0
                },
                {
                  "tag": "B相电压",
                  "data_len": 3,
                  "type": "DATA_TYPE_BCD",
                  "access": "ACCESS_READ_ONLY",
                  "scale": 0.01,
                  "offset": 0.0,
                  "deadband": 0.0
                },
                {
                  "tag": "C相电压",
                  "data_len": 3,
                  "type": "DATA_TYPE_BCD",
                  "access": "ACCESS_READ_ONLY",
                  "scale": 0.01,
                  "offset": 0.0,
                  "deadband": 0.0
                }
              ]
            }
          ]
        },
        "start": true
      }
    ]
  }
}
```

### DI 字节序说明（上位机对接建议）
- 配置 `di` 使用高字节在前的字符串顺序，例如 `02010100`。
- 模块发送时按字节逆序，实际发送为 `00 01 01 02`；PCD 会在 DI 后追加 `device_no`。
- 上位机展示/录入时按配置顺序理解，无需手工反转。

## 协议差异（DLT645PCD）
- 标准版帧结构：`68 + 地址 + 68 + DI(4字节) + data + CS + 16`。
- PCD 版帧结构：`68 + 地址 + 68 + DI(4字节) + 设备序号(1字节) + data + CS + 16`。
- `device_no` 用于区分协议转换器下的设备，响应帧与请求一致。
- PCD 的设备序号视作 DI 第 5 字节，按 0x33 编解码规则处理。

## MQTT 对接说明（Lora/载波）
- 通信方式为 `COMM_MODE_LORA` 或 `COMM_MODE_CARRIER` 时，DLT645 通过 MQTTManager 与对下通信 APP 交互。
- 请求主题：
  - Lora：`AGVC/loraManager/JSON/action/request/monitorNode`
  - 载波：`AGVC/ccoRouter/JSON/action/request/monitorNode`
- 响应主题：
  - Lora：`loraManager/AGVC/JSON/action/response/monitorNode`
  - 载波：`ccoRouter/AGVC/JSON/action/response/monitorNode`
- 请求 JSON 字段：`token/timestamp/prio/acqAddr/data(base64)`。
- 响应 JSON 字段：`token/timestamp/prio/acqAddr/data(base64)/status`（`status=0` 表示成功）。
- 档案管理（仅 Lora/载波）：启动时发送 `addslaveNode`，停止时发送 `delslaveNode`。
- 串口方式（`COMM_MODE_SERIAL`）后续再补充。

### 发送时机与可靠性说明
- `addslaveNode` 在 `StartLink` 时触发发送（仅一次）；消息默认不保留（retain=false）。
- 如订阅端在发送之后才订阅，可能错过该消息；建议订阅端使用 QoS 1 + 持久会话（`clean_session=false`）。
- 若需要再次发送，可通过重新启动该连接（StopLink/StartLink）触发。

### 常见问题排查
- 启动连接失败且日志显示 `Deadline Exceeded`：通常为 MQTT 短暂断连或订阅/发布阻塞导致；检查 `MQTTManager.log` 是否有 `Disconnected/订阅失败/发布失败`。
- 订阅端偶尔收不到：请确认订阅时机、broker 地址是否一致，并建议使用 QoS 1 + 持久会话。

### 变更记录
- 2026-02-03：新增数据块配置（blocks/items/trim_right_space）及读块写点规则说明。

## 线程与日志
- 模块内部线程统一使用 `ModuleManager::StartModuleThread(模块LibInfo.LIB_NAME, ...)` 创建，自动绑定日志模块名上下文。
- 无需在入口手动创建 `ModuleManager::LogModuleScope`，统一规则见 `src/core/ModuleManager/doc/README.md`。

## 构建产物
- 共享库：`package/module/libDLT645.so.<version>`（版本见 `src/DLT645/include/DLT645LibInfo.h`）
