# DataCenter 模块

## 定位与边界
DataCenter 是进程内的“数据总线/转发枢纽”，用于在不同协议连接之间转发数据（例如 ModbusRTU 抄表 → IEC104 上报），以实现跨协议的数据互通。

## 能力清单
- 连接级隔离：通过 `connId` 区分不同连接/通信链路
- connId 分配器：按 `(module_name, conn_name)` 分配/查询 `connId`（全局唯一、可持久化）
- 点位对齐：通过 `tag`（逻辑点名，可中文）对齐跨协议的同一业务量
- 有向路由：按点位维度配置 `src -> dst` 的转发规则，支持一对一与一对多
- 同步命令执行：按唯一路由把协议控制命令转交目标模块，用于 IEC104 等协议形成正/负确认
- 最新值缓存：支持 `GetLatest` / `Subscribe(snapshot=true)` 获取目的连接内的最新值（best-effort）
- 源端实时监视：支持 `GetSourceLatest` 获取协议模块已发布的源端最新值，不依赖业务路由
- 完整状态持久化：将连接注册表、连接标签注册表、路由配置合并落盘到 `./conf/config.db`，重启后按同一份快照恢复

## 暂未实现功能
- 历史数据存储/查询
- 聚合/判据/计算点（例如“多个遥信判断一个遥信状态”建议由独立规则/计算模块实现，订阅原始点后发布派生点）
- 多对一仲裁（多个源同时写入同一目的点时暂不保证行为）

## 关键概念
- `connId`：连接的运行时全局唯一 ID（建议使用无符号整型），建议通过 DataCenter 的 connId 分配器按 `(module_name, conn_name)` 分配/查询；运行发布、订阅、最新值查询继续使用当前 `connId`。
- `tag`：逻辑点名（UTF-8，可使用中文），用于跨协议对齐同一业务量；协议地址（IOA/寄存器等）应由各协议模块自身点表维护。
- `Endpoint`：路由配置端点优先使用 `(module_name, conn_name, tag)` 作为稳定身份；`conn_id` 仅作为当前运行时展示、过滤或兼容调用字段。
- `ConnTags`：连接标签注册表以 `(module_name, conn_name)` 作为稳定身份保存，`conn_id` 仅作为当前运行态查询/展示字段。
- `Route`：有向路由绑定，表示数据从 `src Endpoint` 转发到一个或多个 `dst Endpoint`。

## 数据方向与路由
- 路由按点位维度配置，且为有向：`src -> dst`；双向传输需要配置两条路由。
- 支持关系：
  - 一对一：`(1, srcTag) -> (2, dstTag)`（允许 `srcTag != dstTag`，转发时会按目的端点重写为 `dstTag`）
  - 一对多：`(1, srcTag) -> (2, dstTag1)` + `(1, srcTag) -> (3, dstTag2)` + ...
- 多对一：多个源同时写入同一目的点的行为当前不做仲裁，暂不保证一致性（可能出现顺序相关/last-write-wins），建议避免配置此类规则。

## 接口与协议
- Protobuf：`protobuf/DataCenter.proto`
- gRPC Service：`DataCenterProto::DataCenterService`

## gRPC 接口说明（当前实现）
DataCenter 对外提供一组面向“连接/连接标签注册表/路由/转发”的 gRPC 接口，用于支撑跨协议数据转发闭环。

### 术语
- `Endpoint`：路由配置端点优先使用 `(module_name, conn_name, tag)`；请求只传 `(conn_id, tag)` 时会按当前连接注册表转换为稳定端点。
- `Route`：有向绑定 `src Endpoint -> dst Endpoint`；允许 `srcTag != dstTag`，转发时会按目的端点重写为 `dstTag`。
- `PointUpdate`：路由后的更新，包含 `src/dst` 端点信息与 `value/ts_ms/quality`。

### 连接/连接标签注册表（用于展示与校验，可选）
- `GetOrCreateConnection(GetOrCreateConnectionRequest) -> ConnectionInfo`
  - connId 分配器：按 `(module_name, conn_name)` 分配/查询 `conn_id`（若已存在则返回既有 `conn_id`；否则分配新 ID 并落盘持久化）。
- `RenameConnection(RenameConnectionRequest) -> ConnectionInfo`
  - 重命名连接主键：将 `old_key` 改为 `new_key`，并保持 `conn_id` 不变；若 `new_key` 已存在则返回 `ALREADY_EXISTS`。
- `DeleteConnection(DeleteConnectionRequest) -> Empty`
  - 删除连接（按 `(module_name, conn_name)`）；会同步删除该 `conn_id` 关联的连接标签注册表/路由/源端与目的端最新值缓存，并关闭该 `conn_id` 的订阅者连接（best-effort），随后落盘持久化。
- `UpsertConnection(UpsertConnectionRequest) -> Empty`
  - 兼容接口：更新已分配 `conn_id` 的连接信息（不负责分配 `conn_id`；若 `conn_id` 未在注册表中会返回 `NOT_FOUND`）。
- `ListConnections(Empty) -> ListConnectionsResponse`
  - 列出已注册连接信息（用于 UI/配置工具展示）。
- `UpsertConnTags(UpsertConnTagsRequest) -> Empty`
  - 注册/更新某连接的连接标签注册表（`replace=true` 全量覆盖；否则增量追加）。
  - 说明：请求仍使用当前 `conn_id`，DataCenter 会先通过连接注册表解析成 `(module_name, conn_name)`，再按稳定连接主键保存 tags。
  - 说明：`replace=true` 覆盖后，会同步删除引用已移除 tag 的路由，保证连接、tag、路由始终对齐。
  - 说明：该 RPC 成功后会触发完整状态落盘；若落盘失败会返回 `INTERNAL`（内存中的注册表不会回滚）。
- `GetConnTags(GetConnTagsRequest) -> ConnTags`
  - 获取某连接的连接标签注册表；若未注册返回 `NOT_FOUND`。
  - 说明：连接标签注册表并非硬依赖；但当其存在时，`UpsertRoutes` 会校验 `tag` 必须在注册表中，避免配置错误。

### 路由管理（有向绑定）
- `UpsertRoutes(UpsertRoutesRequest) -> Empty`
  - 配置路由；`replace=true` 会用请求中的路由整体替换全部路由，`replace=false` 增量追加。
  - 新配置应填写端点的 `module_name/conn_name/tag`；只填 `conn_id/tag` 时，该 `conn_id` 必须能从当前连接注册表解析为稳定连接主键。
  - 原子性：DataCenter 会先构造并校验新路由集合，全部通过后才替换内存状态；校验失败时旧路由保持不变。
  - 支持一对一与一对多；多对一暂不保证行为（建议避免）。
  - 说明：该 RPC 成功后会触发完整状态落盘；若落盘失败会返回 `INTERNAL`（内存中的路由不会回滚）。
- `DeleteRoutes(DeleteRoutesRequest) -> Empty`
  - 删除指定路由绑定。
  - 支持按稳定端点删除；`conn_id/tag` 会先按当前连接注册表转换为稳定端点。
  - 说明：该 RPC 成功后会触发完整状态落盘；若落盘失败会返回 `INTERNAL`（内存中的路由不会回滚）。
- `ListRoutes(ListRoutesRequest) -> ListRoutesResponse`
  - 查询路由；支持按 `src/dst` 的 `conn_id/tag` 进行可选过滤，响应会同时返回稳定字段与当前 `conn_id`。

### 数据转发（核心）
- `Publish(PublishRequest) -> Empty`
  - 源连接发布单点数据：`(conn_id, tag, value[, ts_ms][, quality])`。
  - DataCenter 按路由生成 `PointUpdate`，推送给目的连接的订阅者，并更新目的端点的“最新值缓存”。
  - `ts_ms<=0` 时由 DataCenter 填充当前毫秒时间戳。
- `BatchPublish(BatchPublishRequest) -> Empty`
  - 批量发布（减少 RPC 次数）；原子语义：会先校验全部点，若任一参数错误则整批失败且不产生任何更新（不更新最新值缓存/不推送订阅更新）。
- `ExecuteCommand(ExecuteCommandRequest) -> ExecuteCommandResponse`
  - 同步执行命令：DataCenter 按源端点路由解析出唯一目的端点，并调用目标模块实现的 `CommandExecutor.ExecuteCommand`。
  - 该接口仅用于需要协议级正/负确认的控制命令，不替代 `Publish/Subscribe` 数据转发；没有路由、多路由、目标模块不可达、目标拒绝或超时时，会通过 `status/reject_code/reason` 返回失败原因。
  - 目标模块返回 `COMMAND_ACCEPTED` 时，DataCenter 更新目的命令点最新值缓存；返回 `COMMAND_REJECTED` 时不更新该命令点缓存。
  - DataCenter 会将上游 gRPC 调用的截止时间与请求 `timeout_ms` 取较早者设置到目标 `CommandExecutor`；上游调用取消或截止时间到达时，会调用目标 RPC 的取消接口并等待其退出，避免目标协议模块继续执行已取消的控制命令。
  - 上游调用已取消/超过截止时间时，DataCenter 直接返回对应的 gRPC `CANCELLED`/`DEADLINE_EXCEEDED` 状态，不把该次调用误记为已确认成功。
  - 路由解析期间或路由返回无目标/多目标结果后若上游已取消或超时，也直接返回对应的 gRPC 状态，不返回过期的路由业务结果。
- `GetLatest(GetLatestRequest) -> GetLatestResponse`
  - 拉取目的连接内的最新值（best-effort，仅最新值，不含历史）；`tags` 为空表示拉取该连接全部已缓存点。
- `GetSourceLatest(GetSourceLatestRequest) -> GetSourceLatestResponse`
  - 拉取源连接已发布点的最新值（best-effort，仅最新值，不含历史）；`tags` 为空表示拉取该连接全部源端缓存点。
  - 此接口用于上位机协议页实时监视，不要求配置业务 Route，也不读取或修改连接标签注册表、路由和持久化配置。
  - 返回的 `SourcePointUpdate` 包含 `conn_id/tag/value/ts_ms/quality/sequence`；`sequence` 在 DataCenter 当前进程内单调递增，可用于消费方拒绝乱序响应。
  - 连接不存在时返回 `NOT_FOUND`；连接存在但尚未收到指定点的发布时返回空 updates。
- `GetThroughputSnapshot(Empty) -> ThroughputSnapshot`
  - 返回当前 DataCenter 进程内最近 60 秒的路由转发吞吐量窗口，按秒统计 Publish/BatchPublish 生成的目的端点更新数。
  - 不统计源端入站数、订阅投递数或发布失败数；窗口只保存在内存，进程重启后清空。
- `Subscribe(SubscribeRequest) -> stream PointUpdate`
  - 目的连接订阅数据更新（服务端流）；`tags` 为空表示订阅该连接全部点。
  - `snapshot=true` 时，会先 best-effort 推送一次 `GetLatest` 的结果，再推送实时更新。
  - 说明：订阅端消费过慢时，服务端会丢弃过旧消息以避免无限堆积（best-effort，不保证每条更新都可达）。

## 集成约束与关键语义
涉及上位机页面结构、配置流程与操作顺序的统一说明，见 `doc/上位机设计指导.md`。本节仅保留 DataCenter 本身的连接主键、路由校验与订阅语义。

### 1) 连接主键与命名
- `ConnectionKey = (module_name, conn_name)` 是 `connId` 分配的唯一主键：两者都应当稳定且可预测，避免运行期随机字符串。
- `module_name` 建议与模块标识保持一致（例如 `IEC104`、`ModbusRTU`），用于区分不同协议模块空间。
- `conn_name` 建议由集成侧统一规划并保证同一 `module_name` 内唯一（例如 `104-主站A`、`ModbusRTU-1#RTU`）。

### 2) connId 分配与删除语义
- 集成侧或协议模块在“配置阶段”先调用 `GetOrCreateConnection` 获取 `connId`，并将返回的 `connId` 固化到该连接配置中（后续运行期 `Publish/Subscribe` 使用同一 `connId`）。
- `RenameConnection` 用于仅修改主键（`connId` 不变）：DataCenter 会同步改写稳定路由中的连接主键引用。
- `DeleteConnection` 为破坏性操作：会清理该 `connId` 的连接标签注册表/路由/源端与目的端最新值缓存，并关闭该 `connId` 的订阅者连接（best-effort）。

### 3) 标签注册表与路由校验
- 当连接标签注册表存在时，路由会校验 `tag` 必须在注册表中；因此连接标签注册表/路由的 UI 编辑可通过 `GetConnTags` / `ListRoutes` 做回读校验与展示。
- 配置端应保存和下发 `module_name/conn_name/tag`；`conn_id` 可展示给用户并用于运行期查询，但不应作为连接标签注册表或路由的长期持久化身份。

### 4) 实时值与订阅语义
- 接收方启动后若希望“先拿到当前值再跟随实时更新”，建议使用 `Subscribe(snapshot=true)`；若只关心一次性拉取，则使用 `GetLatest`。
- 订阅为 best-effort：消费过慢时服务端会丢弃过旧消息以限制队列增长；如需强一致或历史回放，需要引入独立的历史存储或 ACK/重传机制（不在 DataCenter 当前范围内）。
- `GetLatest` 的对象是路由后的目的端点；协议采集原始值监视应使用 `GetSourceLatest`，不能通过额外业务 Route 或持久化影子 Route 取得。
- 源端最新值仅存在于内存，DataCenter 重启或删除对应连接后会清空；这符合实时监视语义，不提供历史回放。

## 完整状态持久化（当前实现）
DataCenter 会将连接注册表、连接标签注册表与路由配置作为同一份完整状态落盘到工作目录下的 `./conf/config.db`。这样重启恢复时三类配置来自同一代快照，避免 `connections/conn_tags/routes` 分文件落盘导致连接、tag、路由对不齐。

### 文件与策略
- SQLite 配置项：`DataCenter/state`
- 兼容策略：不再读取旧状态文件；SQLite 中没有 `DataCenter/state` 时返回空状态，等待上位机或 ConfigPusher 重新下发。
- 正常保存：只写 `./conf/config.db`。
- 状态版本：`DataCenterState.schema_version=1`，保存时自动写入，用于校验 SQLite payload 格式。

### 保存时机与语义
- 每次 `GetOrCreateConnection` / `RenameConnection` / `DeleteConnection` / `UpsertConnection` / `UpsertConnTags` / `UpsertRoutes` / `DeleteRoutes` 成功后自动落盘完整状态。
- `DeleteConnection` 会先在内存中同步删除该连接关联的连接标签注册表/路由/源端与目的端最新值缓存，再将完整状态一次落盘。
- `UpsertConnTags(replace=true)` 会先在内存中同步删除引用已移除 tag 的路由，再将完整状态一次落盘。
- 落盘失败会返回 `INTERNAL`，但内存中的状态不会回滚（配置端可重试）。

### 启动恢复
- 若 SQLite 中存在 `DataCenter/state` 且 payload 可解析、可校验：加载其中的完整状态。
- 若 SQLite 中没有 `DataCenter/state`：以空状态启动，等待重新下发。
- 若完整状态可解析但内部校验失败：记录日志并不应用该状态。

## 线程与日志
- 模块内部线程统一使用 `ModuleManager::StartModuleThread(模块LibInfo.LIB_NAME, ...)` 创建，自动绑定日志模块名上下文。
- 无需在入口手动创建 `ModuleManager::LogModuleScope`，统一规则见 `src/core/ModuleManager/doc/README.md`。

## 测试
相关单元测试位于 `test/`：

- `dataCenterCore_test`：覆盖连接分配/重命名/删除、连接标签注册表/路由校验、转发与最新值缓存等核心语义。
- `dataCenterStateStore_test`：覆盖完整状态配置落盘/读取、稳定连接主键校验与损坏回退策略。

运行方式：
```bash
ctest --test-dir build -R dataCenterCore_test --output-on-failure
ctest --test-dir build -R dataCenterStateStore_test --output-on-failure
```

## 其他内容
运行时工作目录、socket 目录、端口策略与构建产物路径遵循项目通用约定，详见 [README.md](../../../README.md) 与 [src/core/ModuleManager/doc/README.md](../../core/ModuleManager/doc/README.md)。
