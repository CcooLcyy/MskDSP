# DigitalInput 模块

## 职责

DigitalInput 读取设备板上的 4 路 DI 外部遥信，并将变位事件以 BOOL 点发布到
DataCenter。模块不实现 IEC104、Modbus 等协议转发，协议间路由由 DataCenter
统一配置和执行。

固定端点为 `DigitalInput/board-di`，连接标签如下：

| 标签 | GPIO offset | 业务有效电平 |
| --- | ---: | --- |
| `DI1` | 114 | 物理低电平（短接） |
| `DI2` | 116 | 物理低电平（短接） |
| `DI3` | 113 | 物理低电平（短接） |
| `DI4` | 115 | 物理低电平（短接） |

## 采集语义

- 使用 GPIO character device，优先 GPIO v2 line event；不依赖 `libgpiod`。
- 四路均监听双边沿。GPIO 的物理低电平转换为业务 `bool=true`，物理高电平转换为
  `bool=false`。
- GPIO 内核事件本身带纳秒时间戳；发布到 DataCenter 的 `ts_ms` 为毫秒，时间戳分辨率为
  1 ms，实际通知延迟取决于 Linux 调度和进程负载，不作为硬实时保证。
- 严格 SOE：启动时不读取并发布初值，不周期重发；仅在收到变位事件时发布。
- 同一电平的重复事件不会重复发布。第一条有效边沿事件视为该点进入已知状态并发布。
- 第一版不做触点防抖；如现场触点有抖动，后续在事件处理器中增加可配置防抖窗口。
- GPIO 采集和 DataCenter 发布使用独立线程；发布失败的事件进入最多 256 条的有界队列，
  按 FIFO 顺序重试。队列满时会记录中文错误日志并丢弃新事件。
- 发布失败时会重新执行 `GetOrCreateConnection` 和标签注册；连接恢复后待发事件切换到新的
  `conn_id`，避免 DataCenter 重启后持续使用失效连接。
- GPIO v2 事件序号断档会记录内核 FIFO 可能溢出的错误日志；v1 ABI 没有事件序号，无法检测
  该类丢失。

## DataCenter 约定

模块启动后调用 `GetOrCreateConnection` 注册 `DigitalInput/board-di`，再以
`replace=true` 注册 `DI1` 至 `DI4`。每次边沿调用 `Publish` 发布 BOOL、`QUALITY_GOOD`
和事件时间戳；若内核时间戳无法转换，则传 `ts_ms=0`，由 DataCenter 填充当前时间。
DataCenter 会持久化连接和标签，路由由上位机通过通用 DataBus 页面配置。

## 运行地址

- 内部 gRPC ready 检查：`unix:./socket/DigitalInput.sock`。
- 对外 gRPC：`0.0.0.0:<port>`，端口由 ModuleManager 在 17001 至 17999 范围内分配。
- 模块不提供业务 RPC；上位机通过 ModuleManager 查询运行状态，通过 DataCenter 查询 DI 点值。

## 运行与权限

- 默认设备节点：`/dev/gpiochip0`，可通过 `MSKDSP_DIGITAL_INPUT_GPIOCHIP` 环境变量覆盖。
- 进程需要对 GPIO chip 设备具有读写权限；当前目标设备由 root 运行，设备节点为
  `root:root 0600`。
- 模块没有业务 gRPC/Proto 接口，但会启动无业务方法的通用 gRPC 监听端点，供模块管理器
  做 ready 检查；模块启停和查询仍通过通用 ModuleManager 接口完成。

## 线程与日志

模块采集线程和 SOE 发布线程均使用 `ModuleManager::StartModuleThread` 的日志上下文。
启动、GPIO 请求/释放、边沿事件、事件序号断档、DataCenter 注册、发布成功和发布失败都会写入
中文日志。

## 验收标准

1. 启动 DataCenter 与 DigitalInput 后，DataBus 能看到 `DigitalInput/board-di` 及四个标签。
2. 未短接时不产生启动初值；短接或断开任一路时，DataCenter 收到对应一次 BOOL 变位。
3. 短接发布 `true`，断开发布 `false`，并携带 `QUALITY_GOOD`。
4. 在 DataBus 建立 `DigitalInput/board-di/DIx ->` 任意协议端点的路由后，DI 变位能被目标协议订阅。
5. 停止模块后 GPIO line 被释放，再次启动可以重新申请；无 GPIO 设备时模块记录错误并按间隔重试。

## 构建产物

共享库：`package/module/libDigitalInput.so.<version>`（版本见
`src/DigitalInput/cmake/LibInfo.cmake`）。
