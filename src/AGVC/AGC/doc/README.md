# AGC 模块

## 简介
AGC（Automatic Generation Control）自动功率控制模块：从 DataCenter 订阅“总有功设定/成员量测”等点值，在模块内部按策略拆分后发布“各成员有功设定”以及派生点（例如台区总实时功率），并通过 DataCenter 有向路由转发到上下游连接（IEC104、ModbusRTU 等）。

## 能力清单
- 按连接管理控制组：一个 `group_name` 对应 DataCenter 的一条连接 `(module_name="AGC", conn_name=group_name) -> conn_id`
- 总设定拆分：将一个总有功设定值按策略（平均/按容量比例）分解为多个成员设定点
- 多轮分配：成员触顶/触底时自动再分配剩余量，尽量满足总目标
- 事件触发直接分配：在总设定、成员量测或 `base_tag` 等输入变化时，直接按 `desiredTotalKw` 计算成员目标，不再做组总目标的分步逼近
- 派生点：台区总实时有功/总目标/偏差等由 AGC 计算发布，可转发给主站或其他模块
- 不可控成员支持：不可控成员只参与量测汇总，作为“被动出力”从总目标中扣除后再分配给可控成员

## 接口与协议
- Protobuf：`protobuf/AGC.proto`
- gRPC Service：`AGCProto::AGCService`

## 运行与地址
- 对外 gRPC：随机选择 `0.0.0.0:<port>`（7001–7999）
- 内部 gRPC：`unix socket`：`./socket/AGC.sock`
- 运行时可通过管理器 `GetRunningModuleInfo` 查询实际地址

## 配置与数据
### DataCenter 依赖与通信
AGC 仅与两端通信：
- 上位机 ↔ AGC：通过 `AGCService` 下发/查询配置、启停控制组
- AGC ↔ DataCenter：通过 DataCenter gRPC（建议走 inner unix socket）订阅/发布点值

AGC 不直接对接 IEC104/ModbusRTU；上下游均通过 DataCenter 的有向路由做点值转发。

### 关键概念
- `group_name`：上位机指定的控制组名（模块内唯一）；同时作为 DataCenter 连接主键的 `conn_name`
- `conn_id`：由 DataCenter 分配的连接 ID（稳定且可持久化），上位机后续用它配置路由/订阅
- `p_cmd`：主站下发的总有功设定点（通过 DataCenter 路由进入 AGC 的 `conn_id` 内某个 `tag`）
- `members[]`：成员（例如逆变器）；每个成员至少包含量测点 `p_meas.tag` 与可选设定点 `p_set.tag`
- `outputs`：派生输出点（台区总实时/目标/偏差）

### ValueSpec：目标值/增量值语义（可配置）
`ValueSpec` 用于描述一个点的 `tag/unit/scale/offset` 与值语义：
- `mode=ABSOLUTE`：该点表示“绝对目标值”（例如 kW）
- `mode=DELTA`：该点表示“增量值”，其基准由 `delta_base` 决定：
  - `LAST_TARGET`：相对“上一轮期望总目标值”（AGC 内部记忆）
  - `CURRENT_MEAS`：相对“当前量测值”（来自成员量测点）
  - `BASE_TAG`：相对 `base_tag` 对应点值（同一 `conn_id` 内，通过 DataCenter 路由输入）

> 说明：AGC 内部统一用“kW 绝对值”做计算；当输出配置为 `DELTA` 时，AGC 会在发布前将目标值转换为增量值。

### 控制计算（事件触发直接分配）
每次控制触发的计算过程如下：
- 读取当前总设定输入 `p_cmd`、成员量测以及 `base_tag`
- 按 `ABSOLUTE/DELTA` 与 `DELTA_BASE_*` 语义计算 `desiredTotalKw`
- 直接以 `desiredTotalKw` 作为本轮组总目标，不再应用 `kp`、`max_step_kw`、`deadband_kw` 之类的渐进推进参数
- 先扣减不可控成员的被动出力，再把剩余目标按 weighted 策略和成员 `min_kw/max_kw` 约束分配给可控成员
- 若成员约束导致无法完全分配，记录 `unallocated` 并输出告警日志；`outputs.p_total_target` 发布的是本轮实际可下发的总目标值

#### 计算示例
- total_meas=30, desired_total=60 → target=60 → 按权重 1:2 分配为 [20, 40]
- total_meas=100, last_total_target=90, desired_total=105 → 本轮仍直接按 105 分配，不再从 90 逐步逼近

### 不可控成员建议
对不可控成员（`controllable=false`）：
- 量测参与 `p_total_meas` 汇总
- 不参与拆分分配
- 拆分时将其量测视为“被动出力”从总目标中扣除，再把剩余目标分给可控成员

### 上位机推荐流程（典型闭环）
1. 启动 `DataCenter` 与 `AGC`，用 `GetRunningModuleInfo` 获取 AGC 的 `outer_grpc_server`
2. 上位机连接 AGC gRPC，调用 `UpsertGroup(create_only=true)` 创建控制组并获取 `conn_id`
3. 在 DataCenter 配置路由（示例）：
   - 主站设点（IEC104 conn）→ AGC：`(conn_id_104, P_CMD_SRC) -> (conn_id_agc, p_cmd.signal.tag)`
   - 成员量测（ModbusRTU conn）→ AGC：`(conn_id_inv1, P_MEAS) -> (conn_id_agc, members[0].p_meas.tag)`（依次类推）
   - AGC 成员设点 → 成员控制点：`(conn_id_agc, members[0].p_set.signal.tag) -> (conn_id_inv1, P_SET)`（依次类推）
   - 台区总实时 → 主站：`(conn_id_agc, outputs.p_total_meas.tag) -> (conn_id_104, P_TOTAL_DST)`
4. 调用 `StartGroup` 启动控制组内事件触发控制功能

> 注意：路由与点表的具体 `tag`/单位由上位机配置决定；AGC 本身不关心 IEC104/ModbusRTU 的地址映射。
> 上位机建议：`GroupConfig` 不再支持 `kp`、`max_step_kw`、`deadband_kw`、`period_ms` 等控制环参数；若上位机仍需要限幅、缓升缓降或分步给定，请在 AGC 上游先生成更细粒度的总设定，再通过 `p_cmd` 下发。

### 配置持久化（当前实现）
AGC 会将控制组配置落盘到工作目录下的 `./conf/AGC/groups.pb`，用于进程重启后的自动恢复。

### 文件与策略
- 主文件：`./conf/AGC/groups.pb`
- 备份文件：`./conf/AGC/groups.pb.bak`
- 临时文件：`./conf/AGC/groups.pb.tmp`
- 隔离文件：`./conf/AGC/groups.pb.corrupt.<timestamp>`

### 保存时机与语义
- 每次 `UpsertGroup` / `DeleteGroup` 完成本地配置变更后自动落盘。
- 落盘失败会返回 `INTERNAL`，但内存中的控制组配置不会回滚。
- `RUNNING/STOPPED` 等瞬时运行态不落盘；若 `DeleteGroup` 因 DataCenter 删除失败而进入 `PENDING_DELETE`，会将待删除控制面状态一并落盘。
- 订阅线程、事件控制线程与控制缓存不落盘。

### 启动恢复
- AGC 启动时会自动加载 `groups.pb`，并按 `group_name` 重新向 DataCenter 调用 `GetOrCreateConnection` 取回稳定 `conn_id`。
- 恢复后会重新向 DataCenter 注册 AGC 自身连接标签注册表（`replace=true`），用于路由校验、展示与自愈。
- 恢复出的控制组统一为 `STOPPED` 或 `PENDING_DELETE`，不会自动启动控制组内事件触发控制功能；如需启动仍由上位机调用 `StartGroup`。
- 因为 DataCenter 已持久化连接/连接标签注册表/路由，正常情况下重启后无需重新下发 AGC 路由。

### 当前限制/注意事项
- `StrategyConfig` 目前仅实现加权分配（WeightedStrategy），其他策略为预留。
- `DELTA_BASE_LAST_TARGET` 的基准为“上一轮期望总目标值”（`desired_total`）。
- `GroupConfig` 不再暴露控制环/步长限制参数；旧版 `loop` 字段配置需要从上位机与配置下发链路中移除。
- 当成员约束导致目标无法完全分配时，会记录 `unallocated` 并输出告警日志。
- DataCenter 订阅流异常中断时仅记录 `last_error`，需上位机或外部机制触发重试。
- 可在 `package/log` 查看关键告警（如 `AGC 分配受限`）。

## 线程与日志
- 模块内部线程统一使用 `ModuleManager::StartModuleThread(模块LibInfo.LIB_NAME, ...)` 创建，自动绑定日志模块名上下文。
- 无需在入口手动创建 `ModuleManager::LogModuleScope`，统一规则见 `src/core/ModuleManager/doc/README.md`。

## 构建产物
- 共享库：`package/module/libAGC.so.<version>`（版本见 `src/AGVC/AGC/cmake/LibInfo.cmake`）
