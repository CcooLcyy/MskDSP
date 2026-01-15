# 模块管理器（ModuleManager）

模块管理器负责管理模块生命周期，并对外提供 gRPC 管理接口，实现「发现模块 → 动态加载 → 启动/停止 → 运行时查询」的闭环。

## 运行与目录约定
- 推荐以 `package/` 作为工作目录运行 `./MskDSP`。
- `./lib/`：模块共享库目录（默认对应 `package/lib/`）。
- `./conf/`：运行时配置目录（`SaveModuleStartConfig` 会写入 `modConf.bin`，该文件默认被 `.gitignore` 忽略）。
- `./socket/`：模块内部 gRPC 的 unix socket 文件目录。

## 模块加载约定
- 模块以共享库形式交付，库文件名需以 `lib` 开头并包含 `.so`；版本从 `.so.<version>` 段解析。
- 模块库必须导出 `create()` 工厂函数（签名等价于 `ModuleInterface::ModuleInterface* create()`），供管理器通过 Boost.DLL 获取并实例化模块对象。
- 模块对象实现 `start(std::stop_token)`；在 `start()` 中通常会创建并注册自己的 gRPC Service，然后调用 `grpcServerBuilder(service)` 启动服务。

## gRPC 管理接口
协议定义：`protobuf/ModuleManager.proto`，Service：`ModuleManage`。

- `GetModuleInfo`：返回从 `./lib` 扫描到的模块列表（建议以该结果作为 `StartModule/StopModule` 的入参，避免库名/模块名不匹配）。
- `StartModule`：加载共享库、调用 `create()` 创建实例，并在独立线程中运行模块的 `start()`。
- `StopModule`：请求停止模块、关闭 gRPC Server、回收线程并卸载共享库。
- `GetRunningModuleInfo`：返回已启动模块的运行时信息（版本、inner/outer gRPC 地址等）。
- `SaveModuleStartConfig`：保存模块启动配置到 `./conf/modConf.bin`（当前实现仅保存，未看到启动时自动读取逻辑）。

## 上位机对接建议
本项目有配套上位机/配置工具，建议按以下方式对接模块生命周期与地址发现。

### 入口与地址
- 上位机入口：连接模块管理器对外地址 `0.0.0.0:7000`。
- `inner_grpc_server`（unix socket）：用于进程内模块间互联；一般不建议上位机使用。
- `outer_grpc_server`（TCP）：用于上位机调用；普通模块端口为随机 7001–7999，重启后可能变化。

### 推荐调用流程
1. `GetModuleInfo`：发现可用模块（从 `./lib` 扫描）。
2. `StartModule`：启动所需模块（建议包含 `DataCenter` 与各协议模块）。
3. `GetRunningModuleInfo`：获取已启动模块的 `outer_grpc_server`，上位机据此建立到各模块的 gRPC 连接并进行后续配置/运行期调用。

### 稳定性与容错
- 模块重启/Stop 后其 `outer_grpc_server` 可能变化，上位机应在连接失败时重新调用 `GetRunningModuleInfo` 刷新地址并重连。
- `StartModule` 返回后模块线程已启动，但服务就绪可能存在短暂延迟；建议上位机对目标模块做一次健康探测/重试连接后再进入配置流程。

## 端口策略
- 模块管理器对外 gRPC：固定监听 `0.0.0.0:7000`。
- 普通模块对外 gRPC：随机选择 7001–7999（进程内避免冲突）。
- 模块内部 gRPC：使用 `./socket/<模块名>.sock`（实际监听为绝对路径形式的 unix domain socket）。

## 日志（模块标识）
为便于排查多模块并发运行时的日志来源，日志输出增加了「模块名」标识。

- 日志格式增加 `[<模块名>]` 字段；当无法确定模块上下文时显示 `-`。
- 模块名来源为 `ModuleInterface::initLibInfo(...)` 设置的 `LIB_NAME`（对应 `metaData_.name`），模块应在 `grpcServerBuilder(...)` 前完成初始化。
- gRPC 自动标记：通过 `grpcServerBuilder(...)` 启动的 gRPC Server 会统一注入 server interceptor，在每个 RPC 处理线程中自动设置模块名上下文；RPC 处理函数里直接使用 `LOG_INFO/LOG_ERROR...` 即可。
- 非 gRPC 线程：模块自行创建的后台线程需要在入口处手动创建 `ModuleManager::LogModuleScope scope(metaData_.name);`，否则该线程的日志模块名会是 `-`。
- 如模块绕过 `grpcServerBuilder(...)` 自行创建 gRPC Server，需要自行注入拦截器或手动设置 `LogModuleScope`。
