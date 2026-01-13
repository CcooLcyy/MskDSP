# DataCenter 模块

## 定位与边界
DataCenter 是进程内的“数据总线/转发枢纽”，用于在不同协议连接之间转发数据（例如 Modbus 抄表 → IEC104 上报），以实现跨协议的数据互通。

## 能力清单
- 连接级隔离：通过 `connId` 区分不同连接/通信链路
- 点位对齐：通过 `tag`（逻辑点名，可中文）对齐跨协议的同一业务量
- 有向路由：按点位维度配置 `src -> dst` 的转发规则，支持一对一与一对多
- 最新值缓存：可用于订阅端启动时拉取最新值（当前规划）
- 点表配置持久化：将点表配置落盘到 `./conf/dataCenter/point_tables.pb`，重启后自动恢复（当前实现）
- 路由配置持久化：将路由配置落盘到 `./conf/dataCenter/routes.pb`，重启后自动恢复（当前实现）

不在当前范围内：
- 历史数据存储/查询
- 聚合/判据/计算点（例如“多个遥信判断一个遥信状态”建议由独立规则/计算模块实现，订阅原始点后发布派生点）
- 多对一仲裁（多个源同时写入同一目的点时暂不保证行为）

## 关键概念
- `connId`：连接的全局唯一 ID（建议使用无符号整型），由各协议模块配置产生；要求重启后保持不变。
- `tag`：逻辑点名（UTF-8，可使用中文），用于跨协议对齐同一业务量；协议地址（IOA/寄存器等）应由各协议模块自身点表维护。
- `Endpoint`：`(connId, tag)`，表示某连接中的一个逻辑点。
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
DataCenter 对外提供一组面向“连接/点表/路由/转发”的 gRPC 接口，用于支撑跨协议数据转发闭环。

### 术语
- `Endpoint`：`(connId, tag)`；`tag` 为连接内的逻辑点名（UTF-8，可中文）。
- `Route`：有向绑定 `src Endpoint -> dst Endpoint`；允许 `srcTag != dstTag`，转发时会按目的端点重写为 `dstTag`。
- `PointUpdate`：路由后的更新，包含 `src/dst` 端点信息与 `value/ts_ms/quality`。

### 连接/点表（用于展示与校验，可选）
- `UpsertConnection(UpsertConnectionRequest) -> Empty`
  - 注册/更新连接信息（`conn_id` 必填且全局唯一；同一 `conn_id` 后写覆盖前写）。
- `ListConnections(Empty) -> ListConnectionsResponse`
  - 列出已注册连接信息（用于 UI/配置工具展示）。
- `UpsertPointTable(UpsertPointTableRequest) -> Empty`
  - 注册/更新某连接点表（`replace=true` 全量覆盖；否则增量追加）。
  - 说明：该 RPC 成功后会触发点表配置落盘；若落盘失败会返回 `INTERNAL`（内存中的点表不会回滚）。
- `GetPointTable(GetPointTableRequest) -> PointTable`
  - 获取某连接点表；若未注册返回 `NOT_FOUND`。
  - 说明：点表并非硬依赖；但当点表存在时，`UpsertRoutes` 会校验 `tag` 必须在点表中，避免配置错误。

### 路由管理（有向绑定）
- `UpsertRoutes(UpsertRoutesRequest) -> Empty`
  - 配置路由；`replace=true` 会清空后重新写入。
  - 支持一对一与一对多；多对一暂不保证行为（建议避免）。
  - 说明：该 RPC 成功后会触发路由配置落盘；若落盘失败会返回 `INTERNAL`（内存中的路由不会回滚）。
- `DeleteRoutes(DeleteRoutesRequest) -> Empty`
  - 删除指定路由绑定。
  - 说明：该 RPC 成功后会触发路由配置落盘；若落盘失败会返回 `INTERNAL`（内存中的路由不会回滚）。
- `ListRoutes(ListRoutesRequest) -> ListRoutesResponse`
  - 查询路由；支持按 `src/dst` 的 `conn_id/tag` 进行可选过滤。

### 数据转发（核心）
- `Publish(PublishRequest) -> Empty`
  - 源连接发布单点数据：`(conn_id, tag, value[, ts_ms][, quality])`。
  - DataCenter 按路由生成 `PointUpdate`，推送给目的连接的订阅者，并更新目的端点的“最新值缓存”。
  - `ts_ms<=0` 时由 DataCenter 填充当前毫秒时间戳。
- `BatchPublish(BatchPublishRequest) -> Empty`
  - 批量发布（减少 RPC 次数）；原子语义：会先校验全部点，若任一参数错误则整批失败且不产生任何更新（不更新最新值缓存/不推送订阅更新）。
- `GetLatest(GetLatestRequest) -> GetLatestResponse`
  - 拉取目的连接内的最新值（best-effort，仅最新值，不含历史）；`tags` 为空表示拉取该连接全部已缓存点。
- `Subscribe(SubscribeRequest) -> stream PointUpdate`
  - 目的连接订阅数据更新（服务端流）；`tags` 为空表示订阅该连接全部点。
  - `snapshot=true` 时，会先 best-effort 推送一次 `GetLatest` 的结果，再推送实时更新。
  - 说明：订阅端消费过慢时，服务端会丢弃过旧消息以避免无限堆积（best-effort，不保证每条更新都可达）。

## 配置与使用流程（建议）
1. 在各协议模块（IEC104/Modbus/DLT645/…）中为每条连接配置 `connId`（全局唯一）并完成点表配置；为每个点配置 `tag`（逻辑点名，可中文）。
2. 在 DataCenter 中配置路由方向（有向绑定）：基于源/目的两侧点表中已知的 `connId + tag` 建立 `src -> dst` 规则（支持一对多）。
3. 启动模块：通过管理器启动 DataCenter 与各协议模块；采集端向 DataCenter 发布数据，上送端订阅/获取对应 `tag` 的更新进行上报。

## 点表配置持久化（当前实现）
DataCenter 会将点表配置落盘到工作目录下的 `./conf/dataCenter/point_tables.pb`，用于进程重启后的自动恢复。

### 文件与策略
- 主文件：`./conf/dataCenter/point_tables.pb`
- 备份文件：`./conf/dataCenter/point_tables.pb.bak`（上一份“可解析且校验通过”的配置）
- 临时文件：`./conf/dataCenter/point_tables.pb.tmp`（用于安全写入，避免写坏主文件）
- 隔离文件：`./conf/dataCenter/point_tables.pb.corrupt.<timestamp>`（当发现主文件损坏时保留原件，便于排查）

### 保存时机与语义
- 每次 `UpsertPointTable` 成功后自动落盘。
- 落盘失败会返回 `INTERNAL`，但内存中的点表不会回滚（配置端可重试）。

### 启动恢复
- 若主文件存在且可用：加载主文件。
- 若主文件损坏：尝试使用备份文件启动；若备份可用，会将损坏主文件隔离，并用备份内容恢复出新的主文件（best-effort）。
- 若主/备均不可用：以空点表启动，等待重新下发。

## 路由配置持久化（当前实现）
DataCenter 会将路由配置落盘到工作目录下的 `./conf/dataCenter/routes.pb`，用于进程重启后的自动恢复。

### 文件与策略
- 主文件：`./conf/dataCenter/routes.pb`
- 备份文件：`./conf/dataCenter/routes.pb.bak`（上一份“可解析且校验通过”的配置）
- 临时文件：`./conf/dataCenter/routes.pb.tmp`（用于安全写入，避免写坏主文件）
- 隔离文件：`./conf/dataCenter/routes.pb.corrupt.<timestamp>`（当发现主文件损坏时保留原件，便于排查）

### 保存时机与语义
- 每次 `UpsertRoutes` / `DeleteRoutes` 成功后自动落盘。
- 落盘失败会返回 `INTERNAL`，但内存中的路由不会回滚（配置端可重试）。

### 启动恢复
- 若主文件存在且可用：加载主文件。
- 若主文件损坏：尝试使用备份文件启动；若备份可用，会将损坏主文件隔离，并用备份内容恢复出新的主文件（best-effort）。
- 若主/备均不可用：以空路由启动，等待重新下发。
- 若路由配置可解析但与已加载点表校验冲突：记录日志并不应用该路由配置。

## 其他内容
运行时工作目录、socket 目录、端口策略与构建产物路径遵循项目通用约定，详见 [README.md](../../../README.md) 与 [src/core/ModuleManager/doc/README.md](../../core/ModuleManager/doc/README.md)。
