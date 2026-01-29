# DLT645 模块

## 简介
TODO：一句话说明模块职责/边界。

## 能力清单
- TODO

## 接口与协议
- Protobuf：`protobuf/DLT645.proto`
- gRPC Service：`DLT645Proto::DLT645Service`

## 运行与地址
- 对外 gRPC：随机选择 `0.0.0.0:<port>`（7001–7999）
- 内部 gRPC：`unix socket`：`./socket/DLT645.sock`
- 运行时可通过管理器 `GetRunningModuleInfo` 查询实际地址

## 配置与数据
- TODO：运行时配置项、文件位置、持久化数据目录等

## 线程与日志
- 模块内部线程统一使用 `ModuleManager::StartModuleThread(模块LibInfo.LIB_NAME, ...)` 创建，自动绑定日志模块名上下文。
- 无需在入口手动创建 `ModuleManager::LogModuleScope`，统一规则见 `src/core/ModuleManager/doc/README.md`。

## 构建产物
- 共享库：`package/module/libDLT645.so.<version>`（版本见 `src/DLT645/include/DLT645LibInfo.h`）
