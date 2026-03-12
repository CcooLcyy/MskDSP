# ModbusRTU 模块

## 简介
ModbusRTU 模块负责管理 ModbusRTU 链路与点表，当前同时支持两种传输方式：
- `TRANSPORT_SERIAL`：本地串口直连，保留原有直接操作串口能力。
- `TRANSPORT_MQTT_UART`：通过 `MQTTManager + uartManager` 透传原始 RTU 报文。

控制面统一由 ConfigPusher/上位机通过 gRPC 下发；数据面通过 DataCenter 发布与读取点值。

## 能力清单
- 主站读取线圈（0x01）与保持寄存器（0x03）。
- 从站响应线圈（0x01）与保持寄存器（0x03）。
- 同一物理串口允许多个从站共享，前提是串口参数一致且模式不混用。
- 支持显式区间抄读 `read_plan`。
- 支持点表 `scale/offset/deadband` 工程量换算。
- 支持通过 MQTT UART 透传原始 RTU 帧，并输出完整收发报文日志。

## 接口与运行
- Protobuf：`protobuf/ModbusRTU.proto`
- gRPC Service：`ModbusRTUProto::ModbusRTUService`
- 对外 gRPC：随机 `0.0.0.0:<port>`（7001-7999）
- 内部 gRPC：`./socket/ModbusRTU.sock`

## 配置来源
- 运行时配置统一由 ConfigPusher 通过 gRPC 下发。
- 示例文件：`package/conf/configPusher/modbus_rtu.jsonc`
- 当存在 `TRANSPORT_MQTT_UART` 链路时，ConfigPusher 会先调用 `UpdateConfig` 下发顶层 `modbus_rtu.mqtt`，再继续下发链路/点表并启动连接功能。

## 传输模式

### 1. 本地串口直连
- `transport_type = TRANSPORT_SERIAL`
- 使用 `serial.device` 打开本地串口。
- 支持主站与从站模式。
- 继续沿用原有 `read_timeout_ms`、串口共享与从站响应逻辑。

### 2. MQTT 串口透传
- `transport_type = TRANSPORT_MQTT_UART`
- `serial_port` 表示远端 `uartManager` 的串口标识，例如 `RS485-1`。
- `serial` 中只保留远端串口参数：`baud_rate/data_bits/parity/stop_bits`。
- `serial.device` 在该模式下忽略。
- 当前仅支持主站模式；若配置为从站会返回 `UNIMPLEMENTED`。

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
- `slave_id`：目标从站地址，范围 `1..247`。
- `poll_interval_ms`：主站轮询周期。
- `address_base`：地址基准，支持 `ZERO/ONE`。
- `mode`：`LINK_MODE_MASTER` 或 `LINK_MODE_SLAVE`。
- `read_plan`：主站显式区间抄读配置。
- `serial_port`：MQTT UART 场景必填。
- `request_timeout_ms`：MQTT UART 单次请求超时，默认 `3000ms`。
- `serial_byte_timeout_ms`：对应 `uartManager.byteTimeout`，默认 `100ms`。
- `serial_frame_timeout_ms`：对应 `uartManager.frameTimeout`，默认 `100ms`。
- `serial_est_size`：对应 `uartManager.estSize`，默认 `256`。

## 点表说明
- `FUNCTION_READ_COILS` 仅支持 `DATA_TYPE_BOOL`。
- `FUNCTION_READ_HOLDING_REGISTERS` 支持 `DATA_TYPE_UINT16/DATA_TYPE_UINT32`。
- `scale/offset/deadband` 仅对 `UINT16/UINT32` 生效，BOOL 忽略。
- `scale=0` 视为 `1`，计算公式为 `value = raw * scale + offset`。
- `deadband <= 0` 表示不过滤。
- 从站模式优先读取 DataCenter 最新值；若无值则回退到点表默认值。

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
完整示例见 `package/conf/configPusher/modbus_rtu.jsonc`，其中同时包含：
- 一个 `TRANSPORT_SERIAL` 本地串口主站示例。
- 一个 `TRANSPORT_MQTT_UART` 远端串口主站示例。

## 上位机对接建议
- 配置 MQTT UART 链路时，界面上应同时展示顶层 `mqtt` 与链路级 `serial_port/serial.*` 参数。
- 启停文案建议明确为“启动连接功能/停止连接功能”，避免与“启动模块”混淆。
- 若链路使用 `TRANSPORT_MQTT_UART`，应在校验阶段提示“当前仅支持主站模式”。
- 建议将 `transport_type` 暴露为显式选项，便于本地串口和远端串口透传共存配置。
- 对接调试时优先查看 ModbusRTU 日志中的 JSON payload 与 RTU 十六进制报文，快速定位是 MQTT 通道问题还是设备响应问题。

## 报文日志
- 串口直连与 MQTT UART 两条路径都输出收发报文日志。
- MQTT UART 会同时记录：
  - 请求/响应 Topic
  - JSON payload
  - Base64 解码后的 RTU 十六进制报文
- 生产环境需关注日志量与磁盘占用。
