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

## 未实现/后续计划
- 协议类型：目前仅支持遥测 `M_ME_NC_1` 与总召 `C_IC_NA_1`；遥信/遥控/设点、对时 `C_CS_NA_1`、带时标类型等未实现
- 时标：暂不支持 `CP56Time2a`（后续可扩展到带时标遥测/遥信与对时）
- 链路层完善：当前仅实现简化的 I/S/U 帧处理；`k/w` 窗口、未确认队列、`t0/t1/t2` 超时与重传策略未实现（现仅使用 `t3` 做心跳）
- 报文打包：当前遥测按“单点一帧”发送，未做批量打包/VSQ 序列优化
- 多主站/多会话：Server 模式当前同一 `conn_name` 只保留一个活动连接；多主站并发、会话级隔离策略未实现
- 点表扩展：点表已预留 `scale/offset/type`，但当前不生效；后续可扩展工程量换算、更多类型与双向映射校验
- 配置持久化：连接配置/点表/运行态信息未持久化；后续建议落盘到 `./conf/IEC104/` 并采用 tmp/bak/corrupt 策略
- 观测性：缺少链路状态统计（收发计数、最近一次总召、重连次数等）与更细粒度日志
- 安全：gRPC 与 IEC104 TCP 目前均为明文/无鉴权；TLS、鉴权、白名单等未实现
- 测试：当前以单元测试覆盖点表与 LinkManager 语义；缺少端到端“主站↔从站”协议互操作测试

## grpc接口
### 删除语义（最佳实践）
上位机删除一条连接时，调用 `DeleteLink(conn_name)`：
- IEC104 会先停止该连接，再调用 DataCenter `DeleteConnection(module_name="IEC104", conn_name)` 清理 `conn_id` 相关配置/缓存
- 若 DataCenter 删除失败（例如 `UNAVAILABLE/INTERNAL`），IEC104 会保留本地配置并标记为 `PENDING_DELETE`，上位机应提示用户重试
- DataCenter 返回 `NOT_FOUND` 时视为“已删除”，`DeleteLink` 幂等返回 OK

## 构建产物
- 共享库：`package/lib/libIEC104.so.<version>`（版本见 `src/IEC104/include/IEC104LibInfo.h`）
