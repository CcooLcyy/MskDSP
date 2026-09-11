# DeviceInfo 运行指标模块（首期）

## 目标与范围

首期只采集网关本机的整体 CPU 使用率和内存使用率，并通过 DataCenter 发布，供上位机总览及后续告警/计算/协议路由复用。首期不采集温度、磁盘、网卡清单等其他设备信息，不新增独立设备信息 gRPC 协议。

## 数据接口

模块启动后向 DataCenter 注册稳定连接：

- `module_name=DeviceInfo`
- `conn_name=device-runtime`

连接标签固定为：

- `cpu.usage_percent`：`double`，范围 0～100，单位 `%`
- `memory.usage_percent`：`double`，范围 0～100，单位 `%`

模块不创建路由；是否转发至 IEC104、Modbus 或其他消费者由上位机配置 DataCenter 路由决定。

## 采集规则

- 模块功能启动后立即读取一次；之后默认每 5 秒采集一次。
- 每轮采集通过一次 `BatchPublish` 发布两个指标，使用采集完成时的毫秒时间戳。
- CPU 使用率根据 `/proc/stat` 的两次总 CPU 累计值增量计算：
  `Δ(user+nice+system+irq+softirq+steal) / Δ(total) × 100`。
  第一次采集仅建立基线，不发布 CPU 点；计数器回退或总增量为 0 时本轮 CPU 结果无效。
- 内存使用率根据 `/proc/meminfo` 的 `MemTotal` 和 `MemAvailable`（单位 kB）计算：
  `(MemTotal-MemAvailable) / MemTotal × 100`。

## 异常与质量

- `/proc` 文件缺失、字段缺失、数字格式错误或数值越界时，记录中文错误日志，采集线程继续运行。
- CPU 尚无基线或本轮计算无效时，不伪造 CPU 数值；内存无效时同样不发布无效值。
- DataCenter 发布失败只影响当前轮次，并记录错误；下一轮继续尝试。
- 有效指标发布使用 `QUALITY_GOOD`。首期不发布无效值，因此不使用 `QUALITY_UNCERTAIN/BAD` 覆盖历史值。

## 上位机展示

系统总览增加 CPU、内存占用率卡片，调用现有 DataCenter `ListConnections`、`GetConnTags` 和 `GetSourceLatest` 读取 `DeviceInfo/device-runtime` 源端最新值。卡片显示百分比、数据时间和质量；连接或指标不可用时显示“暂不可用”。浏览器适配器提供确定性的演示值。

## 验收标准

1. 纯解析测试覆盖 CPU 首次采样、正常增量、计数器回退/零增量，以及内存正常值、缺字段、非法数字、总内存为零。
2. DataCenter 客户端测试验证连接/标签注册和批量发布的 tag、类型、质量、时间戳。
3. 采集线程在文件读取失败或 DataCenter RPC 失败后仍可继续下一轮。
4. 上位机总览在真实适配器和浏览器 mock 下均能显示两个指标，并区分指标缺失与首页其他数据加载失败。

