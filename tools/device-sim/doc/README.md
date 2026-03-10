# DLT645PCD 串口模拟器

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
