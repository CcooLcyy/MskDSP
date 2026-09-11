# ModbusTCP 模块方案

## 目标与边界

`ModbusTCP` 是独立的 Modbus TCP 主站模块，负责通过 TCP 连接轮询设备并执行写点命令。模块依赖 `DataCenter`，不依赖 `MQTTManager`，不修改 `ModbusRTU` 的串口和 MQTT UART 行为。

首期对齐现有 ModbusRTU 主站能力：读取 0x01、0x03、0x04，写入 0x05、0x06、0x10；点表数据通过 DataCenter 发布，写命令通过 DataCenter 路由进入模块。

## 配置与协议

- 每条链路配置 `host`、`port`、`unit_id`、连接超时、请求超时和轮询周期。
- `unit_id` 范围为 1..247，作为 Modbus TCP MBAP 头中的 Unit Identifier。
- TCP ADU 为 7 字节 MBAP 头加 Modbus PDU：事务标识、协议标识（必须为 0）、长度、Unit Identifier；不带 RTU CRC。
- 每次请求递增事务标识，并严格校验响应事务标识、协议标识、长度、Unit Identifier、功能码和异常响应。

ConfigPusher 使用 `modbus_tcp.jsonc` 下发目标态，字段采用 Protobuf JSON 命名（例如 `connName`、`unitId`）。每条链路至少配置 `tcp.host`；端口默认为 502，Unit ID 默认为 1。

## 生命周期

配置、点表和链路状态通过独立 gRPC 服务管理，链路配置持久化到 SQLite 的 `ModbusTCP/links` 与 `ModbusTCP/point_tables`。`StartLink` 建立 TCP 连接并启动轮询/写命令，`StopLink` 关闭连接并停止线程；连接和请求使用配置的超时，断线或请求失败记录中文错误日志，下一次请求会自动重新建立连接。

## 验收标准

1. CMake 能生成 `libModbusTCP.so`，ModuleManager 能发现并加载模块。
2. TCP 总线能正确生成 MBAP+PDU 请求，拒绝错误事务标识、协议标识、长度、Unit ID 和异常响应。
3. 读写功能码与现有点表语义一致，收到设备响应后向 DataCenter 发布值。
4. ConfigPusher 可从 `modbus_tcp.jsonc` 下发链路和点表。
5. 单元测试覆盖 MBAP 编解码、异常响应和 TCP 配置校验。
