# AGC 模块

## 简介
AGC（Automatic Generation Control）自动功率控制模块：从 DataCenter 订阅“总有功设定/成员量测”等点值，在模块内部按策略拆分后发布“各成员有功设定”以及派生点（例如台区总实时功率），并通过 DataCenter 有向路由转发到上下游连接（IEC104、ModbusRTU 等）。

## 能力清单
- 按连接管理控制组：一个 `group_name` 对应 DataCenter 的一条连接 `(module_name="AGC", conn_name=group_name) -> conn_id`
- 总设定拆分：将一个总有功设定值按策略（平均/按容量比例）分解为多个成员设定点
- 多轮分配：成员触顶/触底时自动再分配剩余量，尽量满足总目标
- 多次调节（周期闭环）：按周期计算总功率偏差并分步逼近目标（避免一次到位失败/设备响应慢）
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

### 控制环（多次调节）
`ControlLoopConfig` 当前实现为“比例 + 步长限制”的分步逼近：
- 每周期计算 `error = desired_total - total_meas`
- `step = clamp(kp * error, -max_step_kw, +max_step_kw)`，并应用 `deadband_kw`
- 目标总值按 `step` 逐步逼近 `desired_total`，然后再拆分到各成员
> 说明：若未配置 `period_ms`，默认 200ms。

#### 计算示例
- total_meas=30, desired_total=60, kp=1, max_step=0 → target=60 → 按权重 1:2 分配为 [20, 40]
- deadband=10 且 |error|<=10 → step=0 → target=last_total_target（若有）或 total_meas

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
4. 调用 `StartGroup` 启动控制组

> 注意：路由与点表的具体 `tag`/单位由上位机配置决定；AGC 本身不关心 IEC104/ModbusRTU 的地址映射。

### 配置持久化（当前实现）
当前版本的 `GroupConfig`/运行态不落盘：进程重启后需要上位机重新下发 `UpsertGroup` 并重建 DataCenter 路由。

### 当前限制/注意事项
- `StrategyConfig` 目前仅实现加权分配（WeightedStrategy），其他策略为预留。
- `DELTA_BASE_LAST_TARGET` 的基准为“上一轮期望总目标值”（`desired_total`）。
- 当成员约束导致目标无法完全分配时，会记录 `unallocated` 并输出告警日志。
- DataCenter 订阅流异常中断时仅记录 `last_error`，需上位机或外部机制触发重试。
- 可在 `package/log` 查看关键告警（如 `AGC 分配受限`）。

## 构建产物
- 共享库：`package/module/libAGC.so.<version>`（版本见 `src/AGVC/AGC/cmake/LibInfo.cmake`）
