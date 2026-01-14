# IEC104 模块

## 简介
IEC104 协议模块，提供 IEC 60870-5-104 的 TCP Server/Client 能力，并通过 DataCenter 完成“点值转发/路由”的闭环。

当前实现聚焦：**遥测（M_ME_NC_1，短浮点）**；时标（CP56Time2a）、遥信/遥控等后续按需扩展。

## 能力清单
- 角色：Server / Client（通过上位机 gRPC 配置）
- 连接名：`conn_name` 由上位机指定（模块内唯一，用于人类识别/配置归属）
- `conn_id` 分配：IEC104 在配置连接时通过 DataCenter `GetOrCreateConnection` 取/建，并回传给上位机
- 点表下发：上位机通过 IEC104 gRPC 下发 `tag <-> IOA` 映射（先支持 float 遥测）
- 与 DataCenter 联动：
  - Client 收到遥测后 `Publish(conn_id, tag, value)` 到 DataCenter
  - Server 订阅 DataCenter `Subscribe(conn_id)`，将点值转为 IEC104 遥测自发上送
  - Server 支持总召 `C_IC_NA_1`：通过 DataCenter `GetLatest(conn_id)` 拼装快照应答

## 接口与协议
- Protobuf：`protobuf/IEC104.proto`
- gRPC Service：`IEC104Proto::IEC104Service`

## 运行与地址
- 对外 gRPC：随机选择 `0.0.0.0:<port>`（7001–7999）
- 内部 gRPC：`unix socket`：`./socket/IEC104.sock`
- 运行时可通过管理器 `GetRunningModuleInfo` 查询实际地址

## 配置与数据
### 关键概念
- `module_name`：固定为 `"IEC104"`（来自 `src/IEC104/include/IEC104LibInfo.h`），用于 DataCenter 的连接主键
- `conn_name`：上位机指定的“代称”，可为任意字符串；要求在 IEC104 模块内唯一
- `conn_id`：DataCenter 分配的连接 ID（稳定且可持久化），上位机后续用它在 DataCenter 配路由/订阅

### 上位机推荐流程
1. 通过 ModuleManager 启动 `DataCenter` 与 `IEC104`，并用 `GetRunningModuleInfo` 获取 IEC104 的 `outer_grpc_server`
2. 连接 IEC104 gRPC，调用 `UpsertLink(create_only=true)` 配置连接并获取 `conn_id`
3. 调用 `UpsertPointTable(replace=true)` 下发点表（`tag <-> IOA`）
4. 上位机使用返回的 `conn_id` 调用 DataCenter 配置路由（`UpsertRoutes` 等）
5. 调用 `StartLink` 启动该连接的 TCP 监听/连接

当前实现说明：
- 连接配置/点表暂未做落盘持久化；进程重启后上位机需重新下发。

### 删除语义（最佳实践）
上位机删除一条连接时，调用 `DeleteLink(conn_name)`：
- IEC104 会先停止该连接，再调用 DataCenter `DeleteConnection(module_name="IEC104", conn_name)` 清理 `conn_id` 相关配置/缓存
- 若 DataCenter 删除失败（例如 `UNAVAILABLE/INTERNAL`），IEC104 会保留本地配置并标记为 `PENDING_DELETE`，上位机应提示用户重试
- DataCenter 返回 `NOT_FOUND` 时视为“已删除”，`DeleteLink` 幂等返回 OK

## 构建产物
- 共享库：`package/lib/libIEC104.so.<version>`（版本见 `src/IEC104/include/IEC104LibInfo.h`）
