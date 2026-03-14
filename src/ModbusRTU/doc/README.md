# ModbusRTU 模块

## 简介
ModbusRTU 模块负责管理 ModbusRTU 链路与点表，当前同时支持两种传输方式：
- `TRANSPORT_SERIAL`：本地串口直连，保留原有直接操作串口能力。
- `TRANSPORT_MQTT_UART`：通过 `MQTTManager + uartManager` 透传原始 RTU 报文。

控制面统一由 ConfigPusher/上位机通过 gRPC 下发；数据面通过 DataCenter 发布、读取与命令路由完成闭环。

## 能力清单
- 主站读取线圈（0x01）与保持寄存器（0x03）。
- 主站读取输入寄存器（0x04）。
- 主站写单寄存器（0x06）与写多寄存器（0x10），通过 DataCenter 路由触发。
- 本地串口直连与 MQTT UART 透传两种主站链路可并存。
- 支持显式区间抄读 `read_plan`。
- 支持点表 `scale/offset/deadband` 工程量换算。
- 支持通过 MQTT UART 透传原始 RTU 帧，并输出完整收发报文日志。

## 接口与运行
- Protobuf：`protobuf/ModbusRTU.proto`
- gRPC Service：`ModbusRTUProto::ModbusRTUService`
- 对外 gRPC：随机 `0.0.0.0:<port>`（7001-7999）
- 内部 gRPC：`./socket/ModbusRTU.sock`
- 接口规划：当前顶层全局配置仅提供 `UpdateConfig` 写接口；后续建议补充 `GetConfig` 或 `GetGlobalConfig` 回读接口，供上位机做界面回显、配置对账与一致性校验；当前版本尚未实现。

## 配置来源
- 运行时配置统一由 ConfigPusher 通过 gRPC 下发。
- 示例文件：`package/conf/configPusher/modbus_rtu.jsonc`
- 当存在 `TRANSPORT_MQTT_UART` 链路时，ConfigPusher 会先调用 `UpdateConfig` 下发顶层 `modbus_rtu.mqtt`，再继续下发链路/点表并启动连接功能。

## 配置持久化（当前实现）
- 当前实现会将已下发的 MQTT/链路/点表配置本地持久化到 `./conf/ModbusRTU/`，用于模块重启后的自动恢复。
- 持久化文件：
  - `./conf/ModbusRTU/mqtt.pb`
  - `./conf/ModbusRTU/links.pb`
  - `./conf/ModbusRTU/point_tables.pb`
- 恢复时会重新向 DataCenter 获取/确认 `conn_id`，并同步该链路的 tags 到 DataCenter 连接标签注册表。
- 恢复后的链路状态统一为 `STOPPED` 或 `PENDING_DELETE`，不会自动启动链路连接功能；如需启动连接功能，仍需由上位机或 ConfigPusher 调用 `StartLink`。
- `last_error`、总线实例、轮询线程、订阅线程与运行中的收发状态不落盘。

## 传输模式

### 1. 本地串口直连
- `transport_type = TRANSPORT_SERIAL`
- 使用 `serial.device` 打开本地串口。
- 链路按主站方式执行轮询与写寄存器。
- 继续沿用原有 `read_timeout_ms` 与串口共享逻辑。

### 2. MQTT 串口透传
- `transport_type = TRANSPORT_MQTT_UART`
- `serial_port` 表示远端 `uartManager` 的串口标识，例如 `RS485-1`。
- `serial` 中只保留远端串口参数：`baud_rate/data_bits/parity/stop_bits`。
- `serial.device` 在该模式下忽略。
- 链路按主站方式执行轮询与写寄存器。

## 顶层 MQTT 配置
仅当存在 `TRANSPORT_MQTT_UART` 链路时需要配置：

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
    }
  }
}
```

字段说明：
- `host/port`：MQTT broker 地址。
- `client_id/username/password`：连接身份参数。
- `keepalive_sec/clean_session/connect_timeout_ms`：连接行为参数。

## LinkConfig 关键字段
- `conn_name`：链路名，模块内唯一。
- `transport_type`：`TRANSPORT_SERIAL` 或 `TRANSPORT_MQTT_UART`。
- `serial`：串口参数；MQTT UART 场景下表示远端串口参数。
- `device_id`：目标设备地址，范围 `1..247`。
- `poll_interval_ms`：主站轮询周期。
- `address_base`：地址基准，支持 `ZERO/ONE`。
- `read_plan`：主站显式区间抄读配置。
- `serial_port`：MQTT UART 场景必填。
- `request_timeout_ms`：MQTT UART 单次请求超时，默认 `3000ms`。
- `serial_byte_timeout_ms`：对应 `uartManager.byteTimeout`，默认 `100ms`。
- `serial_frame_timeout_ms`：对应 `uartManager.frameTimeout`，默认 `100ms`。
- `serial_est_size`：对应 `uartManager.estSize`，默认 `256`。

## 点表说明
- `FUNCTION_READ_COILS` 仅支持 `DATA_TYPE_BOOL`。
- `FUNCTION_READ_HOLDING_REGISTERS` 支持 `DATA_TYPE_UINT16/DATA_TYPE_UINT32/DATA_TYPE_INT16/DATA_TYPE_INT32`。
- `FUNCTION_READ_INPUT_REGISTERS` 支持 `DATA_TYPE_UINT16/DATA_TYPE_UINT32/DATA_TYPE_INT16/DATA_TYPE_INT32`。
- `FUNCTION_WRITE_SINGLE_REGISTER` 只允许 `DATA_TYPE_UINT16/DATA_TYPE_INT16`、`reg_count=1`。
- `FUNCTION_WRITE_MULTIPLE_REGISTERS` 支持 `DATA_TYPE_UINT16/DATA_TYPE_UINT32/DATA_TYPE_INT16/DATA_TYPE_INT32`。
- 读寄存器点位按 `value = raw * scale + offset` 做工程量换算；写寄存器点位按 `raw = (value - offset) / scale` 反向换算。
- `deadband` 仅对读寄存器上报生效；写寄存器点位忽略。
- `scale=0` 视为 `1`，计算公式为 `value = raw * scale + offset`。
- `deadband <= 0` 表示不过滤。

## 主站写寄存器闭环
- 触发方式：上位机或其他模块先把命令写入 DataCenter 源端点，再通过 `Route` 路由到 ModbusRTU 的 `conn_id + tag`。
- ModbusRTU 主站启动连接功能后，会订阅本连接内所有写点 `tag`；收到更新后自动编码并发送 `0x06/0x10` RTU 报文。
- `FUNCTION_WRITE_SINGLE_REGISTER` 适合单个 16 位设定值。
- `FUNCTION_WRITE_MULTIPLE_REGISTERS` 适合 32 位设定值，或明确要求用 `0x10` 下发的 16 位设定值。
- 命令值支持 DataCenter `double/int/bool`；`bool` 会按 `0/1` 处理。
- 当前不额外发布“写成功确认点”；如需确认，建议由设备侧回传读点或由上位机结合现场协议响应判断。

## MQTT UART 对接约束
该部分按 UART 设计文档实现，不自定义协议。

请求 Topic：
`AGVC/uartManager/JSON/transparant/notification/{port}/data`

响应 Topic：
`uartManager/AGVC/JSON/transparant/notification/{port}/data`

请求 JSON 字段：
- `token`
- `timestamp`
- `port`
- `prio`
- `prm`
- `byteTimeout`
- `frameTimeout`
- `taskTimeout`
- `param`
- `estSize`
- `data`（Base64 编码后的 RTU 原始帧）

响应 JSON 字段：
- `token`
- `timestamp`
- `prm`
- `port`
- `status`
- `data`（Base64 编码后的 RTU 原始帧）

说明：
- 设计文档中的 Topic 关键字拼写固定为 `transparant`，实现保持一致。
- MQTT UART 路径对 ModbusRTU 只做原始 RTU 报文透传，不做 645 式档案管理。

## 示例
完整示例见 `package/conf/configPusher/modbus_rtu.jsonc`，其中包含：
- 一个 `TRANSPORT_SERIAL` 本地串口主站示例。
- 一个 `TRANSPORT_MQTT_UART` 远端串口主站示例。

## 上位机对接建议
- 配置 MQTT UART 链路时，界面上应同时展示顶层 `mqtt` 与链路级 `serial_port/serial.*` 参数。
- 启停文案建议明确为“启动连接功能/停止连接功能”，避免与“启动模块”混淆。
- 无需展示 `mode` 字段；链路固定按主站方式运行。
- 若点位类型选择 `INT16/INT32`，应限制为主站寄存器场景（`0x03/0x04/0x10`）。
- 若点位功能码选择 `0x06`，应限制为单个 16 位寄存器写入；若选择 `0x10`，应允许 16/32 位寄存器写入。
- 建议把“采集点 tag”和“控制点 tag”分开配置；控制命令通过 DataCenter 路由写入 ModbusRTU 的控制点 tag，不需要额外调用写寄存器 gRPC。
- 建议将 `transport_type` 暴露为显式选项，便于本地串口和远端串口透传共存配置。
- 对接调试时优先查看 ModbusRTU 日志中的 JSON payload 与 RTU 十六进制报文，快速定位是 MQTT 通道问题还是设备响应问题。

## 报文日志
- 串口直连与 MQTT UART 两条路径都输出收发报文日志。
- MQTT UART 会同时记录：
  - 请求/响应 Topic
  - JSON payload
  - Base64 解码后的 RTU 十六进制报文
- 生产环境需关注日志量与磁盘占用。
