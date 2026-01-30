# MQTTManager 模块

## 简介
MQTTManager 提供通用 MQTT 连接与请求-响应能力，业务模块通过 gRPC 调用完成发布、订阅与请求-响应交互。业务 payload 透传，不在模块内解析。

## 能力清单
- 单实例多连接（每个连接一个线程）
- 业务模块通过 gRPC 直接提供 broker 连接参数（host/port）
- 请求-响应：payload 原样透传，按 JSON 字段匹配响应
- 普通发布与订阅：payload 原样透传

## 接口与协议
- Protobuf：`protobuf/MQTTManager.proto`
- gRPC Service：`MQTTManagerProto::MQTTManagerService`

## 运行与地址
- 对外 gRPC：随机选择 `0.0.0.0:<port>`（7001–7999）
- 内部 gRPC：`unix socket`：`./socket/MQTTManager.sock`
- 运行时可通过管理器 `GetRunningModuleInfo` 查询实际地址

## 连接与隔离规则
- 连接唯一标识：`host:port`
- 若同一 `host:port` 连接参数不一致，将返回错误（避免连接冲突）
- 请求-响应通过 `match_field` 指定的 JSON 字段值做关联；同一 `response_topic + match_field + match_value` 只允许一个请求在途

## 连接复用与并发行为
- 同一 `host:port` 连接在模块内复用；并发请求触发重复创建时会复用已有连接（日志会提示“连接已存在，丢弃重复创建”）。
- 发布/订阅/重订阅在单连接内做串行化，避免并发操作导致 MQTT 客户端阻塞或超时。
- 连接断开时可能出现短暂不可用；建议业务侧使用 QoS 1 并配合持久会话（`clean_session=false`）降低丢消息概率。

## 断线与恢复建议
- 断线后，如使用 `clean_session=true`，broker 不保留订阅；需要业务侧重新触发订阅（或重启/重配）。
- 若发现请求偶发超时，请优先检查 `MQTTManager.log` 中的 `Disconnected`、`订阅失败/发布失败` 日志。

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
