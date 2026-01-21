# AVC 模块

## 简介
AVC（Automatic Voltage Control）自动电压控制模块（骨架/预留）：后续计划与 AGC 类似，通过 DataCenter 订阅/发布点值并基于策略计算派生点与控制设定点。

## 能力清单
- TODO

## 接口与协议
- Protobuf：`protobuf/AVC.proto`
- gRPC Service：`AVCProto::AVCService`

## 运行与地址
- 对外 gRPC：随机选择 `0.0.0.0:<port>`（7001–7999）
- 内部 gRPC：`unix socket`：`./socket/AVC.sock`
- 运行时可通过管理器 `GetRunningModuleInfo` 查询实际地址

## 配置与数据
- TODO：运行时配置项、文件位置、持久化数据目录等

## 构建产物
- 共享库：`package/module/libAVC.so.<version>`（版本见 `src/AGVC/AVC/cmake/LibInfo.cmake`）
