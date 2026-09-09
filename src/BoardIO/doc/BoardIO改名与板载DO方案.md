# BoardIO 改名与板载 DO 方案

## 目标与范围

现有 `DigitalInput` 模块直接改名为 `BoardIO`，统一管理设备板上的固定 GPIO：

- 保留 4 路 DI 外部遥信采集。
- 新增 DO1、DO2 两个 BOOL 遥控点。
- 新增失电继电器生命周期控制。
- 使用现有 DataCenter 路由和通用 `CommandExecutor` 完成同步遥控，不新增模块专用 Proto。

不保留旧模块名、旧共享库或旧 DataCenter 稳定端点。升级部署必须移除旧的
`libDigitalInput.so*`；模块首次启动时通过 `RenameConnection` 将已有
`DigitalInput/board-di` 自动迁移为 `BoardIO/board-di`，保留 `conn_id` 和关联路由。

## GPIO 与端点

遥信固定端点为 `BoardIO/board-di`：

| 标签 | GPIO offset | 业务有效电平 |
| --- | ---: | --- |
| `DI1` | 114 | 物理低电平（短接） |
| `DI2` | 116 | 物理低电平（短接） |
| `DI3` | 113 | 物理低电平（短接） |
| `DI4` | 115 | 物理低电平（短接） |

遥控固定端点为 `BoardIO/board-do`：

| 标签 | GPIO offset | `false` | `true` |
| --- | ---: | --- | --- |
| `DO1` | 256 | 输出低电平 | 输出高电平 |
| `DO2` | 39 | 输出低电平 | 输出高电平 |

GPIO257 为失电继电器控制，不注册为 DataCenter 点位，也不接受外部遥控。模块成功申请
输出 GPIO 后将 GPIO257 置为高电平，使继电器自动吸合；模块停止时主动置为低电平并释放
GPIO，整机掉电或重启后继电器同样回到释放状态。

## 启停语义

- 模块启动时以一组 GPIO 请求原子设置初值：DO1=false、DO2=false、失电继电器=true，
  避免 DO 在初始化期间短暂误动作。
- 输出 GPIO 申请失败时不接受 DO 命令，记录中文错误日志并按间隔重试；只有输出 GPIO
  准备完成后才允许返回遥控成功。
- DI 采集和 DataCenter 注册失败不会主动释放已经吸合的失电继电器；失电继电器表达
  BoardIO 模块仍在运行并持有输出 GPIO，不表达外部通信链路健康状态。
- 模块停止时先停止接受新命令，再等待在途命令结束，随后将 DO1、DO2 和失电继电器
  全部置为低电平并释放 GPIO。

## DI 采集语义

- 使用 GPIO character device，优先 GPIO v2 line event；不依赖 `libgpiod`。
- 四路均监听双边沿。GPIO 的物理低电平转换为业务 `bool=true`，物理高电平转换为
  `bool=false`。
- 严格 SOE：启动时不读取并发布初值，不周期重发；仅在收到变位事件时发布。
- 同一电平的重复事件不会重复发布。第一条有效边沿事件视为该点进入已知状态并发布。
- GPIO 采集和 DataCenter 发布使用独立线程；发布失败的事件进入最多 256 条的有界队列，
  按 FIFO 顺序重试。

## DO 遥控语义

- BoardIO 实现 DataCenter 通用 `CommandExecutor.ExecuteCommand`。
- DataCenter 将唯一同步命令路由转发到 `BoardIO/board-do/DO1` 或 `DO2`；目标点只接受
  BOOL 值。
- 模块校验目的模块名、连接名、连接 ID、点名和值类型。目标不存在、类型错误或连接信息
  不一致时明确拒绝，不操作 GPIO。
- GPIO 写入成功后返回 `COMMAND_ACCEPTED`；写入失败或输出尚未准备完成时返回
  `COMMAND_TARGET_UNAVAILABLE`，不能先确认再操作硬件。
- 成功响应回填完整稳定目的端点、请求值和接受值，供 DataCenter 保存已接受命令快照。
- DO1、DO2 不跨模块重启保持；每次模块重新申请输出 GPIO 时均恢复为低电平。

## DataCenter 约定

模块启动后分别注册 `BoardIO/board-di` 和 `BoardIO/board-do`。`board-di` 以
`replace=true` 注册 DI1 至 DI4，`board-do` 以 `replace=true` 注册 DO1、DO2。DI 边沿通过
`Publish` 发布 BOOL、`QUALITY_GOOD` 和事件时间戳；DO 通过同步命令路由执行。
旧 `DigitalInput/board-di` 不存在时直接创建新遥信端点；若新旧端点已同时存在，则保留
现有 `BoardIO/board-di` 并记录迁移冲突警告，避免覆盖已投入使用的新端点。

## 运行地址与权限

- 内部 gRPC：`unix:./socket/BoardIO.sock`，同时承载 ready 检查和通用命令执行服务。
- 默认设备节点：`/dev/gpiochip1`，可通过 `MSKDSP_BOARD_IO_GPIOCHIP` 环境变量覆盖。
- 进程需要对 GPIO chip 设备具有读写权限。

## 异常场景

- GPIO 输出申请失败：保持命令服务不可用状态并重试，不影响模块管理器继续停止模块。
- GPIO 写入失败：返回目标不可用并记录 GPIO、点名、目标值和系统错误。
- DataCenter 重启：重新注册 DI/DO 两个连接；待发 DI 事件切换到新的 DI `conn_id`。
- 端点自愈每 5 秒查询连接和标签，仅在标签缺失或变化时执行 `replace=true` 注册，避免周期性
  重复落盘。
- 目的连接名称与 `conn_id` 不一致：拒绝命令，不操作 GPIO。
- 模块停止与命令并发：停止接收新命令并等待在途命令结束后再释放 GPIO。

## 验收标准

1. 启动 DataCenter 与 BoardIO 后，DataBus 能看到 `BoardIO/board-di` 的 DI1 至 DI4，
   以及 `BoardIO/board-do` 的 DO1、DO2。
2. 模块启动后 DO1(GPIO256) 与 DO2(GPIO39) 为低电平，失电继电器(GPIO257)为高电平并吸合。
3. 向 DO1、DO2 下发 BOOL 同步命令时，对应 GPIO 输出一致；非 BOOL、未知点或错误连接被拒绝。
4. GPIO 写入失败时命令不得返回成功；输出未准备完成时返回目标不可用。
5. DI 现有 SOE、低电平有效和失败重试语义保持不变。
6. 模块停止时 DO1、DO2 和 GPIO257 均输出低电平，失电继电器释放；再次启动可重新申请。
7. 升级包只包含 `libBoardIO.so*`，目标安装目录不残留 `libDigitalInput.so*`。

## 上位机建议

上位机继续通过 DataBus 页面配置路由，不增加 BoardIO 专用配置页。DI 路由选择
`BoardIO/board-di/DIx`，遥控路由的目的端点选择 `BoardIO/board-do/DO1` 或 `DO2`。
