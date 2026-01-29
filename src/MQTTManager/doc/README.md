# MQTTManager 模块

## 简介
MQTTManager 负责管理 MQTT 连接与脚本编解码，通过 gRPC 提供发布/订阅与配置下发能力。

## 能力清单
- 通过 gRPC 下发多配置项（连接参数 + Lua 脚本）
- 业务模块通过 gRPC 发布/订阅 MQTT 消息（脚本负责编解码）
- 支持内联脚本与脚本文件路径（内联优先）

## 接口与协议
- Protobuf：`protobuf/MQTTManager.proto`
- gRPC Service：`MQTTManagerProto::MQTTManagerService`

## 运行与地址
- 对外 gRPC：随机选择 `0.0.0.0:<port>`（7001–7999）
- 内部 gRPC：`unix socket`：`./socket/MQTTManager.sock`
- 运行时可通过管理器 `GetRunningModuleInfo` 查询实际地址

## 配置与数据
- 配置来源：由 ConfigPusher 通过 gRPC `UpdateConfig` 统一下发（上位机侧请通过 ConfigPusher 触发下发）
- 脚本路径：`<可执行程序同目录>/conf/MQTTManager/script`（文件路径需相对该目录）
- 示例配置：`package/conf/configPusher/MQTTManager.jsonc`（仅示例）
- 脚本环境：当前仅启用基础库与字符串库

## 使用流程
1) 启动 MQTTManager 模块
2) 通过 ModuleManager 查询 MQTTManager 的 gRPC 地址
3) 通过 ConfigPusher 下发配置（ConfigPusher 调用 `UpdateConfig`，含多配置项与脚本）
4) 业务模块调用 `Publish`（仅发送/广播）或 `Subscribe`（仅接收/广播）

说明：当前 `Publish/Subscribe` 尚未启用，调用会返回 `UNIMPLEMENTED`，仅用于接口对接与流程验证。

## 脚本接口约定
- 解码函数：`decode(topic, payload, props)`
- 编码函数：`encode(topic, data, props)`
- `data` 为 `map<string, string>`，仅支持字符串字段

示例：
```lua
function decode(topic, payload, props)
  return { data = { topic = topic, payload = payload }, meta = {} }
end

function encode(topic, data, props)
  return data["payload"] or ""
end
```

## gRPC 调用示例
> 以下为示例，地址需以 ModuleManager 实际返回为准。

1) 下发配置（UpdateConfig，主要供 ConfigPusher 调用）
```bash
grpcurl -plaintext 127.0.0.1:7xxx \
  MQTTManagerProto.MQTTManagerService/UpdateConfig \
  -d '{
    "profiles": [
      {
        "profile_id": "main",
        "connection": {
          "broker_uri": "tcp://127.0.0.1:1883",
          "client_id": "mqtt_manager_main",
          "username": "user",
          "password": "pass",
          "keepalive_sec": 30,
          "clean_session": true
        },
        "script": {
          "inline_script": "function decode(topic, payload, props)\\n  return { data = { topic = topic, payload = payload }, meta = {} }\\nend\\n\\nfunction encode(topic, data, props)\\n  return data[\\\"payload\\\"] or \\\"\\\"\\nend\\n",
          "decode_entry": "decode",
          "encode_entry": "encode"
        }
      }
    ]
  }'
```

2) 发布消息（Publish）
```bash
grpcurl -plaintext 127.0.0.1:7xxx \
  MQTTManagerProto.MQTTManagerService/Publish \
  -d '{
    "profile_id": "main",
    "topic": "demo/topic",
    "qos": 1,
    "retain": false,
    "data": {
      "payload": "hello"
    }
  }'
```

3) 订阅消息（Subscribe，服务端流）
```bash
grpcurl -plaintext 127.0.0.1:7xxx \
  MQTTManagerProto.MQTTManagerService/Subscribe \
  -d '{
    "profile_id": "main",
    "topics": [
      { "topic": "demo/#", "qos": 1 }
    ]
  }'
```

## 线程与日志
- 模块内部线程统一使用 `ModuleManager::StartModuleThread(模块LibInfo.LIB_NAME, ...)` 创建，自动绑定日志模块名上下文。
- 无需在入口手动创建 `ModuleManager::LogModuleScope`，统一规则见 `src/core/ModuleManager/doc/README.md`。

## 构建产物
- 共享库：`package/module/libMQTTManager.so.<version>`（版本见 `src/MQTTManager/cmake/LibInfo.cmake`）
