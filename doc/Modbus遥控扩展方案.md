# Modbus 遥控扩展方案

## 目标

补齐 ModbusRTU 的单线圈遥控能力，并允许保持寄存器单写点使用 BOOL 命令。点表配置、DataCenter 路由、同步命令和异步命令订阅均保持现有集成方式。

## 范围

- 新增 `FUNCTION_WRITE_SINGLE_COIL`，对应 Modbus 功能码 `0x05`。
- 暂不新增 `0x0F` 写多线圈；批量线圈点的点表建模另行设计。
- `FUNCTION_WRITE_SINGLE_COIL` 仅允许 `DATA_TYPE_BOOL`，且 `reg_count` 必须为 1。
- `FUNCTION_WRITE_SINGLE_REGISTER` 允许 `DATA_TYPE_BOOL`，且 `reg_count` 必须为 1。
- `0x05` 的 BOOL 编码遵循 Modbus 标准：`true` 编码为 `0xFF00`，`false` 编码为 `0x0000`。
- `0x06` 的 BOOL 编码采用工程约定：`true` 编码为 `0x0001`，`false` 编码为 `0x0000`。
- BOOL 点忽略 `scale/offset/deadband/word_order/byte_order`，不做工程量换算。
- 同步命令和异步 `PointUpdate` 均接受 DataCenter `bool` 值；为兼容现有命令入口，数值值仍可转换为工程量，BOOL 点最终按非零为 true 处理。

## 约束与错误

- `0x05` 地址遵循链路的 `address_base`，有效范围为 0..65535（1 基模式下不允许 0）。
- `0x05` 响应必须校验设备地址、功能码、线圈地址、线圈值和 CRC；异常响应转换为现有命令状态。
- `0x06 + BOOL` 只接受单寄存器写入，不允许 `reg_count=2` 或其他数据类型。
- 写点只能在链路 `RUNNING` 且总线可用时执行；错误状态沿用现有 `ExecuteCommand` 映射。

## 配置与调用链

- Modbus 点表页面增加 `0x05 写单线圈`，并使 `0x06 写单寄存器` 可选择 BOOL。
- ConfigPusher 的 Modbus 十六进制功能码预处理增加 `0x05` 映射。
- 控制源仍通过 DataCenter Route 指向 Modbus 写点。
- 同步场景使用 `DataCenter.ExecuteCommand`；异步场景使用 DataCenter 发布/订阅闭环。
- 不新增 Modbus 私有写入 RPC，也不新增独立的 Modbus `business_type` 字段。

## 测试与验收

- 点表校验：`0x05` 仅 BOOL、`0x06` BOOL 合法、非法类型和寄存器数被拒绝。
- 串口 PTY：验证 `0x05` ON/OFF 请求帧、响应校验和 `0x06` BOOL 的 1/0 请求帧。
- LinkManager：验证同步命令能路由到 `0x05` 和 `0x06 + BOOL`，并返回 `COMMAND_ACCEPTED`。
- ConfigPusher：验证 `0x05` 字符串功能码可转换为新枚举值。
- 上位机：验证功能码与数据类型联动规则允许上述两种配置，并继续拒绝不支持组合。
