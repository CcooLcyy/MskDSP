# DataCenter 稳定路由修复方案

> 历史背景文档：本文记录的是稳定路由字段设计过程。当前实现已进一步切换为 `./conf/config.db` 中的 `DataCenter/state`，不再使用旧 `.pb/.bak/.tmp` 配置文件。

## 背景

旧 DataCenter 路由持久化只保存 `conn_id + tag`。当设备断电重启后，如果连接注册表没有恢复成功，协议模块会重新注册连接并重新分配 `conn_id`。此时旧 `routes.pb` 仍保留旧 `conn_id`，但连接注册表已经变成新 ID，导致路由无法匹配，表现为数据总线丢失。

## 目标

- 路由的长期持久化身份改为稳定连接主键：`module_name + conn_name + tag`。
- 当前 RPC 仍允许只传 `conn_id + tag`，但 DataCenter 会立即按当前连接注册表转换为稳定端点；持久化不再以 `conn_id` 作为主键。
- DataCenter 对外返回路由时同时返回 `conn_id` 与稳定连接主键，便于上位机逐步切换。
- 废除旧的 DataCenter 三文件持久化格式，不再迁移旧 `connections.pb/conn_tags.pb/routes.pb`。
- 连接注册表、连接标签注册表与路由合并为 SQLite 配置项 `DataCenter/state` 落盘，避免三类配置来自不同代快照。

## 设计

`Endpoint` 新增字段：

- `module_name`：连接所属模块名。
- `conn_name`：模块内连接名。

DataCenter 内部路由索引使用稳定端点：

```text
module_name + conn_name + tag
```

处理规则：

- `UpsertRoutes/DeleteRoutes` 支持两种输入：
  - 新格式：`module_name + conn_name + tag`。
  - 当前运行态格式：`conn_id + tag`，由 DataCenter 通过连接注册表转换为稳定端点。
- `Publish/BatchPublish` 仍使用运行时 `conn_id + tag`，DataCenter 先根据当前连接注册表解析到稳定端点，再匹配路由。
- `ListRoutes/DumpRoutesConfig` 输出时填充稳定字段；如果当前连接注册表能解析稳定端点，也填充当前 `conn_id`。
- 如果路由端点的稳定连接主键或 `conn_id` 无法按当前连接注册表解析，下发应失败，避免继续写入语义不明的路由。

## 兼容性

- Protobuf 新字段追加在 `Endpoint` 后面，不改变已有字段编号。
- 旧持久化文件不再迁移；现场应重新通过配置流程生成 SQLite 配置项 `DataCenter/state`。
- 上位机应保存和下发 `module_name + conn_name + tag`，`conn_id` 仅用于展示、过滤或当前运行态调用。

## 验收标准

- 连接 ID 重排后，只要连接主键不变，已保存的稳定路由仍能正确转发。
- `conn_id + tag` 路由请求在连接注册表存在时能自动归一化为稳定字段。
- `ListRoutes` 返回的每条路由包含稳定字段。
- 路由引用无法解析的稳定连接主键或 `conn_id` 时返回明确错误，不静默当作有效路由。
