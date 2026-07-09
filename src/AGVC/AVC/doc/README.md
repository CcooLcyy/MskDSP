# AVC 模块

## 简介
AVC（Automatic Voltage Control）自动电压控制模块：从 DataCenter 订阅“目标电压/总无功命令、主电压量测、成员无功量测”等点值，在模块内部按策略计算总无功目标并分配到各成员无功设定点，再通过 DataCenter 有向路由转发到上下游连接（IEC104、ModbusRTU 等）。

## 能力清单
- 按连接管理控制组：一个 `group_name` 对应 DataCenter 的一条连接 `(module_name="AVC", conn_name=group_name) -> conn_id`
- 双命令模式：
  - 目标电压模式：上游下目标电压，AVC 按 `kp/deadband` 计算总无功目标
  - 总无功模式：上游直接下总无功目标，AVC 只做总量约束和成员分配
- 总无功分配：将一个总无功目标值按策略（当前实现为 weighted）分解为多个成员设定点
- 默认点：AVC 会自动生成并注册一组内建点（理论/当前无功上下限、调节返回值、当前电压、总无功目标/实测/偏差、电压偏差），无需手工建点即可直接通过 DataCenter 路由
- 不可控成员支持：不可控成员只参与无功量测汇总与动态上下限计算，不参与分配

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
- `members[]`：成员（例如逆变器）；每个成员至少包含量测点 `q_meas.tag` 与可选设定点 `q_set.tag`

### 默认点
AVC 为每个控制组固定生成以下 10 个默认点，并自动注册到该控制组自己的 `conn_id` 下：
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

### 控制计算
- 目标电压模式：
  - 读取 `voltage_cmd` 与 `voltage_meas`
  - 计算 `error_v = v_ref - v_meas`
  - 若 `|error_v| <= deadband`，则保持 `desired_total_q = current_total_q_meas`
  - 否则 `desired_total_q = current_total_q_meas + kp * error_v`
- 总无功模式：
  - 按 `ABSOLUTE/DELTA` 与 `DELTA_BASE_*` 语义将 `q_total_cmd` 转成 `desired_total_q`
- 上述 `desired_total_q` 会统一钳制到当前总无功能力范围，再按 weighted 策略分配给可控成员
- 输入进入 AVC 内部计算时，绝对量按 `value * scale + offset` 换算，增量量按 `value * scale` 换算；AVC 输出到 DataCenter 时直接发布工程量，不再按 `scale/offset` 反向换算。成员设定配置为 `DELTA` 时发布的是工程量增量值。

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

## 线程与日志
- 模块内部线程统一使用 `ModuleManager::StartModuleThread(模块LibInfo.LIB_NAME, ...)` 创建，自动绑定日志模块名上下文。
- 无需在入口手动创建 `ModuleManager::LogModuleScope`，统一规则见 `src/core/ModuleManager/doc/README.md`。

## 构建产物
- 共享库：`package/module/libAVC.so.<version>`（版本见 `src/AGVC/AVC/cmake/LibInfo.cmake`）
