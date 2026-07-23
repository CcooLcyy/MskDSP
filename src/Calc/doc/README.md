# Calc 模块

## 简介
Calc 类型化运算模块：从 DataCenter 订阅同一计算分组 `conn_id` 下的内建输入点，在模块内部执行数值/逻辑运算后，将结果发布到对应的内建结果点，并通过 DataCenter 路由转发到上下游连接。

## 能力清单
- 按分组管理计算：一个 `group_name` 对应 DataCenter 的一条连接 `(module_name="Calc", conn_name=group_name) -> conn_id`
- 分组内多 item：同一个 group 可包含多条计算项
- 类型化运算：支持二元数值运算 `ADD/SUB/MUL/DIV`、多项聚合 `SUM/AVERAGE` 与逻辑运算 `NOT/AND/OR/XOR`
- 类型化常量：支持 `bool/int64/double` 常量
- 自动内建点：二元/逻辑 item 自动生成 `left_input/right_input/result`；SUM/AVERAGE 按操作数顺序生成 `input_1/input_2/.../result`，并同步到 DataCenter 连接标签注册表
- 自动启动：分组配置达到当前最小可运行条件后，会自动尝试启动分组内运算功能

## 接口与协议
- Protobuf：`protobuf/Calc.proto`
- gRPC Service：`CalcProto::CalcService`
- ConfigPusher 初始化模板：`./conf/configPusher/calc.jsonc`

控制面 RPC 固定为 group 级 7 项：

- `UpsertGroup`
- `RenameGroup`
- `GetGroup`
- `ListGroups`
- `DeleteGroup`
- `StartGroup`
- `StopGroup`

`CalcItem` 不单独暴露 item 级 CRUD/启停 RPC。上位机应把 item 视为 `group` 的子配置，通过一次 `UpsertGroup` 覆盖整组 items。

ConfigPusher 在 `CONFIG_PUSHER` 模式下也复用 `UpsertGroup` 作为初始化下发入口：`calc.groups[].upsert` 表示目标态计算分组配置；`jsonc` 未声明的旧分组会被收敛删除，同名运行中分组会先停止分组运算功能再覆盖配置。`calc.groups[].start` 仅保留兼容日志，不会额外调用 `StartGroup`。

## 运行与地址
- 对外 gRPC：随机选择 `0.0.0.0:<port>`（17001–17999）
- 内部 gRPC：`unix socket`：`./socket/Calc.sock`
- 运行时可通过管理器 `GetRunningModuleInfo` 查询实际地址

## 配置与数据
### DataCenter 依赖与通信
Calc 仅与两端通信：
- 上位机 ↔ Calc：通过 `CalcService` 下发/查询配置、启停计算分组
- Calc ↔ DataCenter：通过 DataCenter gRPC 订阅输入点与发布结果点

Calc 不直接对接 IEC104/ModbusRTU/DLT645；上下游均通过 DataCenter 的有向路由做点值转发。

### 关键概念
- `group_name`：上位机指定的计算分组名（模块内唯一）；同时作为 DataCenter 连接主键的 `conn_name`
- `conn_id`：由 DataCenter 分配的连接 ID（稳定且可持久化）
- `item_name`：分组内唯一的计算项名
- `left_input/right_input/result`：二元或逻辑 item 自动派生的内建点角色，实际 tag 形式分别为 `<item_name>/left_input`、`<item_name>/right_input`、`<item_name>/result`
- `operands`：SUM/AVERAGE 的有序操作数列表，每项可以是 DataCenter 路由输入或类型化常量；对应输入 tag 按 1-based 顺序派生为 `<item_name>/input_1`、`<item_name>/input_2` 等，结果统一发布到 `<item_name>/result`
- `decimal_places`：AVERAGE 可选的小数位数；未设置时不主动舍入，设置后按普通四舍五入输出指定小数位

### 运算与类型规则
- 数值运算：`ADD/SUB/MUL/DIV` 仅接受 `int64/double`
- 聚合运算：`SUM/AVERAGE` 至少需要两个 `operands`，仅接受 `int64/double`；SUM 在全为 int64 且不溢出时输出 int64，否则输出 double；AVERAGE 始终输出 double
- 逻辑运算：`NOT/AND/OR/XOR` 仅接受 `bool`
- 类型不匹配：记录中文日志并跳过本轮结果发布
- `DIV` 除零：记录中文告警日志并跳过本轮结果发布
- `ADD/SUB/MUL` 在两侧均为 `int64` 且结果溢出时，会自动提升为 `double`
- `SUM` 在任一路输入缺失时等待，不发布部分和；`AVERAGE` 同样等待所有路由输入到齐，平均值分母为配置的操作数总数
- 全常量 SUM/AVERAGE 在分组运算功能启动时即可计算并发布一次；含路由输入的计算在收到全部输入后发布，后续按最新值重新计算

### 上位机选点约束
- 上位机“选择已有点”仅负责发现 DataCenter 连接和 tag，并维护到二元 item 的 `<item_name>/left_input`、`<item_name>/right_input` 或聚合 item 的 `<item_name>/input_N` 的 Route
- Calc 自身不保存外部源点 `conn/tag`，避免与 DataCenter Route 形成两份真相
- `GetGroup/ListGroups` 返回的旧 `left_input_tag/right_input_tag/result_tag` 继续用于二元 item；聚合 item 通过 `input_tags` 按操作数顺序返回输入点，并通过 `result_tag` 返回结果点
- 即使 `NOT` 这类单目运算，`right_input_tag` 也会保留展示，但不会参与本轮计算
- ConfigPusher 模板只声明 Calc 计算分组与 item；外部源点到内建输入点、内建结果点到下游点的绑定仍由 DataCenter Route 表达

### 运行状态与缺测诊断
- Calc 对每个操作数维护就绪状态、最后一次质量和时间戳，并在 `CalcItemInfo` 返回 `operand_status`
- 某一路由输入尚未收到点值时，该操作数状态标明具体 input tag 和等待原因，当前计算项不发布结果
- Calc 能确认的事实是“尚未收到该输入点数据”；上位机可结合 DataCenter Route 列表进一步提示“未配置路由”或“已配置但上游尚未发布/设备离线”等可能原因
- 输入已经到达但类型不支持、平均值精度配置非法等情况，会记录中文错误原因并跳过结果发布

### 上位机建模建议
- 页面建议按“分组列表 + item 列表 + 运算配置 + 内建点说明 + 运行状态”组织，而不是拆成 item 级独立 CRUD
- 数值运算仅允许为常量选择 `int64/double`；逻辑运算仅允许为常量选择 `bool`
- “已有点 + 已有点”与“已有点 + 常量”都应通过同一个 `CalcItemConfig` 表达，不要为两者拆两套模型
- `conn_id` 应作为关键只读字段展示，供用户在 `数据总线` 页面确认 Route 是否绑定到正确分组

### 配置持久化（当前实现）
- Calc 会将计算分组配置作为 protobuf payload 写入 `./conf/config.db`，用于进程重启后的自动恢复。
- SQLite 表：`config_blobs`
- 模块：`Calc`
- 配置项：`groups`
- protobuf 类型：`CalcProto.GroupsConfig`
- 兼容策略：不再读取旧 `./conf/Calc/groups.pb/.bak/.tmp`；SQLite 中没有 `Calc/groups` 时返回空配置，等待上位机或 ConfigPusher 重新下发。

## 线程与日志
- 模块内部线程统一使用 `ModuleManager::StartModuleThread(模块LibInfo.LIB_NAME, ...)` 创建，自动绑定日志模块名上下文。
- DataCenter 订阅线程异常退出时，会记录中文 `last_error` 与日志，等待上位机修正或手工重新启动分组功能。

## 构建产物
- 共享库：`package/module/libCalc.so.<version>`（版本见 `src/Calc/cmake/LibInfo.cmake`）
