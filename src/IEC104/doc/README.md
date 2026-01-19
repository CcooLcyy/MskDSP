# IEC104 模块

## 简介
IEC104 协议模块，提供 IEC 60870-5-104 的 TCP Server/Client 能力，并通过 DataCenter 完成“点值转发/路由”的闭环。

当前实现聚焦：**遥测（M_ME_NC_1，短浮点）**；时标（CP56Time2a）、遥信/遥控等后续按需扩展。

## 能力清单
- 角色：Server / Client（通过上位机 gRPC 配置）
- 连接名：`conn_name` 由上位机指定（模块内唯一，用于人类识别/配置归属）
- `conn_id` 分配：IEC104 在配置连接时通过 DataCenter `GetOrCreateConnection` 取/建，并回传给上位机
- 点表下发：上位机通过 IEC104 gRPC 下发 `tag <-> IOA` 映射（先支持 float 遥测，支持 scale/offset/deadband）
- 链路层：支持 `k/w` 窗口与 `t0/t1/t2/t3` 超时（通过 `LinkConfig.apci` 配置）
- 与 DataCenter 联动：
  - Client 收到遥测后 `Publish(conn_id, tag, value)` 到 DataCenter
  - Server 订阅 DataCenter `Subscribe(conn_id)`，将点值转为 IEC104 遥测自发上送
  - Server 支持总召 `C_IC_NA_1`：通过 DataCenter `GetLatest(conn_id)` 拼装快照应答
  - Client 在 STARTDT 成功后自动发起总召 `C_IC_NA_1(QOI=20)`
- 遥测合包：自发遥测支持窗口合包与 IOA 顺序打包，连续 IOA 使用 SQ=1 压缩；总召快照按帧大小批量打包

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

### 链路层参数（APCI）
- `k/w`：I 帧发送/接收窗口
- `t0`：等待 STARTDT 建链超时（秒）
- `t1`：I 帧确认等待超时（秒）
- `t2`：延迟确认超时（秒，触发 S 帧）
- `t3`：空闲保活超时（秒，触发 TESTFR）
- 参数由上位机在 `LinkConfig.apci` 下发，默认值：`k=12, w=8, t0=30, t1=15, t2=10, t3=20`

### 点表字段（遥测）
- `scale/offset`：工程量换算 `value = raw * scale + offset`（`scale=0` 视为 1）。
- `deadband`：工程量单位；`|value - last_reported| < deadband` 时不上报，<=0 表示不过滤。
- `deadband` 同时作用于 Client 发布与 Server 自发上送，总召快照不受 deadband 影响。
- Client 收到遥测后按 `scale/offset` 转为工程量再发布；Server 上送遥测时按 `scale/offset` 反向换算。

ConfigPusher 点表示例（含 scale/offset/deadband，字段可省略，默认 scale=1、offset=0、deadband=0）：
```jsonc
{
  "tag": "1-A相电压",
  "ioa": 16385,
  "type": "TELEMETRY_TYPE_FLOAT",
  "scale": 1.0,
  "offset": 0.0,
  "deadband": 0.0
}
```

### 遥测合包参数
- `telemetry_batch_window_ms`：自发遥测合包窗口（毫秒）；0 表示使用默认值（20ms）。
- `telemetry_max_asdu_bytes`：遥测 ASDU 单帧最大字节数（<=249）；0 表示默认值（249）。
- `telemetry_use_standard_limit`：true 时强制使用标准上限 249 字节，忽略 `telemetry_max_asdu_bytes`。
- `telemetry_dedupe`：自发遥测合包去重（按 IOA 保留最新值），默认开启。
- 合包策略：按 IOA 顺序组织报文；连续 IOA 使用 SQ=1 压缩，不连续则 SQ=0 带 IOA。

### 上位机推荐流程
1. 通过 ModuleManager 启动 `DataCenter` 与 `IEC104`，并用 `GetRunningModuleInfo` 获取 IEC104 的 `outer_grpc_server`
2. 连接 IEC104 gRPC，调用 `UpsertLink(create_only=true)` 配置连接并获取 `conn_id`（ROLE_SERVER 会在配置阶段检查 `local.ip/local.port`：本模块内冲突返回 `ALREADY_EXISTS`；端口被系统占用返回 `FAILED_PRECONDITION`）
3. 调用 `UpsertPointTable(replace=true)` 下发点表（`tag <-> IOA`）
4. 上位机使用返回的 `conn_id` 调用 DataCenter 配置路由（`UpsertRoutes` 等）
5. 调用 `StartLink` 启动该连接的 TCP 监听/连接

## 日志
- 日志前缀包含模块名 `[IEC104]`，便于与其他模块混合排查。
- 报文日志为 INFO 级别，逐帧输出完整 APDU（包含 `conn_name/角色/长度/数据`）。
- 报文日志输出量大，生产环境需关注日志大小与磁盘占用。

## 测试
相关单元测试位于 `test/`：

- `iec104PointTable_test`：覆盖点表更新、双向查询、冲突校验与序列化输出稳定性。
- `iec104LinkManager_test`：覆盖 LinkManager 与 DataCenter 的交互语义（使用 gMock stub），以及配置校验、点表下发合并、删除语义等边界。
- `iec104TcpSession_test`：覆盖链路层 STARTDT 握手、自动总召与 t2 延迟确认行为。

运行方式：
```bash
ctest --test-dir build -R iec104PointTable_test --output-on-failure
ctest --test-dir build -R iec104LinkManager_test --output-on-failure
ctest --test-dir build -R iec104TcpSession_test --output-on-failure
```

## 未实现/后续计划
- 协议类型：目前仅支持遥测 `M_ME_NC_1` 与总召 `C_IC_NA_1`；遥信/遥控/设点、对时 `C_CS_NA_1`、带时标类型等未实现
- 时标：暂不支持 `CP56Time2a`（后续可扩展到带时标遥测/遥信与对时）
- 链路层完善：已支持 `k/w` 与 `t0–t3`；未实现 I 帧重传策略与更细粒度链路统计
- 报文打包：当前遥测按“单点一帧”发送，未做批量打包/VSQ 序列优化
- 多主站/多会话：Server 模式当前同一 `conn_name` 只保留一个活动连接；多主站并发、会话级隔离策略未实现
- 点表扩展：当前仅支持 float 遥测；后续可扩展更多类型与双向映射校验
- 配置持久化：连接配置/点表/运行态信息未持久化；后续建议落盘到 `./conf/IEC104/` 并采用 tmp/bak/corrupt 策略
- 观测性：已补充逐帧日志，但仍缺少链路状态统计（收发计数、最近一次总召、重连次数等）与可配置采样/汇总
- 安全：gRPC 与 IEC104 TCP 目前均为明文/无鉴权；TLS、鉴权、白名单等未实现
- 测试：当前以单元测试覆盖点表与 LinkManager 语义；缺少端到端“主站↔从站”协议互操作测试
- SOE/COS：遥信变位/事件相关语义与时序处理尚未实现；后续需独立队列与不去重策略

## grpc接口
### 删除语义（最佳实践）
上位机删除一条连接时，调用 `DeleteLink(conn_name)`：
- IEC104 会先停止该连接，再调用 DataCenter `DeleteConnection(module_name="IEC104", conn_name)` 清理 `conn_id` 相关配置/缓存
- 若 DataCenter 删除失败（例如 `UNAVAILABLE/INTERNAL`），IEC104 会保留本地配置并标记为 `PENDING_DELETE`，上位机应提示用户重试
- DataCenter 返回 `NOT_FOUND` 时视为“已删除”，`DeleteLink` 幂等返回 OK

## 构建产物
- 共享库：`package/lib/libIEC104.so.<version>`（版本见 `src/IEC104/include/IEC104LibInfo.h`）
