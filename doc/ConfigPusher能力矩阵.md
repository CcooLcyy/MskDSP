# ConfigPusher 能力矩阵

## 1. 文档定位

本文档用于集中收束 `ConfigPusher` 的能力矩阵、模块 RPC 映射、导入执行语义以及与上位机同步时需要统一的边界。

本文档不替代模块 README，也不替代上位机页面设计文档：

- `src/ConfigPusher/doc/README.md` 负责说明模块职责、启动方式、配置入口与基础语义。
- `doc/上位机设计指导.md` 负责说明上位机页面职责、交互路径与使用边界。
- 本文档负责收束“ConfigPusher 到底能配什么、实际调用什么、按什么语义生效”。

## 2. 对接能力矩阵

下表建议作为后续与上位机、模板中心或配置生成器同步时的最小基线。不要只同步字段名；还应同时同步“ConfigPusher 会实际调用哪些模块 RPC、最终按什么目标态语义生效”。

| 配置对象 | `jsonc` 入口 | ConfigPusher 实际编排动作 | 最终生效语义 | 自动启动与依赖 | 上位机建议建模 |
| --- | --- | --- | --- | --- | --- |
| 模块启动编排 | `./conf/module_manager.jsonc` 的 `boot_config_mode`、`auto_start_modules` | 通过 ModuleManager 查询模块信息、按需启动 `DataCenter`、`IEC104`、`ModbusRTU`、`DLT645`、`AGC`、`AVC`、`Calc`、`MQTTManager` | `CONFIG_PUSHER` 下由 ConfigPusher 主导初始化编排；`UPPER` 下仅提供服务，不自动下发 | 依赖 ModuleManager；是否执行配置下发由 `boot_config_mode` 决定 | 作为“初始化模式开关”和“模块准备状态”展示，不应混入日常业务对象编辑页 |
| IEC104 链路与点表 | `iec104.links[]` | 先查询现状；必要时停止旧连接功能、清理 `jsonc` 未声明的旧链路；再调用 `UpsertLink`、`UpsertPointTable` | `jsonc` 是目标态快照；同名链路按目标配置覆盖，未声明旧链路不保留 | 依赖 `IEC104` 与 `DataCenter`；配置满足条件后由 IEC104 自动启动模块内连接功能 | 适合建模为“IEC104 初始化模板”；在线增量维护仍建议直连 `IEC104Service` |
| ModbusRTU 链路、点表与 MQTT 全局参数 | `modbus_rtu.links[]`、`modbus_rtu.mqtt` | 先判断是否需要 MQTT；必要时先调 `UpdateConfig`，再查询/收敛旧链路，随后调 `UpsertLink`、`UpsertPointTable` | 链路与点表按目标态覆盖；存在 `TRANSPORT_MQTT_UART` 时要求 `mqtt` 顶层参数完整 | 依赖 `ModbusRTU`、`DataCenter`；MQTT 透传场景还依赖 `MQTTManager`；模块满足条件后自动启动模块内连接功能 | 页面上应区分“本地串口直连”和“MQTT UART 透传”两种模板，不要把 MQTT 全局参数误建成每条链路私有字段 |
| DLT645 链路、点表与 MQTT 全局参数 | `dlt645.links[]`、`dlt645.mqtt` | 先下发 MQTT 全局参数；支持按 `device_nos` 展开为多条链路，再按展开结果收敛旧链路并调用 `UpsertLink`、`UpsertPointTable` | 展开后的连接名集合构成最终目标态；未出现在展开结果中的旧链路不保留 | 依赖 `DLT645`、`DataCenter`；需要 MQTT 时依赖 `MQTTManager`；模块满足条件后自动启动模块内连接功能 | 适合提供“公共模板 + device_nos 批量展开预览”；上位机应保存展开前模板和展开后连接名两层视图 |
| AGC 控制组 | `agc.groups[]` | 先查询现状；必要时停止旧控制组功能、清理 `jsonc` 未声明的旧控制组；再调用 `UpsertGroup` | `jsonc` 未声明的旧控制组不保留；同名控制组按目标配置覆盖 | 依赖 `AGC` 与 `DataCenter`；控制组配置满足条件后由 AGC 自动启动模块内控制功能 | 适合建模为“控制组初始化模板”；在线调优仍建议直连 `AGCService` |
| AVC 控制组 | `avc.groups[]` | 先查询现状；必要时停止旧控制组功能、清理 `jsonc` 未声明的旧控制组；再调用 `UpsertGroup` | `jsonc` 未声明的旧控制组不保留；同名控制组按目标配置覆盖；控制组改名按删除旧组再创建新组处理 | 依赖 `AVC` 与 `DataCenter`；控制组配置满足条件后由 AVC 自动启动模块内控制功能 | 适合建模为“控制组初始化模板”；在线调优仍建议直连 `AVCService` |
| Calc 计算分组 | `calc.groups[]` | 先查询现状；必要时停止旧分组运算功能、清理 `jsonc` 未声明的旧计算分组；再调用 `UpsertGroup` | `jsonc` 未声明的旧计算分组不保留；同名分组按目标配置覆盖；分组改名按删除旧组再创建新组处理 | 依赖 `Calc` 与 `DataCenter`；计算分组配置满足条件后由 Calc 自动启动模块内运算功能 | 适合建模为“派生点/计算分组初始化模板”；外部源点和结果转发仍在 DataCenter 路由页绑定 |
| DataCenter 连接标签注册表与路由 | `point_tables[]`、`routes` | 直接调用 `UpsertConnTags`、`UpsertRoutes`，按目标态覆盖标签与路由 | `point_tables` 表示 ConnTags 目标集合；`routes` 表示路由目标集合；旧标签/旧路由不应残留 | 依赖相关连接已存在；若引用不存在连接，则该次 DataCenter 配置不下发 | 页面上应明确区分“业务模块点表”和“DataCenter 路由/ConnTags”；对已自动同步 tags 的连接，默认只做展示和路由绑定 |

## 3. 与上位机同步时建议记录的最小信息集

若后续要让上位机生成、导入或校验 ConfigPusher 模板，建议至少同步以下信息：

- 配置对象清单：每类对象的 `jsonc` 路径、根字段、唯一标识字段，例如 `conn_name`、`group_name`。
- 编排顺序：例如 ModbusRTU、DLT645 可能需要先下发 MQTT 全局参数，再下发链路与点表。
- 目标态语义：未在本次 `jsonc` 中声明的旧对象是否会被删除、覆盖或清空。
- 自动启动规则：哪些对象配置完成后会由模块自动启动模块内功能，哪些 `start` 字段仅保留兼容说明。
- 前置依赖：对象下发前要求哪些模块已启动、哪些连接已存在、何时需要 `MQTTManager`。
- 批量展开规则：如 DLT645 的 `device_nos`、`{device_no}` 占位符替换与展开后连接名唯一性。
- 字段级约束：必填项、默认值、互斥关系、兼容保留字段、仍沿用历史命名的字段，例如 DataCenter `point_tables`。
- 错误与补救：常见失败原因、是否允许部分成功、失败后应在上位机提示用户修正模板还是改走模块原生 RPC。

## 4. 与上位机同步时不应遗漏的边界

- `ConfigPusher` 只提供 `Ping` 服务；真正的业务配置仍是通过各模块原生 gRPC 接口落地。
- 当前没有“远程触发读取本地 `jsonc` 并执行下发”的对外 RPC；默认只有在模块启动且 `boot_config_mode=CONFIG_PUSHER` 时，才会读取本地模板并执行编排。
- `ConfigPusher` 更适合“模板导入、批量初始化、目标态收敛”，不适合作为上位机日常在线调参的统一控制面。
- 上位机若同时支持“初始化导入”和“在线维护”，应分别维护 `ConfigPusher` 模板模型与模块原生 RPC 模型，不要混成一套表单。
- 对 `start` 字段的任何 UI 呈现都应明确标注“兼容保留，仅记录意图/日志”，不要设计成主路径必点按钮。

## 5. 导入执行语义

- 运行前置编排顺序固定为：查询模块信息与运行态，按需启动 `DataCenter`、`IEC104`、`ModbusRTU`、`MQTTManager`、`DLT645`、`AGC`、`AVC`、`Calc`。
- 进入配置下发阶段后，当前按 `IEC104 -> ModbusRTU -> DLT645 -> AGC -> AVC -> Calc -> DataCenter` 的顺序串行执行。
- 单个协议或模块在下发本次目标态前，会先查询现状；必要时停止仍在运行的模块内功能，并清理 `jsonc` 未声明的旧对象。
- 当前没有跨模块事务；某一类配置下发失败后，不会自动回滚此前已经成功的其他模块或对象。
- 当前没有自动重试；错误会写日志，后续由上位机或实施人员修正模板、或改走模块原生 RPC 后再次执行。
- 已进入配置下发阶段后，允许出现“部分对象成功、部分对象失败”以及“前面模块失败、后面模块继续尝试”的结果；上位机应按模块和对象粒度展示导入结果。
- 当模板内容与当前目标态一致时，编排语义应趋向稳定收敛；但上位机仍不应把它当成严格事务型、全局幂等导入接口来设计。
