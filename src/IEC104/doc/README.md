# IEC104 模块

## 简介
TODO：一句话说明模块职责/边界。

## 能力清单
- TODO

## 接口与协议
- Protobuf：`protobuf/IEC104.proto`
- gRPC Service：`IEC104Proto::IEC104Service`

## 运行与地址
- 对外 gRPC：随机选择 `0.0.0.0:<port>`（7001–7999）
- 内部 gRPC：`unix socket`：`./socket/IEC104.sock`
- 运行时可通过管理器 `GetRunningModuleInfo` 查询实际地址

## 配置与数据
- TODO：运行时配置项、文件位置、持久化数据目录等

## 构建产物
- 共享库：`package/lib/libIEC104.so.<version>`（版本见 `src/IEC104/include/IEC104LibInfo.h`）
