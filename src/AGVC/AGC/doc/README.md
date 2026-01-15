# AGC 模块

## 简介
TODO：一句话说明模块职责/边界。

## 能力清单
- TODO

## 接口与协议
- Protobuf：`protobuf/AGC.proto`
- gRPC Service：`AGCProto::AGCService`

## 运行与地址
- 对外 gRPC：随机选择 `0.0.0.0:<port>`（7001–7999）
- 内部 gRPC：`unix socket`：`./socket/AGC.sock`
- 运行时可通过管理器 `GetRunningModuleInfo` 查询实际地址

## 配置与数据
- TODO：运行时配置项、文件位置、持久化数据目录等

## 构建产物
- 共享库：`package/lib/libAGC.so.<version>`（版本见 `src/AGVC/AGC/cmake/LibInfo.cmake`）
