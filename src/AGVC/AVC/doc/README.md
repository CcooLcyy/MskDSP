# AVC 模块

## 简介
AVC（Automatic Voltage Control）自动电压控制模块：从 DataCenter 订阅“目标电压/总无功命令、主电压量测、成员无功量测”等点值，在模块内部按策略计算总无功目标并分配到各成员无功设定点，再通过 DataCenter 有向路由转发到上下游连接（IEC104、ModbusRTU 等）。

## 能力清单
- 按连接管理控制组：一个 `group_name` 对应 DataCenter 的一条连接 `(module_name="AVC", conn_name=group_name) -> conn_id`
- 双命令模式：
  - 目标电压模式：上游下目标电压，AVC 按 `kp/deadband` 计算总无功目标
  - 总无功模式：上游直接下总无功目标，AVC 只做总量约束和成员分配
- 总无功分配：将一个总无功目标值按策略（当前实现为 weighted）分解为多个成员设定点
- 总无功组和目标电压组都可选 `PI_EVENT` 事件触发调节或 `DIRECT_CYCLIC` 周期控制。两种模式的运行态相互隔离。
- 默认点：AVC 自动注册理论与当前无功上下限、调节返回值、电压与无功量、功能投入、远方操作和增减闭锁，均可通过 DataCenter 配置路由。
- 不可控成员支持：不可控成员只参与无功量测汇总与动态上下限计算，不参与分配
- 同步命令拒绝：当 IEC104 等协议控制命令经 `DataCenter.ExecuteCommand` 进入 AVC 时，若功能未投入、当前不允许远方操作、总无功目标超过当前可调上下限或目标电压模式缺少必要量测，AVC 返回拒绝结果，由协议模块形成负确认

## 接口与协议
- Protobuf：`protobuf/AVC.proto`
- gRPC Service：`AVCProto::AVCService`

## 运行与地址
- 对外 gRPC：随机选择 `0.0.0.0:<port>`（17001–17999）
- 内部 gRPC：`unix socket`：`./socket/AVC.sock`
- 运行时可通过管理器 `GetRunningModuleInfo` 查询实际地址

## 配置与数据
### DataCenter 依赖与通信
AVC 仅与两端通信：
- 上位机 ↔ AVC：通过 `AVCService` 下发/查询配置、启停/改名控制组
- AVC ↔ DataCenter：通过 DataCenter gRPC（建议走 inner unix socket）订阅/发布点值

AVC 不直接对接 IEC104/ModbusRTU；上下游均通过 DataCenter 的有向路由做点值转发。

### 关键概念
- `group_name`：上位机指定的控制组名（模块内唯一）；同时作为 DataCenter 连接主键的 `conn_name`
- `conn_id`：由 DataCenter 分配的连接 ID（稳定且可持久化），上位机后续用它配置路由/订阅
- `voltage_meas`：AVC 主闭环电压测量点
- `voltage_cmd`：主站下发的目标电压点（绝对值）
- `q_total_cmd`：主站下发的总无功目标点（支持绝对值/增量值）
- `control_mode`：控制方式；未指定时兼容为 `PI_EVENT`
- `calculation_execution_period_seconds` 配置周期控制的计算周期，范围 1～15 秒。
- `command_control_period_seconds` 配置周期控制向下游发布成员设定的最小间隔，范围 4～30 秒。主站向 AVC 下发命令的频率独立于这个间隔。
- `members[]`：成员（例如逆变器）；每个成员至少包含量测点 `q_meas.tag` 与可选设定点 `q_set.tag`

### 默认点
AVC 为每个控制组固定生成以下 14 个默认点，并自动注册到该控制组自己的 `conn_id` 下：
- `理论可调无功下限`
- `理论可调无功上限`
- `当前可调无功下限`
- `当前可调无功上限`
- `调节返回值`
- `当前电压`
- `总无功目标`
- `总无功实测`
- `总无功偏差`
- `电压偏差`
- `AVC功能投入`：BOOL 控制点/状态点，true 表示 AVC 自动控制功能投入
- `AVC远方操作`：BOOL 控制点/状态点，true 表示允许远方命令
- `AVC增闭锁`：只读 BOOL 遥信，全部可控成员最近成功发布的绝对无功设点均达到各自上限时为 true。
- `AVC减闭锁`：只读 BOOL 遥信，全部可控成员最近成功发布的绝对无功设点均达到各自下限时为 true。

说明：
- 上述中文名称同时作为默认点的 DataCenter `tag`
- 默认点无需在 `GroupConfig` 中手工声明
- 默认点的 `tag` 视为 AVC 保留名；若用户自定义点复用了这些 `tag`，AVC 会拒绝该配置
- `GetGroup/ListGroups` 返回的 `GroupInfo.default_points` 会显式列出这些默认点元数据，供上位机展示与配路由

### 默认点发布规则
- 理论可调上下限：控制组创建、恢复或配置更新成功后立即发布，质量恒为 `GOOD`
- 当前可调上下限：按 AVC 当前控制口径实时更新；缺测的不可控成员按 `0` 参与计算
- 若存在不可控成员缺测：当前可调上下限仍会发布回退后的数值，但质量置为 `BAD`
- 调节返回值：仅在 AVC 实时收到新的命令点更新时按命令点 `scale/offset` 发布工程量回显，不会在启动阶段回放历史快照时重复回显
- `当前电压 / 总无功目标 / 总无功实测 / 总无功偏差`：在每轮控制计算后更新
- `电压偏差`：仅在目标电压模式下发布；总无功模式下不发布
- `AVC功能投入、AVC远方操作`：控制组创建、恢复、配置更新和状态变更时发布当前 BOOL 状态；两者均为运行时状态，不写入 `GroupConfig`，创建或恢复时默认值均为 `true`
- `AVC增闭锁、AVC减闭锁` 沿用 AGC 的设点判定方式。只读状态按成功发布至 DataCenter 的绝对成员设点计算，成员使用增量下发时仍以对应绝对目标判断。周期缓存中的目标、发布失败的新目标及实测出力变化不更新判定基准。可控成员设点未齐时两点为 false、品质为 `BAD`；无可控成员时两点为 true、品质为 `GOOD`。停止、重新启动控制组功能时保留最近设点，配置更新或进程重启后重新建立缓存。
- 在 IEC104 被控站中可分别从数据总线导入 `AVC增闭锁` 和 `AVC减闭锁`，配置为单点遥信（BOOL）并保存点号和路由。选配的点才会上送主站，未配置转发时仍可在 AVC 默认点中查看。

### 控制计算
`PI_EVENT` 由输入事件触发计算。`DIRECT_CYCLIC` 按周期使用最新命令和量测计算，间隔内收到多个目标时只保留最新值，达到命令控制周期后才向下游发布。总无功组按 `q_total_cmd` 直接分配。目标电压组沿用 `kp/deadband` 和无功限幅，输入保持有效且电压偏差持续超出死区时，即使没有新输入也按周期重新计算。

启动控制组功能后，周期控制收到初始输入即执行首次计算。若首次计算在发布控制结果前因命令更新而失效，立即用最新输入重新计算。首轮之后按配置的计算周期运行，成功下发成员设定后按命令控制周期限制下一次下发。

- 目标电压模式：
  - 读取 `voltage_cmd` 与 `voltage_meas`
  - 计算 `error_v = v_ref - v_meas`
  - 若 `|error_v| <= deadband`，则保持 `desired_total_q = current_total_q_meas`
  - 否则 `desired_total_q = current_total_q_meas + kp * error_v`
- 总无功模式：
  - 按 `ABSOLUTE/DELTA` 与 `DELTA_BASE_*` 语义将 `q_total_cmd` 转成 `desired_total_q`
- 上述 `desired_total_q` 会统一钳制到当前总无功能力范围，再按 weighted 策略分配给可控成员
- 输入进入 AVC 内部计算时，绝对量按 `value * scale + offset` 换算，增量量按 `value * scale` 换算；AVC 输出到 DataCenter 时直接发布工程量，不再按 `scale/offset` 反向换算。成员设定配置为 `DELTA` 时发布的是工程量增量值。
- 若该轮来自 `DataCenter.ExecuteCommand` 同步命令，AVC 会先用未钳制的原始总无功目标与当前可调上下限比较；超限时返回 `COMMAND_REJECTED`，不更新组内命令缓存，也不下发成员设定。
- 周期模式的同步命令仍先执行现有合法性和可调范围校验；校验通过后返回已接受并等待周期调度，不表示下游设备已经执行。

### 配置持久化（当前实现）
AVC 会将控制组配置作为 protobuf payload 写入 `./conf/config.db`，用于进程重启后的自动恢复。

### SQLite 项
- 数据库：`./conf/config.db`
- 表：`config_blobs`
- 模块：`AVC`
- 配置项：`groups`
- protobuf 类型：`AVCProto.GroupsConfig`
- 兼容策略：不再读取旧 `./conf/AVC/groups.pb/.bak/.tmp`；SQLite 中没有 `AVC/groups` 时返回空配置，等待上位机或 ConfigPusher 重新下发。

### 启动恢复
- AVC 启动时会自动加载 SQLite 中的 `AVC/groups`，并按 `group_name` 重新向 DataCenter 调用 `GetOrCreateConnection` 取回稳定 `conn_id`
- 恢复后会重新向 DataCenter 注册 AVC 自身连接标签注册表（`replace=true`），用于路由校验、展示与自愈
- 恢复出的控制组若满足当前最小可运行条件，会在模块启动阶段自动启动组内控制功能
- `StartGroup` RPC 仍保留用于兼容，但已改为幂等语义：控制组已在运行时直接返回成功
- `RUNNING/STOPPED` 表示控制线程与 DataCenter 订阅生命周期，不等同于 `AVC功能投入`；功能未投入时仍可保留量测订阅和状态发布，但不会执行新的成员设定下发。`AVC远方操作=false` 时，来自 IEC104 等协议的总无功/目标电压同步命令会被拒绝。
- 停止控制组功能或切换模式时会清理事件模式和周期模式的临时运行态；两类运行态均不持久化。

## 线程与日志
- 模块内部线程统一使用 `ModuleManager::StartModuleThread(模块LibInfo.LIB_NAME, ...)` 创建，自动绑定日志模块名上下文。
- 无需在入口手动创建 `ModuleManager::LogModuleScope`，统一规则见 `src/core/ModuleManager/doc/README.md`。

## 构建产物
- 共享库：`package/module/libAVC.so.<version>`（版本见 `src/AGVC/AVC/cmake/LibInfo.cmake`）
