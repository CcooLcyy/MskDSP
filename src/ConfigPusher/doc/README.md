# ConfigPusher 模块

## 简介
ConfigPusher 读取 JSONC 配置文件，自动启动 DataCenter/IEC104，并按配置调用 IEC104 gRPC 接口完成连接与点表下发。

## 能力清单
- 自动通过 ModuleManager 启动 DataCenter 与 IEC104
- 解析 JSONC（支持 `//` 与 `/* */` 注释）
- 下发 IEC104 配置：UpsertLink / UpsertPointTable / StartLink
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

## 配置与数据
- 配置文件：`./conf/configPusher/iec104.jsonc`
- 使用 Protobuf JSON 映射：枚举需写全名（例如 `ROLE_SERVER`、`TELEMETRY_TYPE_FLOAT`）
- `point_table.conn_name` 可省略（默认使用 `link.config.conn_name`）

示例：
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
            "apci": { "k": 12, "w": 8, "t0": 30, "t1": 15, "t2": 10, "t3": 20 }
          }
        },
        "point_table": {
          "replace": true,
          "points": [
            { "tag": "Ua", "ioa": 100, "type": "TELEMETRY_TYPE_FLOAT" }
          ]
        },
        "start": true
      }
    ]
  }
}
```

## 构建产物
- 共享库：`package/lib/libConfigPusher.so.<version>`（版本见 `src/ConfigPusher/cmake/LibInfo.cmake`）
