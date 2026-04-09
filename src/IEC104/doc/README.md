# IEC104 模块

## 简介
IEC104 协议模块，提供 IEC 60870-5-104 的 TCP Server/Client 能力，并通过 DataCenter 完成“点值转发/路由”的闭环。

当前实现聚焦：**短浮点遥测（M_ME_NC_1/M_ME_TF_1）**、**单点遥信（M_SP_NA_1/M_SP_TB_1）**、**单点遥控（C_SC_NA_1，选择-执行）** 与 **短浮点设点（C_SE_NC_1）**。

## 能力清单
- 传输角色 `role`：Server / Client（决定 TCP 监听/连接）
- 站点角色 `station_role`：Master / Slave（决定业务语义）
- 启动帧：`STARTDT_ACT` 由主站发送，与 `role` 解耦；从站不主动发送
- 连接名：`conn_name` 由上位机指定（模块内唯一，用于人类识别/配置归属）
- `conn_id` 分配：IEC104 在配置连接时通过 DataCenter `GetOrCreateConnection` 取/建，并回传给上位机
- 点表下发：上位机通过 IEC104 gRPC 下发 `tag <-> IOA` 映射（支持短浮点与单点遥信；BOOL 忽略 scale/offset/deadband）
- 链路层：支持 `k/w` 窗口与 `t0/t1/t2/t3` 超时（通过 `LinkConfig.apci` 配置）
- 与 DataCenter 联动：
  - STATION_ROLE_MASTER 收到点值后 `Publish(conn_id, tag, value)` 到 DataCenter
  - STATION_ROLE_SLAVE 订阅 DataCenter `Subscribe(conn_id)`，将点值转为 IEC104 点值自发上送
  - STATION_ROLE_SLAVE 支持总召 `C_IC_NA_1`：通过 DataCenter `GetLatest(conn_id)` 拼装快照应答
  - STATION_ROLE_MASTER 在 STARTDT 成功后自动发起总召 `C_IC_NA_1(QOI=20)`
- 点值合包：自发点值支持窗口合包与 IOA 顺序打包，连续 IOA 使用 SQ=1 压缩；总召快照按帧大小批量打包
- 对时：STATION_ROLE_MASTER 可通过 `time_sync_tag` 订阅触发或 gRPC `SendTimeSync` 主动触发；STATION_ROLE_SLAVE 收到对时命令后发布事件到 DataCenter
- 时标：发送默认使用不带时标类型（`M_SP_NA_1`/`M_ME_NC_1`），可通过 `point_with_time` 切换为带时标；接收兼容带/不带时标类型
- 遥控/设点：主站通过 DataCenter 订阅命令触发发送 `C_SC_NA_1`（预置+执行）与 `C_SE_NC_1`；从站收到命令后发布到 DataCenter

## 接口与协议
- Protobuf：`protobuf/IEC104.proto`
- gRPC Service：`IEC104Proto::IEC104Service`

## 运行与地址
- 对外 gRPC：随机选择 `0.0.0.0:<port>`（17001–17999）
- 内部 gRPC：`unix socket`：`./socket/IEC104.sock`
- 运行时可通过管理器 `GetRunningModuleInfo` 查询实际地址

## 配置与数据
### 关键概念
- `module_name`：固定为 `"IEC104"`（来自 `src/IEC104/include/IEC104LibInfo.h`），用于 DataCenter 的连接主键
- `conn_name`：上位机指定的“代称”，可为任意字符串；要求在 IEC104 模块内唯一
- `conn_id`：DataCenter 分配的连接 ID（稳定且可持久化），上位机后续用它在 DataCenter 配路由/订阅
- `role`：传输角色（Server/Client），只影响 TCP 监听/连接行为
- `station_role`：站点角色（Master/Slave），决定业务语义；未设置时默认 `ROLE_CLIENT -> MASTER`、`ROLE_SERVER -> SLAVE`；可与 `role` 任意组合

### DataCenter 交互与路由配置
- DataCenter 以 `conn_id + tag` 作为路由端点；上位机负责下发连接/连接标签注册表/路由配置，IEC104 仅负责 Publish/Subscribe 与协议互操作。
- IEC104 在 `UpsertPointTable` 成功后，会把点表 tags（包含 `time_sync_tag`）同步到 DataCenter 连接标签注册表，用于路由校验、展示与自愈；这不是协议地址映射点表。
- STATION_ROLE_MASTER：收到 IEC104 点值后 `Publish(conn_id, tag, value)` 到 DataCenter；是否能转发给其他模块由 DataCenter 路由配置决定。
- STATION_ROLE_SLAVE：通过 `Subscribe(conn_id)` 接收 DataCenter 更新并转为 IEC104 自发上送；总召通过 `GetLatest(conn_id)` 拉取快照。
- 语义强调：DataCenter 订阅与最新值缓存为 best-effort，可能丢消息；配置落盘失败会返回错误，但内存状态不回滚。

### 链路层参数（APCI）
- `k/w`：I 帧发送/接收窗口
- `t0`：等待 STARTDT 建链超时（秒）
- `t1`：I 帧确认等待超时（秒）
- `t2`：延迟确认超时（秒，触发 S 帧）
- `t3`：空闲保活超时（秒，触发 TESTFR）
- 参数由上位机在 `LinkConfig.apci` 下发，默认值：`k=12, w=8, t0=30, t1=15, t2=10, t3=20`

### 点表字段
- `scale/offset`：工程量换算 `value = raw * scale + offset`（`scale=0` 视为 1），仅对短浮点生效。
- `deadband`：工程量单位；`|value - last_reported| < deadband` 时不上报，<=0 表示不过滤，仅对短浮点生效。
- `deadband` 同时作用于 STATION_ROLE_MASTER 发布与 STATION_ROLE_SLAVE 自发上送，总召快照不受 deadband 影响。
- STATION_ROLE_MASTER 收到短浮点后按 `scale/offset` 转为工程量再发布；STATION_ROLE_SLAVE 上送短浮点时按 `scale/offset` 反向换算；单点遥信忽略这些字段。
- 遥控/设点复用同一张点表：`POINT_TYPE_SINGLE` 作为单点遥控，`POINT_TYPE_FLOAT` 作为短浮点设点；设点按 `scale/offset` 做工程量换算。

ConfigPusher 点表示例（含 scale/offset/deadband，字段可省略，默认 scale=1、offset=0、deadband=0）：
```jsonc
{
  "tag": "1-A相电压",
  "ioa": 16385,
  "type": "POINT_TYPE_FLOAT",
  "scale": 1.0,
  "offset": 0.0,
  "deadband": 0.0
}
```

### 点值上送参数
- `point_batch_window_ms`：自发点值合包窗口（毫秒）；0 表示使用默认值（20ms）。
- `point_max_asdu_bytes`：ASDU 单帧最大字节数（<=249）；0 表示默认值（249）。
- `point_use_standard_limit`：true 时强制使用标准上限 249 字节，忽略 `point_max_asdu_bytes`。
- `point_dedupe`：自发点值合包去重（按 IOA 保留最新值），默认开启。
- `point_with_time`：点值上送是否带时标；false 默认不带（`M_SP_NA_1`/`M_ME_NC_1`），true 使用带时标（`M_SP_TB_1`/`M_ME_TF_1`）。
- `point_with_time` 同时影响自发上送与总召应答。
- 合包策略：按 IOA 顺序组织报文；连续 IOA 使用 SQ=1 压缩，不连续则 SQ=0 带 IOA。

### 遥控/设点（命令触发）
- 触发方式：上位机或其他模块通过 DataCenter 路由将命令写入 IEC104 的 `conn_id + tag`，IEC104 主站订阅后发送命令。
- 单点遥控：`POINT_TYPE_SINGLE`，DataCenter value 使用 `bool`（或 int/double 非 0 视为 true）；IEC104 发送 `C_SC_NA_1`，先预置再执行。
- 短浮点设点：`POINT_TYPE_FLOAT`，DataCenter value 使用 `double`（或 int 转换为 double）；IEC104 发送 `C_SE_NC_1`（执行）。
- 设点工程量：DataCenter 侧提供工程量；IEC104 发送前按 `scale/offset` 反向换算为原始值；从站收到命令后按 `scale/offset` 正向换算再发布到 DataCenter。
- 命令确认：IEC104 仅在 IEC104 协议内发送 ACT_CON/ACT_TERM，不向 DataCenter 发布确认结果点。

### 对时触发
- `time_sync_tag`：STATION_ROLE_MASTER 订阅该 tag 的 DataCenter 更新并发送对时命令 `C_CS_NA_1`；为空时默认 `__time_sync__`。
- 触发来源：DataCenter 推送更新时优先使用 `update.ts_ms` 作为对时毫秒时间戳；若 <=0 且 value 为 int/double，则取该数值；仍无效时使用本地当前时间。
- gRPC 主动对时：上位机调用 `SendTimeSync(conn_name, ts_ms)`，`ts_ms<=0` 时使用本地当前时间。
- STATION_ROLE_SLAVE 收到对时命令后会发布对时事件到 DataCenter（tag 为 `time_sync_tag`），便于上位机或其他模块订阅。
- 强调：IEC104 不修改系统时钟，仅发送/转发对时报文并发布事件；如需修改系统时间需由上位机或独立模块负责。

### 配置示例
IEC104 链路配置示例（上位机下发的 LinkConfig 内容）：
```jsonc
{
  "conn_name": "line-1",
  "role": "ROLE_CLIENT",
  "station_role": "STATION_ROLE_MASTER",
  "local": { "ip": "0.0.0.0", "port": 0 },
  "remote": { "ip": "192.168.1.10", "port": 2404 },
  "ca": 1,
  "oa": 0,
  "apci": { "k": 12, "w": 8, "t0": 30, "t1": 15, "t2": 10, "t3": 20 },
  "point_batch_window_ms": 20,
  "point_max_asdu_bytes": 240,
  "point_use_standard_limit": false,
  "point_dedupe": true,
  "point_with_time": false,
  "time_sync_tag": "__time_sync__"
}
```

DataCenter 路由配置示例（触发对时）：
```jsonc
{
  "routes": [
    {
      "src": { "conn_id": 2001, "tag": "clock_sync" },
      "dst": { "conn_id": 1001, "tag": "__time_sync__" }
    }
  ],
  "replace": false
}
```

### 集成说明
跨模块的上位机页面结构、配置顺序与操作流程，统一见 `doc/上位机设计指导.md`。IEC104 模块内的字段语义、点表约束、删除语义与对时语义仍以本文档和 `protobuf/IEC104.proto` 为准。

### 本地配置持久化
IEC104 会将本地链路配置与点表配置落盘到工作目录下的 `./conf/IEC104/`，用于进程重启后的自动恢复。

文件与策略：
- 链路主文件：`./conf/IEC104/links.pb`
- 点表主文件：`./conf/IEC104/point_tables.pb`
- 备份文件：对应主文件追加 `.bak`
- 临时文件：对应主文件追加 `.tmp`
- 隔离文件：对应主文件追加 `.corrupt.<timestamp>`

保存时机与语义：
- 每次 `UpsertLink` 成功后自动落盘链路配置。
- 每次 `UpsertPointTable` 成功且已同步 DataCenter 连接标签注册表后自动落盘点表配置。
- 每次 `DeleteLink` 成功后会同步删除本地链路/点表配置并落盘。
- 若 `DeleteLink` 因 DataCenter 删除失败而进入 `PENDING_DELETE`，会将待删除状态一并落盘，便于上位机重启后继续重试删除。
- 落盘失败会返回错误，但内存中的 IEC104 本地配置不会回滚。
- `UpsertLink`、`UpsertPointTable` 成功后仅表示配置已生效并完成必要落盘或 tags 同步，不会自动启动模块内的链路连接功能；如需进入运行态，需由上位机显式调用 `StartLink`。

启动恢复：
- 模块启动时先恢复链路配置，再恢复点表配置。
- 恢复每条链路时会重新向 DataCenter 调用 `GetOrCreateConnection(conn_name)` 对齐当前 `conn_id`；若发现 `conn_id` 变化，会记录 warning，并按需回写本地链路持久化配置。
- 恢复点表后会调用 `UpsertConnTags(conn_id, tags, true)` 重新同步 tags；单条链路恢复失败只记录日志，不影响其他链路继续恢复。
- IEC104 当前采用的“可运行最小条件”为：链路对象处于可启动状态、链路不在 `PENDING_DELETE`，并且该链路对应的点表对象已经成功恢复或下发并通过现有点表校验。
- 模块启动完成后，若恢复出的链路满足上述最小条件，会自动启动模块内的链路连接功能；`PENDING_DELETE` 链路不会自动启动。
- 若恢复阶段发现点表非法、链路与点表不匹配、DataCenter 连接恢复失败或自动启动失败，IEC104 仅记录中文日志并保持模块服务在线，等待上位机后续重新下发修正配置。
- `transport`、订阅线程、`last_error`、运行中的会话状态不会持久化；`StartLink` 是显式启动模块内链路连接功能的唯一入口，对于已运行链路按幂等成功处理。

## 日志
- 日志前缀包含模块名 `[IEC104]`，便于与其他模块混合排查。
- 模块内部线程统一使用 `ModuleManager::StartModuleThread(模块LibInfo.LIB_NAME, ...)` 创建，自动绑定日志模块名上下文。
- 报文日志为 INFO 级别，逐帧输出完整 APDU（包含 `conn_name/角色/长度/数据`）。
- 报文日志输出量大，生产环境需关注日志大小与磁盘占用。

## 测试
相关单元测试位于 `test/`：

- `iec104PointTable_test`：覆盖点表更新、双向查询、冲突校验与序列化输出稳定性。
- `iec104LinkManager_test`：覆盖 LinkManager 与 DataCenter 的交互语义（使用 gMock stub），以及配置校验、点表下发合并、删除语义等边界。
- `iec104Persistence_test`：覆盖 IEC104 链路配置/点表的本地落盘与重启恢复语义。
- `iec104TcpSession_test`：覆盖链路层 STARTDT 握手、自动总召与 t2 延迟确认行为。

运行方式：
```bash
ctest --test-dir build -R iec104PointTable_test --output-on-failure
ctest --test-dir build -R iec104LinkManager_test --output-on-failure
ctest --test-dir build -R iec104Persistence_test --output-on-failure
ctest --test-dir build -R iec104TcpSession_test --output-on-failure
```

## 未实现/后续计划
- 协议类型：目前支持单点遥信 `M_SP_TB_1`、短浮点遥测 `M_ME_TF_1`、总召 `C_IC_NA_1`、单点遥控 `C_SC_NA_1` 与短浮点设点 `C_SE_NC_1`；双点遥控/归一化设点等未实现
- 链路层完善：已支持 `k/w` 与 `t0–t3`；未实现 I 帧重传策略与更细粒度链路统计
- 报文打包：当前仅覆盖短浮点与单点遥信类型，其他类型未实现
- 多主站/多会话：Server 模式当前同一 `conn_name` 只保留一个活动连接；多主站并发、会话级隔离策略未实现
- 点表扩展：当前仅支持短浮点与单点遥信；后续可扩展更多类型与双向映射校验
- 配置持久化增强：当前已持久化连接配置、点表配置与 `PENDING_DELETE` 控制面状态；运行态统计与会话状态未持久化
- 观测性：已补充逐帧日志，但仍缺少链路状态统计（收发计数、最近一次总召、重连次数等）与可配置采样/汇总
- 安全：gRPC 与 IEC104 TCP 目前均为明文/无鉴权；TLS、鉴权、白名单等未实现
- 测试：当前以单元测试覆盖点表与 LinkManager 语义；缺少端到端“主站↔从站”协议互操作测试
- SOE/COS：遥信变位/事件相关语义与时序处理尚未实现；后续需独立队列与不去重策略

## grpc接口
### 删除语义（最佳实践）
删除一条连接时，调用 `DeleteLink(conn_name)`：
- IEC104 会先停止该连接，再调用 DataCenter `DeleteConnection(module_name="IEC104", conn_name)` 清理 `conn_id` 相关配置/缓存
- 若 DataCenter 删除失败（例如 `UNAVAILABLE/INTERNAL`），IEC104 会保留本地配置并标记为 `PENDING_DELETE`
- DataCenter 返回 `NOT_FOUND` 时视为“已删除”，`DeleteLink` 幂等返回 OK

### 对时
主动对时时，调用 `SendTimeSync(conn_name, ts_ms)`：
- 仅 STATION_ROLE_MASTER 允许主动对时；非主站会返回 `FAILED_PRECONDITION`
- `ts_ms<=0` 时使用当前本地时间
- IEC104 会发送 `C_CS_NA_1` 并记录日志；不会修改系统时间

### 请求示例
IEC104 `UpsertLink`（ROLE_CLIENT + STATION_ROLE_MASTER）：
```jsonc
{
  "config": {
    "conn_name": "line-1",
    "role": "ROLE_CLIENT",
    "station_role": "STATION_ROLE_MASTER",
    "remote": { "ip": "192.168.1.10", "port": 2404 },
    "apci": { "k": 12, "w": 8, "t0": 30, "t1": 15, "t2": 10, "t3": 20 },
    "ca": 1,
    "oa": 0,
    "time_sync_tag": "__time_sync__"
  },
  "create_only": true
}
```

IEC104 `UpsertPointTable`（点表下发）：
```jsonc
{
  "conn_name": "line-1",
  "replace": true,
  "points": [
    { "tag": "A相电压", "ioa": 16385, "type": "POINT_TYPE_FLOAT", "scale": 1.0, "offset": 0.0, "deadband": 0.0 },
    { "tag": "断路器合位", "ioa": 100, "type": "POINT_TYPE_SINGLE" }
  ]
}
```

IEC104 `SendTimeSync`（主动对时）：
```jsonc
{
  "conn_name": "line-1",
  "ts_ms": 1710000000000
}
```

DataCenter `Publish`（触发对时，需先配置路由）：
```jsonc
{
  "conn_id": 2001,
  "tag": "clock_sync",
  "value": { "int_value": 1710000000000 },
  "ts_ms": 1710000000000,
  "quality": "QUALITY_GOOD"
}
```

## 构建产物
- 共享库：`package/module/libIEC104.so.<version>`（版本见 `src/IEC104/include/IEC104LibInfo.h`）
