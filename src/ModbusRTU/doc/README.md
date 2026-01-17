# ModbusRTU 模块

## 简介
ModbusRTU 主站/从站模块：主站基于串口轮询从站点表并发布到 DataCenter；从站在收到请求后按点表回应。
控制面通过 gRPC 由上位机/ConfigPusher 下发配置。

## 能力清单
- 主站读线圈（0x01）与保持寄存器（0x03）
- 从站读线圈（0x01），按点表响应
- 支持同一串口多个从站（串口参数需一致，模式不可混用）
- 点表管理、启停轮询/响应
- 轮询数据发布到 DataCenter（bool/uint16）

## 接口与协议
- Protobuf：`protobuf/ModbusRTU.proto`
- gRPC Service：`ModbusRTUProto::ModbusRTUService`

## 运行与地址
- 对外 gRPC：随机选择 `0.0.0.0:<port>`（7001–7999）
- 内部 gRPC：`unix socket`：`./socket/ModbusRTU.sock`
- 运行时可通过管理器 `GetRunningModuleInfo` 查询实际地址

## 配置与数据
- 配置入口：ConfigPusher `./conf/configPusher/modbus_rtu.jsonc`
- 关键配置：serial.device/baud_rate/data_bits/parity/stop_bits/read_timeout_ms、slave_id、poll_interval_ms、address_base、mode
- 点表字段：tag/function/address/type、default_value（从站兜底），scale/offset 预留但当前不生效

## 地址与点表说明
- `address_base=ADDRESS_BASE_ZERO`：点表地址按协议偏移填写（例如保持寄存器 0 对应 40001）。
- `address_base=ADDRESS_BASE_ONE`：点表地址按人类编号填写，模块会在轮询时自动减 1（地址需 >=1）。
- `function/type` 约束：READ_COILS 仅支持 BOOL；READ_HOLDING_REGISTERS 仅支持 UINT16。

## 从站说明
- `mode=LINK_MODE_SLAVE` 时，模块作为从站响应请求（当前仅支持 0x01）。
- 点值来源：优先读取 DataCenter 最新值；若无值或 DataCenter 不可用则使用点表 `default_value` 兜底。
- 广播地址（slave_id=0）不应答；不支持的功能码返回异常。

## 上位机对接建议
- 配置流程：`UpsertLink(create_only=true)` → `UpsertPointTable` → `StartLink`。
- 同串口多从站：不同 `conn_name` + 不同 `slave_id`，其余串口参数必须一致。
- 读出数据将发布到 DataCenter，点值类型分别映射为 bool/int64（uint16）。

## 常见错误码
- `INVALID_ARGUMENT`：参数缺失、address_base 与点表地址不匹配、点表类型不合法。
- `ALREADY_EXISTS`：`conn_name` 已存在或点表冲突映射。
- `FAILED_PRECONDITION`：链路状态不允许操作、串口参数冲突。

## 构建产物
- 共享库：`package/lib/libModbusRTU.so.<version>`（版本见 `src/ModbusRTU/include/ModbusRTULibInfo.h`）
