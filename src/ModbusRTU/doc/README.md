# ModbusRTU 模块

## 简介
ModbusRTU 主站/从站模块：主站基于串口轮询从站点表并发布到 DataCenter；从站在收到请求后按点表回应。
控制面通过 gRPC 由上位机/ConfigPusher 下发配置。

## 能力清单
- 主站读线圈（0x01）与保持寄存器（0x03）
- 从站读线圈（0x01）与保持寄存器（0x03），按点表响应
- 支持同一串口多个从站（串口参数需一致，模式不可混用）
- 点表管理、启停轮询/响应
- 轮询数据发布到 DataCenter（BOOL 原样、UINT16/UINT32 按 scale/offset 转为 double）

## 接口与协议
- Protobuf：`protobuf/ModbusRTU.proto`
- gRPC Service：`ModbusRTUProto::ModbusRTUService`

## 运行与地址
- 对外 gRPC：随机选择 `0.0.0.0:<port>`（7001–7999）
- 内部 gRPC：`unix socket`：`./socket/ModbusRTU.sock`
- 运行时可通过管理器 `GetRunningModuleInfo` 查询实际地址

## 配置与数据
- 配置入口：ConfigPusher `./conf/configPusher/modbus_rtu.jsonc`
- 关键配置：serial.device/baud_rate/data_bits/parity/stop_bits/read_timeout_ms、slave_id、poll_interval_ms、address_base、mode、read_plan（可选）
- 点表字段：tag/function/address/type、reg_count/word_order/byte_order、scale/offset/deadband、default_value（从站兜底）
- function 字段支持枚举名/数字/十六进制字符串（`0x01`/`0x03`）

ConfigPusher 点表示例（含 scale/offset/deadband，字段可省略，默认 scale=1、offset=0、deadband=0；BOOL 忽略这三个字段）：
```jsonc
[
  {
    "tag": "A相电压",
    "function": "0x03",
    "address": 0,
    "type": "DATA_TYPE_UINT16",
    "reg_count": 1,
    "byte_order": "AB",
    "scale": 1.0,
    "offset": 0.0,
    "deadband": 0.0,
    "default_uint16": 2200
  },
  {
    "tag": "总有功功率",
    "function": "0x03",
    "address": 10,
    "type": "DATA_TYPE_UINT32",
    "reg_count": 2,
    "word_order": "HL",
    "byte_order": "AB",
    "scale": 1.0,
    "offset": 0.0,
    "deadband": 0.0,
    "default_uint32": 123456
  }
]
```

## 地址与点表说明
- `address_base=ADDRESS_BASE_ZERO`：点表地址按协议偏移填写（例如保持寄存器 0 对应 40001）。
- `address_base=ADDRESS_BASE_ONE`：点表地址按人类编号填写，模块会在轮询时自动减 1（地址需 >=1）。
- `function/type` 约束：READ_COILS 仅支持 BOOL；READ_HOLDING_REGISTERS 支持 UINT16/UINT32。
- `reg_count`：寄存器数量（0 表示按 type 默认：UINT16=1、UINT32=2）；UINT32 必须为 2；address 为起始寄存器地址。
- `word_order`：32 位拼接字序（HL 高字在前/LH 低字在前），仅对 UINT32 生效，默认 HL（兼容 WORD_ORDER_HL/WORD_ORDER_LH）。
- `byte_order`：16 位字节序（AB 高字节在前/BA 低字节在前），对 UINT16/UINT32 生效，默认 AB（兼容 BYTE_ORDER_AB/BYTE_ORDER_BA）。
- `scale/offset`：工程量换算 `value = raw * scale + offset`（`scale=0` 视为 1）；仅对 UINT16/UINT32 生效。
- `deadband`：工程量单位，仅对 UINT16/UINT32 生效；`|value - last_reported| < deadband` 时不上报，<=0 表示不过滤。
- `deadband` 仅对主站轮询发布生效，从站响应不受 deadband 影响。

## 主站显式抄读方案
当 `mode=LINK_MODE_MASTER` 且配置 `read_plan.mode=READ_PLAN_MODE_EXPLICIT` 时，主站按配置区间抄读寄存器。
未配置 `read_plan` 或 `read_plan.mode=READ_PLAN_MODE_POINT` 时，保持原有逐点抄读逻辑。

示例（显式区间抄读）：
```jsonc
{
  "read_plan": {
    "mode": "READ_PLAN_MODE_EXPLICIT",
    "blocks": [
      { "function": "0x03", "start": 0, "quantity": 10 },
      { "function": "0x03", "start": 100, "quantity": 20 }
    ]
  }
}
```

注意事项：
- `blocks` 的 `start/quantity` 遵循 `address_base`；区间范围为 `[start, start + quantity - 1]`。
- 当前仅支持功能码 `0x03`（读保持寄存器）；单次最大数量为 125。
- `UINT32` 点位需要覆盖连续 2 个寄存器；区间未覆盖的点不会被抄读，会在日志中提示。

## 从站说明
- `mode=LINK_MODE_SLAVE` 时，模块作为从站响应请求（支持 0x01/0x03）。
- 点值来源：优先读取 DataCenter 最新值并按 `scale/offset` 反向缩放；若无值或 DataCenter 不可用则使用点表 `default_value`（如 default_uint16/default_uint32）兜底。
- 广播地址（slave_id=0）不应答；不支持的功能码返回异常。

## 上位机对接建议
- 配置流程：`UpsertLink(create_only=true)` → `UpsertPointTable` → `StartLink`。
- 同串口多从站：不同 `conn_name` + 不同 `slave_id`，其余串口参数必须一致。
- 读出数据将发布到 DataCenter，点值类型分别映射为 bool/double（UINT16/UINT32 经 scale/offset）。

## 报文日志
- 模块内部线程统一使用 `ModuleManager::StartModuleThread(模块LibInfo.LIB_NAME, ...)` 创建，自动绑定日志模块名上下文。
- 报文日志为 INFO 级别，逐帧输出完整 RTU 帧（包含发送/接收、设备、长度、数据）。
- 报文日志输出量大，生产环境需关注日志大小与磁盘占用。

## 常见错误码
- `INVALID_ARGUMENT`：参数缺失、address_base 与点表地址不匹配、点表类型不合法。
- `ALREADY_EXISTS`：`conn_name` 已存在或点表冲突映射。
- `FAILED_PRECONDITION`：链路状态不允许操作、串口参数冲突。

## 构建产物
- 共享库：`package/module/libModbusRTU.so.<version>`（版本见 `src/ModbusRTU/include/ModbusRTULibInfo.h`）
