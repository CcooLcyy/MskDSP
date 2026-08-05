# 设备模拟器

本目录目前包含 DLT645PCD 串口模拟器和 IEC 61850 MMS 基础模拟 IED。两者都是独立进程，
仅用于联调和反向验证，不进入 MskDSP 主模块发布链路。

## IEC 61850 MMS 基础模拟 IED

### 工具定位

`iec61850_mms_sim` 作为 TCP 服务端模拟下方 IED，使用项目内 `IEC61850` 库的自研
ISO-on-TCP、COTP、Session、Presentation、ACSE 和 MMS 编解码。它用于验证
MskDSP MMS 客户端跨进程的建链、目录请求、基础 Read 和基础 Write。

`iec61850_mms_worker_smoke` 是生产 `MmsSessionWorker` 的最小命令行联调客户端，
用于等待模拟 IED 的 `READY` 状态。它支持空模型以及最小 URCB/数据集模型，
可以验证 RCB 三阶段 Write、首次 GI，以及 GI 报告中的 RCB/DataSet、ConfRev、序号、
点值、品质和 TimeOfEntry。

当前工具明确不模拟生产 IED 的完整能力。它只提供基础目录、有限的
`BOOLEAN/INT32/INT32U/FLOAT32/FLOAT64/QUALITY/TIMESTAMP` 类型、RCB/GI 和基础
InformationReport；不提供厂商控制模型、普通周期报告、报告分段、断线接管、
CommandTermination 或 GOOSE/SV 数据面。这些能力仍需通过协议层测试夹具和真实 IED 联调验证。

### 构建

```bash
cmake -S . -B build -DMSKDSP_BUILD_TOOLS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target iec61850_mms_sim
cmake --build build --target iec61850_mms_worker_smoke
```

### 启动

```bash
./build/tools/iec61850_mms_sim \
  --listen-ip 127.0.0.1 \
  --port 2102 \
  --ied IED1 \
  --access-point AP1 \
  --domain IED1LD0 \
  --variable 'LLN0$Beh$stVal' \
  --dataset 'LLN0$ds1' \
  --rcb \
  --once
```

`--port 0` 仍可让系统分配临时端口，但客户端必须使用日志中打印的实际端口。`--once`
在处理一个客户端会话后退出，适合脚本联调。可重复使用 `--variable` 和 `--dataset`
添加目录项；使用 `--rcb --type INT32` 等参数可以切换最小模型的数据类型。

启动模拟器后，在另一个终端运行生产工作器 smoke 客户端：

```bash
./build/tools/iec61850_mms_worker_smoke --ip 127.0.0.1 --port 2102
```

验证最小 URCB、三阶段 Write 和 GI 后的 `READY`：

```bash
./build/tools/iec61850_mms_worker_smoke \
  --ip 127.0.0.1 --port 2102 --rcb --timeout-ms 15000
```

日志使用中文，包含连接阶段、收发方向和完整十六进制报文。MMS 模拟器不需要 raw socket
权限，只使用 TCP 监听端口。

## DLT645PCD 串口模拟器

## 工具定位

该工具位于 `tools/device-sim/`，用于联调阶段模拟 DLT645PCD 从站，反向验证主站程序的串口通信可靠性。

- 运行形态：独立进程。
- 通信方式：仅串口（通过 PTY 伪串口）。
- 当前能力：按配置中的 `read_map` 返回固定值。

## 当前支持范围

- 支持协议：`DLT645PCD`。
- 支持请求：读请求（控制码 `0x11`）。
- 支持应答：
  - 正常应答（控制码 `0x91`）。
  - 异常应答（控制码 `0xD1`，错误码 1 字节）。
- 支持多实例：单进程内可同时创建多个模拟从站实例，每个实例一个 PTY 链路。

## 配置说明

配置示例见：`tools/device-sim/conf/dlt645_pcd_sim.jsonc`。

根字段：

- `instances`: 实例数组。

实例字段：

- `name`: 实例名称（日志标识）。
- `meter_addr`: 表地址，12 位十六进制字符串。
- `device_no`: 设备号，2 位十六进制字符串。
- `pty_link`: 对外暴露的伪串口软链接路径。
- `read_map`: 读请求映射数组。

实例约束：

- `name` 在进程内必须唯一。
- `pty_link` 在进程内必须唯一。
- `meter_addr + device_no` 组合在进程内必须唯一。

`read_map` 字段：

- `di`: DI，8 位十六进制字符串（按人读顺序填写）。
- `data_hex`: 固定回包数据（十六进制字符串，偶数长度）。

## 运行方式

先开启顶层工具构建开关，再运行模拟器进程：

```bash
cmake -S . -B build -DMSKDSP_BUILD_TOOLS=ON
cmake --build build --target dlt645_pcd_sim
./build/tools/dlt645_pcd_sim --config tools/device-sim/conf/dlt645_pcd_sim.jsonc
```

运行后会创建 `pty_link` 对应软链接，主站程序可直接打开该串口路径进行收发。

## 日志说明

工具会输出中文日志，包含：

- 实例名称。
- 收发方向（接收/发送）。
- 十六进制报文。
- 请求处理结果（正常应答/异常应答原因）。
