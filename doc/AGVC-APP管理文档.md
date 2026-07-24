# AGVC-APP管理文档

## 1. 文档说明

本文档用于说明 AGVC-APP 的部署与运行管理方式。本文中的 AGVC-APP 指当前项目对外提供的主程序。

当前版本重点说明安装包的使用方法、执行后的文件布局以及运行行为。后续如有补充或调整，统一在本文档中维护。

## 2. 安装包概述

AGVC-APP 提供安装包，用于在目标机器上准备运行所需文件，并启动 AGVC-APP 主程序。

安装包为单个可执行文件。目标机器执行该安装包后，即可完成程序运行准备并启动 AGVC-APP。

## 3. 安装包使用方法

部署交付后，使用者拿到的是一个可直接执行的安装包文件，文件名格式为 `mskdsp-<version>`。

### 3.1 启动 AGVC-APP

```bash
./mskdsp-<version> start
```

执行后，AGVC-APP 会进入运行状态，并使用 `/data/mskdsp/` 下的配置、运行文件和日志目录。

### 3.2 停止 AGVC-APP

```bash
./mskdsp-<version> stop
```

执行后，当前 AGVC-APP 运行实例会停止，但 `/data/mskdsp/` 下已保留的配置文件、运行文件和日志文件不会被删除。

### 3.3 容器名称与版本检查

AGVC-APP 运行时使用的容器名称固定为：

```text
mskdsp
```

如需查看当前正在运行的容器名称及版本，可执行：

```bash
docker ps --filter name=mskdsp --format '{{.Names}} {{.Image}}'
```

输出示例：

```text
mskdsp mskdsp:1.2.3
```

说明如下：

- 前半部分 `mskdsp` 为容器名称。
- 后半部分 `mskdsp:1.2.3` 为当前运行版本。
- 容器版本与 AGVC-APP 版本不进行区分，两者视为同一版本号。
- 若命令无输出，表示当前没有正在运行的 AGVC-APP 实例。

## 4. 执行后的文件布局

安装包执行后，宿主机侧运行目录为：

```text
/data/mskdsp/
```

该目录下会使用以下子目录：

```text
/data/mskdsp/conf/
/data/mskdsp/module/
/data/mskdsp/log/
```

目录用途如下：

- `conf/`：保存运行配置。
- `module/`：保存程序运行所需的扩展文件。
- `log/`：保存运行日志。

### 4.1 映射目录及常见文件说明

`/data/mskdsp/conf/` 为配置目录，常见内容如下：

- `module_manager.jsonc`：程序启动相关配置文件。
- `configPusher/*.jsonc`：各类业务配置文件。
- `config.db`：程序运行过程中生成的 SQLite 配置数据库。

`/data/mskdsp/module/` 为程序运行文件目录，常见内容如下：

- `libAGC.so.*`：AGC 相关运行文件。
- `libAVC.so.*`：AVC 相关运行文件。
- `libCalc.so.*`：数值计算相关运行文件。
- `libConfigPusher.so.*`：配置下发相关运行文件。
- `libDLT645.so.*`：DLT645 通信相关运行文件。
- `libDataCenter.so.*`：数据存储与转发相关运行文件。
- `libIEC104.so.*`：IEC104 通信相关运行文件。
- `libMQTTManager.so.*`：MQTT 相关运行文件。
- `libModbusRTU.so.*`：Modbus RTU 通信相关运行文件。

`/data/mskdsp/log/` 为日志目录，用于保存程序运行期间生成的日志文件。

## 5. 安装包执行行为

### 5.1 执行 `start` 时的主要流程

执行 `start` 后，安装包会完成以下动作：

1. 检查并准备 `/data/mskdsp/` 下的运行目录。
2. 检查宿主机 `module/` 目录：
   - 若安装包内容有更新，则按安装包内容同步最新运行文件。
   - 若宿主机 `module/` 为空，则自动初始化运行文件。
3. 检查宿主机 `conf/` 目录：
   - 若 `conf/` 不存在或为空，则初始化默认配置。
   - 若宿主机已有配置，则保留 `config.db` 及其他业务配置。
   - 无论是否已有现场配置，都会从当前安装包同步 `module_manager.jsonc`，确保安装版本对应的模块启动策略生效。
4. 检查宿主机 `log/` 目录，不存在时自动创建。
5. 停止旧的 AGVC-APP 运行实例。
6. 启动新的 AGVC-APP 运行实例。

### 5.2 执行 `stop` 时的行为

执行 `stop` 后，安装包会停止当前 AGVC-APP 运行实例。

该操作不会删除以下内容：

- 宿主机 `/data/mskdsp/conf/`
- 宿主机 `/data/mskdsp/module/`
- 宿主机 `/data/mskdsp/log/`

## 6. 使用注意事项

使用时还需注意以下事项：

- 后续执行 `start` 时只会刷新 `conf/module_manager.jsonc`；`conf/config.db` 及其他现场业务配置不会被覆盖。
- 宿主机 `module/` 目录会在安装包版本更新后重新同步，应确保部署内容与安装包版本一致。
- 执行 `stop` 仅停止当前运行实例，不会清理已保留的配置、扩展文件和日志。

## 7. 运行日志说明

> **重要说明：运行日志建议优先交由开发人员进行问题定位，不建议测试人员自行依据运行日志直接判断问题根因；如测试人员需要自行进行初步排查，可参照以下日志说明。**

`/data/mskdsp/log/` 下的运行日志主要分为两类：

- `RTU.log`：汇总日志，用于统一查看程序整体运行情况。
- 按功能分类的日志目录及日志文件：用于查看某一类功能的详细运行信息。

常见日志内容包括：

- 程序启动、停止、重启等运行状态信息。
- 配置加载、配置更新、配置初始化等处理结果。
- 通信连接建立、断开、异常、重连等过程信息。
- 请求处理结果、告警信息、错误原因等问题定位信息。
- 部分通信场景下的收发报文内容或报文摘要。

日志内容通常采用以下格式：

```text
YYYY-MM-DD HH:MM:SS [级别] [来源] 日志内容
```

示例如下：

```text
2026-03-18 10:23:45 [info] [AGC] 启动控制组成功: group_name=group_1
2026-03-18 10:23:48 [warning] [IEC104] 连接断开: conn_name=main_link
2026-03-18 10:23:52 [error] [ModbusRTU] 读取数据失败: conn_name=meter_1, reason=超时
```

其中：

- 时间字段表示日志产生时间。
- 级别字段常见为 `info`、`warning`、`error`。
- 来源字段用于标识日志来源，便于快速定位问题范围。
- 日志内容字段用于描述本次事件的具体信息。

日志文件会持续追加写入，程序重启后不会清空已有日志。历史日志会按日期轮转保存，并自动压缩为 `.gz` 文件，便于长期留存和问题追溯。
