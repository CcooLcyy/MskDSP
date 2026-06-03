# SQLite 配置持久化设计

## 目标

- 模块间 RPC 与控制面消息继续使用 protobuf。
- 运行时关键配置统一持久化到 `./conf/config.db`。
- 不再读取或写入旧配置文件；缺少 SQLite 配置项时等待重新下发。
- 上位机导出文件继续保持二进制 `.mskcfg`，用于降低现场可读性。

## 阶段

1. 新增公共 SQLite 配置库，提供 protobuf payload 的事务化保存与 checksum 校验。
2. IEC104、DLT645、ModbusRTU 的链路、点表与 MQTT 配置切到 SQLite 读写。
3. DataCenter 完整状态、ModuleManager 启动配置、AGC/AVC/Calc 分组配置全部切到 SQLite。

## 语义约束

- SQLite 中的 row 存在表示显式配置存在；payload 可以是 0 字节，以支持显式空 protobuf 配置。
- 启动恢复失败不能把空内存反写为目标配置。
- DataCenter 未 ready 时，依赖模块只能保留目标配置并等待重试，不能删除或覆盖持久化配置。
- `conn_id` 只作为运行期展示/兼容字段，路由与连接标签必须保留稳定连接主键。

## 恢复策略

启动加载时只查 SQLite；若 SQLite 不存在对应配置项，则返回空配置并等待上位机或 ConfigPusher 重新下发。后续 Save 只写 SQLite。

## 当前配置项

- `DataCenter/state`
- `IEC104/links`
- `IEC104/point_tables`
- `ModbusRTU/mqtt`
- `ModbusRTU/links`
- `ModbusRTU/point_tables`
- `DLT645/mqtt`
- `DLT645/links`
- `DLT645/point_tables`
- `AGC/groups`
- `AVC/groups`
- `Calc/groups`
- `ModuleManager/module_start_config`
