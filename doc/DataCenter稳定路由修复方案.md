# DataCenter 稳定路由修复方案

## 背景

当前 DataCenter 路由持久化只保存 `conn_id + tag`。当设备断电重启后，如果连接注册表没有恢复成功，协议模块会重新注册连接并重新分配 `conn_id`。此时 `routes.pb` 仍保留旧 `conn_id`，但连接注册表已经变成新 ID，导致路由无法匹配，表现为数据总线丢失。

## 目标

- 路由的长期持久化身份改为稳定连接主键：`module_name + conn_name + tag`。
- 兼容现有上位机和旧配置：旧请求仍可只传 `conn_id + tag`。
- DataCenter 对外返回路由时同时返回 `conn_id` 与稳定连接主键，便于上位机逐步切换。
- 旧 `routes.pb` 在连接注册表可用时自动补齐稳定字段并继续运行。

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
  - 旧格式：`conn_id + tag`，由 DataCenter 通过连接注册表转换为稳定端点。
- `Publish/BatchPublish` 仍使用运行时 `conn_id + tag`，DataCenter 先根据当前连接注册表解析到稳定端点，再匹配路由。
- `ListRoutes/DumpRoutesConfig` 输出时填充稳定字段；如果当前连接注册表能解析稳定端点，也填充当前 `conn_id`。
- 如果只有旧格式路由且连接注册表无法解析对应 `conn_id`，加载或下发应失败，避免继续写入语义不明的路由。

## 兼容性

- Protobuf 新字段追加在 `Endpoint` 后面，不改变已有字段编号。
- 旧上位机继续使用 `conn_id + tag` 时，DataCenter 依赖当前连接注册表完成兼容转换。
- 新上位机应优先保存和下发 `module_name + conn_name + tag`，`conn_id` 仅用于展示或当前运行时加速。

## 验收标准

- 连接 ID 重排后，只要连接主键不变，旧稳定路由仍能正确转发。
- 旧 `conn_id` 路由在连接注册表存在时能自动补齐稳定字段。
- `ListRoutes` 返回的每条路由包含稳定字段。
- 路由引用无法解析的旧 `conn_id` 时返回明确错误，不静默当作有效路由。
