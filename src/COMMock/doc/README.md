# COMMock 模块

## 简介
COMMock 用于通过 PTY 创建虚拟串口，并按配置生成 dev_path 软链，方便在无真实串口环境下进行 Modbus/串口测试。

## 能力清单
- 通过 ConfigPusher 下发配置并全量替换虚拟串口
- dev_path 无权限或冲突时记录日志并跳过
- 模块卸载时清理软链并关闭句柄

## 接口与协议
- Protobuf：`protobuf/COMMock.proto`
- gRPC Service：`COMMockProto::COMMockService`

## 运行与地址
- 对外 gRPC：随机选择 `0.0.0.0:<port>`（7001–7999）
- 内部 gRPC：`unix socket`：`./socket/COMMock.sock`
- 运行时可通过管理器 `GetRunningModuleInfo` 查询实际地址

## 配置与数据
- 配置文件：`./conf/configPusher/COMMock.jsonc`（JSONC，支持 `//` 与 `/* */` 注释）
- 字段说明：
  - `com_mock.ports[]`
    - `name`：端口名称（用于日志）
    - `dev_path`：希望创建的设备路径（软链到 `/dev/pts/<N>`）
- 行为说明：ConfigPusher 仅在 COMMock 模块已启动时下发配置；每次下发为全量替换

示例：
```jsonc
{
  // COMMock 配置示例
  "com_mock": {
    "ports": [
      {
        "name": "com-1",
        "dev_path": "/dev/COMMock0"
      }
    ]
  }
}
```

## 构建产物
- 共享库：`package/lib/libCOMMock.so.<version>`（版本见 `src/COMMock/cmake/LibInfo.cmake`）
