# COMMock 模块

## 简介
COMMock 用于通过 socat 创建成对互通的虚拟串口，并按配置生成 dev_path 软链，方便在无真实串口环境下进行 Modbus/串口测试。

## 能力清单
- 通过 ConfigPusher 下发配置并全量替换虚拟串口互通对
- dev_path 无权限或冲突时返回错误并记录日志
- 模块卸载时停止 socat 进程并清理软链

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
    - `name`：端口名称（用于日志与配对）
    - `dev_path`：希望创建的设备路径（软链到 `/dev/pts/<N>`）
    - `peer_name`：配对端口的 `name`（必须双向互指）
- 行为说明：ConfigPusher 仅在 COMMock 模块已启动时下发配置；每次下发为全量替换；不支持单端口模式
- 运行依赖：需要系统可执行 `socat`

示例：
```jsonc
{
  // COMMock 配置示例
  "com_mock": {
    "ports": [
      {
        "name": "com-1",
        "dev_path": "/dev/COMMock0",
        "peer_name": "com-2"
      },
      {
        "name": "com-2",
        "dev_path": "/dev/COMMock1",
        "peer_name": "com-1"
      }
    ]
  }
}
```

## 线程与日志
- 模块内部线程统一使用 `ModuleManager::StartModuleThread(模块LibInfo.LIB_NAME, ...)` 创建，自动绑定日志模块名上下文。
- 无需在入口手动创建 `ModuleManager::LogModuleScope`，统一规则见 `src/core/ModuleManager/doc/README.md`。

## 构建产物
- 共享库：`package/module/libCOMMock.so.<version>`（版本见 `src/COMMock/cmake/LibInfo.cmake`）
