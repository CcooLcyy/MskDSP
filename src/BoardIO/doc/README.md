# BoardIO 模块

## 职责

BoardIO 统一管理设备板固定 GPIO：采集 4 路 DI 遥信，执行 DO1、DO2 两路 BOOL 遥控，
并在模块内控制失电继电器。协议间路由仍由 DataCenter 统一配置和执行。

遥信端点为 `BoardIO/board-di`：

| 标签 | GPIO offset | 业务有效电平 |
| --- | ---: | --- |
| `DI1` | 114 | 物理低电平（短接） |
| `DI2` | 116 | 物理低电平（短接） |
| `DI3` | 113 | 物理低电平（短接） |
| `DI4` | 115 | 物理低电平（短接） |

遥控端点为 `BoardIO/board-do`：

| 标签 | GPIO offset | `false` | `true` |
| --- | ---: | --- | --- |
| `DO1` | 256 | 低电平 | 高电平 |
| `DO2` | 39 | 低电平 | 高电平 |

GPIO257 为模块内部失电继电器控制，不注册为遥控点。模块内输出功能成功启动后自动输出高电平，
使继电器吸合；停止输出功能时恢复低电平并释放 GPIO。

## 启停语义

- 输出驱动一次申请 GPIO256、GPIO39、GPIO257，并原子设置初值 `0/0/1`。
- 输出 GPIO 申请失败时按间隔重试；DI 和 DataCenter 的故障不会释放已吸合的失电继电器。
- 模块停止时先拒绝新遥控并等待在途写入，再依次拉低 GPIO256、GPIO39、GPIO257，最后释放 line。
- DO 状态不跨模块重启保持。模块或整机重启后，DO1、DO2 从低电平开始。

## DI 采集语义

- 使用 GPIO character device，优先 GPIO v2 line event，并兼容 v1；不依赖 `libgpiod`。
- 四路均监听双边沿，物理低电平转换为业务 `bool=true`，物理高电平转换为 `bool=false`。
- 严格 SOE：不发布启动初值、不周期重发，仅在收到变位事件时发布。
- 同一电平的重复事件不会重复发布；第一条有效边沿视为该点进入已知状态并发布。
- GPIO 采集和 DataCenter 发布使用独立线程。发布失败事件进入最多 256 条的 FIFO 队列并重试。
- GPIO v2 事件序号断档会记录内核 FIFO 可能溢出的错误日志；v1 无法检测该类丢失。

## DO 遥控语义

BoardIO 实现 DataCenter 通用 `CommandExecutor.ExecuteCommand`。命令目的端点必须完整匹配
`BoardIO/board-do/<DO1|DO2>` 及当前 `conn_id`，且值类型必须为 BOOL。GPIO 写入成功后返回
`COMMAND_ACCEPTED`；输出尚未准备或写入失败时返回 `COMMAND_TARGET_UNAVAILABLE`；连接、点名或类型
不合法时返回 `COMMAND_REJECTED`。GPIO257 不接受外部命令。

## DataCenter 约定

模块以 `replace=true` 分别注册 `BoardIO/board-di` 的 DI1 至 DI4，以及 `BoardIO/board-do` 的
DO1、DO2。升级后首次启动会尝试通过 `RenameConnection` 将旧 `DigitalInput/board-di` 迁移为
`BoardIO/board-di`，保留 `conn_id` 并由 DataCenter 改写关联路由；旧端点不存在时直接创建新端点。
模块每 5 秒校验一次 DI/DO 连接和标签，仅在缺失、变化或 DataCenter 重启后重新注册，避免
周期性重复落盘。

DI 边沿通过 `Publish` 发布 BOOL、`QUALITY_GOOD` 和事件时间戳。DO 通过同步命令路由执行，
不通过普通发布路径控制。

## 运行地址与权限

- 内部 gRPC：`unix:./socket/BoardIO.sock`，同时承载 ready 检查和同步遥控服务。
- 对外 gRPC：`0.0.0.0:<port>`，端口由 ModuleManager 在 17001 至 17999 范围内分配。
- 默认 GPIO 设备为 `/dev/gpiochip1`，可通过 `MSKDSP_BOARD_IO_GPIOCHIP` 环境变量覆盖。
- 进程需要对 GPIO chip 设备具有读写权限。

## 验收标准

1. DataBus 能看到 `BoardIO/board-di` 的 DI1 至 DI4，以及 `BoardIO/board-do` 的 DO1、DO2。
2. 模块内输出功能启动后 GPIO256=0、GPIO39=0、GPIO257=1，失电继电器自动吸合。
3. DO1、DO2 的 BOOL 命令正确写入 GPIO256、GPIO39；错误点、错误连接和非 BOOL 命令不操作 GPIO。
4. DI 保持低电平有效、严格 SOE 和失败重试语义。
5. 模块停止时三路输出均拉低，失电继电器释放；再次启动后能够重新申请 GPIO。

## 构建产物

共享库为 `package/module/libBoardIO.so.<version>`，版本见 `src/BoardIO/cmake/LibInfo.cmake`。
安装时会精确清理旧的 `package/module/libDigitalInput.so*`，避免新旧模块同时被 ModuleManager 加载。
