# MQTTManager 模块

## 简介
MQTTManager 提供通用 MQTT 连接与请求-响应能力，业务模块通过 gRPC 调用完成发布、订阅与请求-响应交互。业务 payload 透传，不在模块内解析。

## 能力清单
- 单实例多连接（每个连接一个线程）
- 业务模块通过 gRPC 直接提供 broker 连接参数（host/port/client_id 等）
- 请求-响应：payload 原样透传，按 JSON 字段匹配响应
- 普通发布与订阅：payload 原样透传

## 接口与协议
- Protobuf：`protobuf/MQTTManager.proto`
- gRPC Service：`MQTTManagerProto::MQTTManagerService`

## 运行与地址
- 对外 gRPC：随机选择 `0.0.0.0:<port>`（17001–17999）
- 内部 gRPC：`unix socket`：`./socket/MQTTManager.sock`
- 运行时可通过管理器 `GetRunningModuleInfo` 查询实际地址

## 连接与隔离规则
- 连接唯一标识：`host:port + client_id`
- 若同一 `host:port` 但 `client_id` 不同，将建立独立连接，便于多个模块同时接入同一 broker
- 若同一 `host:port + client_id` 下其余连接参数（`username/password/keepalive_sec/clean_session/connect_timeout_ms`）不一致，将返回错误（避免同名客户端配置冲突）
- 请求-响应通过 `match_field` 指定的 JSON 字段值做关联；同一 `response_topic + match_field + match_value` 只允许一个请求在途

## 连接复用与并发行为
- 同一 `host:port + client_id` 连接在模块内复用；并发请求触发重复创建时会复用已有连接（日志会提示“连接已存在，丢弃重复创建”）。
- 发布/订阅/重订阅在单连接内做串行化，避免并发操作导致 MQTT 客户端阻塞或超时。
- 连接断开时可能出现短暂不可用；建议业务侧使用 QoS 1 并配合持久会话（`clean_session=false`）降低丢消息概率。

## 重连后订阅恢复说明
### 问题现象
- 业务侧已成功订阅响应主题后，若 MQTT 连接中途断开，再由后续 `publish` 路径触发重连，现场可能出现“请求仍能继续发送，但响应主题再也收不到消息”的现象。
- 在 DLT645 LoRa 超时排查中，可观察到 `RequestAndWait` 持续发起请求，但 `RTU.log` 中不再出现 `MQTTManager 收到消息: 主题=loraManager/AGVC/JSON/action/response/monitorNode`。
- 该现象同样会影响 ModbusRTU 等依赖 `RequestAndWait` 或预先订阅响应主题的业务模块，表现通常为请求超时、重试增多或链路看似在线但响应长期缺失。

### 根因分析
- `RequestAndWait` 会先订阅 `response_topic`，再发布请求消息；首次调用时 broker 端订阅通常可以建立成功。
- MQTTManager 内部维护了本地 `subscriptions` 缓存；若同一主题已记录在缓存中，后续 `subscribe()` 会直接返回，不会再次向 broker 发起订阅。
- `publish()` 路径在发现连接断开后会通过 `ensureConnected()` 触发重连，但旧实现不会在该路径补发 `resubscribeAll()`。
- `resubscribeAll()` 旧实现只在 `consumeLoop()` 捕获异常后才会触发；若底层 `consume_message()` 在断链时返回空消息而不是抛异常，消费线程会静默空转，绕过重连后的订阅恢复。

### clean_session=true 的风险
- 当 `clean_session=true` 时，broker 在客户端断线并重新建立会话后不会保留旧订阅。
- 若模块仅凭本地 `subscriptions` 缓存判断“已经订阅过”，就会出现“代码认为已订阅，但 broker 实际未订阅”的状态偏差。
- 该状态下请求主题仍可能继续发布成功，但响应主题因为 broker 端没有订阅而无法收到消息，最终表现为请求超时。

### 当前修复策略
- MQTTManager 在检测到连接刚刚建立或刚刚重连成功，且本地缓存中已有订阅主题时，会主动执行重订阅，而不是只在消费线程异常分支中补救。
- `publish()` 路径触发重连后，会先恢复订阅，再继续执行发布，避免请求先发出而响应主题尚未恢复订阅。
- `consumeLoop()` 在收到空消息且检测到底层连接已断开时，不再只做休眠，而是进入断线恢复链路，执行重连与重订阅。
- 恢复链路会输出中文日志，明确记录“检测断链、触发重连、开始重订阅、重订阅结果”，便于后续定位现场问题。

### 对业务侧的影响
- DLT645、ModbusRTU 等通过 MQTTManager 发起请求-响应的模块，无需再依赖“重新下发配置”或“人工重复订阅”来恢复响应主题订阅。
- 业务侧仍建议根据现场可靠性选择合适的 QoS，并优先评估 `clean_session=false` 的持久会话能力，以进一步降低断线窗口内的消息丢失概率。
- 若 broker 不可达、鉴权失败或重订阅本身失败，业务侧仍可能看到请求超时；此时应优先检查 `MQTTManager.log` 中的重连、重订阅相关日志。

## 请求-响应模型
1) 调用 `RequestAndWait`，指定 `match_field`（JSON 字段路径）  
2) MQTTManager 将业务 payload 原样发布到请求 topic  
3) MQTTManager 订阅响应 topic，解析响应 JSON 并提取 `match_field`  
4) 当响应字段值与请求字段值一致时返回 payload

注意：
- payload 必须是 JSON 文本，`match_field` 必须在请求与响应中同时存在。
- `match_field` 支持嵌套路径（如 `data.id`、`data.items[0].id`）。
- 建议使用字符串/数值字段作为匹配值，避免复杂对象造成歧义。
- 匹配规则为 JSON 值全等（类型和值一致），例如 `"1"` 与 `1` 不相等。
- `request_id` 仅用于 gRPC 返回与日志，不参与 MQTT 匹配。

### 匹配字段路径说明
- `.` 表示对象字段，`[index]` 表示数组下标（从 0 开始），支持根数组（如 `[0].id`）。
- 字段名不支持包含 `.` 或 `[]` 字符；如需复杂字段名，请在业务层自行规避或映射。
- 同一 `response_topic + match_field + match_value` 只允许一个请求在途，后续请求会返回 `ALREADY_EXISTS`。

### 响应方示例
响应方收到请求后，需在响应 JSON 中带回相同字段值：
```
{
  "req": { "id": "req-1700000000000-1" },
  "result": "ok",
  "payload": { ...业务数据... }
}
```

## gRPC 调用示例
> 以下为示例，地址需以 ModuleManager 实际返回为准。

1) 请求-响应（RequestAndWait）
```bash
grpcurl -plaintext 127.0.0.1:7xxx \
  MQTTManagerProto.MQTTManagerService/RequestAndWait \
  -d '{
    "connection": {
      "host": "127.0.0.1",
      "port": 1883,
      "client_id": "modbus_client"
    },
    "request_topic": "uart/req",
    "response_topic": "uart/resp",
    "qos": 1,
    "retain": false,
    "timeout_ms": 3000,
    "retry_times": 1,
    "match_field": "req.id",
    "payload": "eyJyZXEiOnsiaWQiOiJyZXEtMTcwMDAwMDAwMDAwMC0xIn0sImNtZCI6IndyaXRlIiwiaGV4IjoiMDEwMzAwMDAwMDAyQzQwQiJ9"
  }'
```

2) 发布消息（Publish）
```bash
grpcurl -plaintext 127.0.0.1:7xxx \
  MQTTManagerProto.MQTTManagerService/Publish \
  -d '{
    "connection": {
      "host": "127.0.0.1",
      "port": 1883,
      "client_id": "modbus_client"
    },
    "topic": "demo/topic",
    "qos": 1,
    "retain": false,
    "payload": "aGVsbG8="
  }'
```

3) 订阅消息（Subscribe，服务端流）
```bash
grpcurl -plaintext 127.0.0.1:7xxx \
  MQTTManagerProto.MQTTManagerService/Subscribe \
  -d '{
    "connection": {
      "host": "127.0.0.1",
      "port": 1883,
      "client_id": "modbus_client"
    },
    "topics": [
      { "topic": "demo/#", "qos": 1 }
    ]
  }'
```

## 线程与日志
- 每个连接对应一个内部线程，负责消费 MQTT 消息。
- 所有收发消息内容都会记录日志（中文）。

## 构建产物
- 共享库：`package/module/libMQTTManager.so.<version>`（版本见 `src/MQTTManager/cmake/LibInfo.cmake`）
