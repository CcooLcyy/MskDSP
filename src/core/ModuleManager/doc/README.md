# 模块管理器（ModuleManager）

模块管理器负责管理模块生命周期，并对外提供 gRPC 管理接口，实现「发现模块 → 动态加载 → 启动/停止 → 运行时查询」的闭环。

## 运行与目录约定
- 推荐以 `package/` 作为工作目录运行 `./MskDSP`。
- `./module/`：模块共享库目录（默认对应 `package/module/`）。
- `./conf/`：运行时配置目录（`SaveModuleStartConfig` 会写入 `modConf.bin`，该文件默认被 `.gitignore` 忽略）。
- `./socket/`：模块内部 gRPC 的 unix socket 文件目录。

## 模块加载约定
- 模块以共享库形式交付，库文件名需以 `lib` 开头并包含 `.so`；版本以模块 manifest 为准（`.so.<version>` 仅用于日志对照）。
- 模块库必须导出 `create()` 工厂函数（签名等价于 `ModuleInterface::ModuleInterface* create()`），供管理器通过 Boost.DLL 获取并实例化模块对象。
- 模块需导出 `GetModuleManifestPb`（无副作用），返回 `ModuleManagerProto::ModuleManifest` 的序列化内容，用于声明模块名、版本与依赖。
- 模块对象实现 `start(std::stop_token)`；在 `start()` 中通常会创建并注册自己的 gRPC Service，然后调用 `grpcServerBuilder(service)` 启动服务。

## 模块依赖与 Manifest
- manifest 字段：`module_name`、`version`、`dependencies`（依赖模块名 + 版本约束）。
- 版本约束表达式支持 `= >= > <= <`，可用空格分隔多个条件（示例：`=0.0.1`、`>=1.2.0 <2.0.0`）；`==` 不支持，解析失败将标记为 `manifest_error`。
- `GetModuleInfo` 返回的 `manifest_error` 非空时表示模块不可用，`StartModule` 将直接失败并输出日志。
- `StartModule` 会自动按依赖拓扑顺序启动依赖模块；`StopModule` 会级联停止依赖它的上游模块。
- ModuleManager 启动时扫描 `./module` 并构建依赖图；依赖缺失/循环/版本不满足会在日志中给出清晰错误。

Manifest 导出示例（无副作用，返回 protobuf 序列化数据）：
```cpp
extern "C" BOOST_SYMBOL_EXPORT bool GetModuleManifestPb(const uint8_t **data, size_t *size) {
  if (data == nullptr || size == nullptr) {
    return false;
  }
  const auto &serialized = GetSerializedManifest();
  *data = reinterpret_cast<const uint8_t *>(serialized.data());
  *size = serialized.size();
  return true;
}
```

## 启动自加载配置
- 配置文件：`./conf/module_manager.jsonc`（可选；缺失则跳过）。
- 字段：
  - `boot_config_mode`：启动配置模式，支持 `CONFIG_PUSHER` 与 `UPPER`
  - `auto_start_modules`（字符串数组，模块名需与 `lib<模块名>.so` 对应）
- 行为：
  - 管理器在进程启动时只读取一次 `boot_config_mode`，并固定为本次运行模式；运行中修改配置文件不会立即生效，需重启 `MskDSP`
  - `CONFIG_PUSHER`：允许 `ConfigPusher` 在启动后读取 JSONC 并下发配置
  - 管理器启动后会先读取 `auto_start_modules` 并自动加载列表中的模块；已运行的模块会安全跳过
  - `UPPER`：即使 `auto_start_modules` 中包含 `ConfigPusher`，也会跳过其自动启动；若后续手动启动 `ConfigPusher`，模块仅启动服务，不会执行配置下发
  - `UPPER`：在处理完 `auto_start_modules` 后，管理器还会继续检查各模块配置目录下是否存在持久化配置主文件或 `.bak` 备份文件；只要发现文件痕迹，就会自动启动对应模块
  - `UPPER`：管理器只做“文件是否存在”的判断，不会预解析各模块的 pb 内容；pb 合法性、对象恢复以及模块内功能是否能自动进入运行态，统一由模块自身负责
  - `UPPER`：`auto_start_modules` 与“文件痕迹自动启动”可以同时存在，重复启动会复用现有“已运行则跳过”逻辑，避免双重启动
  - 若配置文件读取失败、JSONC 解析失败、根节点不是对象，或 `boot_config_mode` 填写非法，管理器会按安全模式回退为 `UPPER`；此时忽略无法解析的 `auto_start_modules`，但仍继续执行“文件痕迹自动启动”
- 建议：使用 `CONFIG_PUSHER` 时，自启动列表仅填写 `ConfigPusher`；使用 `UPPER` 时，不要把 `ConfigPusher` 作为启动时配置入口，而应由模块根据持久化配置痕迹自行恢复与启动。

### `UPPER` 模式文件痕迹清单
- `DataCenter`
  - `./conf/dataCenter/connections.pb`
  - `./conf/dataCenter/connections.pb.bak`
  - `./conf/dataCenter/conn_tags.pb`
  - `./conf/dataCenter/conn_tags.pb.bak`
  - `./conf/dataCenter/routes.pb`
  - `./conf/dataCenter/routes.pb.bak`
  - `./conf/dataCenter/point_tables.pb`
  - `./conf/dataCenter/point_tables.pb.bak`
    说明：这是 `conn_tags.pb` 的历史兼容文件名；若现场仍保留旧文件痕迹，`UPPER` 模式也会自动启动 `DataCenter`
- `IEC104`
  - `./conf/IEC104/links.pb`
  - `./conf/IEC104/links.pb.bak`
  - `./conf/IEC104/point_tables.pb`
  - `./conf/IEC104/point_tables.pb.bak`
- `ModbusRTU`
  - `./conf/ModbusRTU/mqtt.pb`
  - `./conf/ModbusRTU/mqtt.pb.bak`
  - `./conf/ModbusRTU/links.pb`
  - `./conf/ModbusRTU/links.pb.bak`
  - `./conf/ModbusRTU/point_tables.pb`
  - `./conf/ModbusRTU/point_tables.pb.bak`
- `DLT645`
  - `./conf/DLT645/mqtt.pb`
  - `./conf/DLT645/mqtt.pb.bak`
  - `./conf/DLT645/links.pb`
  - `./conf/DLT645/links.pb.bak`
  - `./conf/DLT645/point_tables.pb`
  - `./conf/DLT645/point_tables.pb.bak`
- `AGC`
  - `./conf/AGC/groups.pb`
  - `./conf/AGC/groups.pb.bak`
- `AVC`
  - `./conf/AVC/groups.pb`
  - `./conf/AVC/groups.pb.bak`
- `Calc`
  - `./conf/Calc/groups.pb`
  - `./conf/Calc/groups.pb.bak`

示例：
```jsonc
{
  // 启动配置模式：仅在进程启动时读取一次；修改后需重启 MskDSP 生效。
  "boot_config_mode": "UPPER",
  // 显式自动启动模块列表；与 UPPER 模式的“文件痕迹自动启动”可同时存在。
  "auto_start_modules": ["DataCenter"]
}
```

## gRPC 管理接口
协议定义：`protobuf/ModuleManager.proto`，Service：`ModuleManage`。

- `GetModuleInfo`：返回从 `./module` 扫描到的模块列表（建议以该结果作为 `StartModule/StopModule` 的入参，避免库名/模块名不匹配），并携带依赖信息与 `manifest_error`。
- `StartModule`：加载共享库、调用 `create()` 创建实例，并在独立线程中运行模块的 `start()`；自动按依赖拓扑顺序启动依赖模块；依赖缺失/循环/版本不满足返回错误。
- `StopModule`：请求停止模块、关闭 gRPC Server、回收线程并卸载共享库；级联停止依赖它的上游模块；级联失败返回错误。
- `GetRunningModuleInfo`：返回已启动模块的运行时信息（版本、inner/outer gRPC 地址等）。
- `SaveModuleStartConfig`：保存模块启动配置到 `./conf/modConf.bin`（当前实现仅保存，未看到启动时自动读取逻辑）。
- `UploadModule`/`DeleteModule`：上传/删除模块当前未实现（no-op），优先级较低，后续再补齐。

## 地址发现与容错说明
涉及上位机页面结构、菜单组织与完整操作流程的统一说明，见 `doc/上位机设计指导.md`。本节仅保留模块管理器对接时必须关注的事实性约束。

### 入口与地址
- 上位机入口：连接模块管理器对外地址 `0.0.0.0:17000`。
- `inner_grpc_server`（unix socket）：用于进程内模块间互联；一般不建议上位机使用。
- `outer_grpc_server`（TCP）：用于上位机调用；普通模块端口为随机 17001–17999，重启后可能变化。

### 稳定性与容错
- 模块重启/Stop 后其 `outer_grpc_server` 可能变化，上位机应在连接失败时重新调用 `GetRunningModuleInfo` 刷新地址并重连。
- `StartModule` 返回后模块线程已启动，但服务就绪可能存在短暂延迟；建议上位机对目标模块做一次健康探测/重试连接后再进入配置流程。
- 若启用了 `./conf/module_manager.jsonc` 的自启动列表，或在 `UPPER` 模式下命中了持久化配置文件痕迹自动启动，可跳过 `StartModule`，直接通过 `GetRunningModuleInfo` 获取已启动模块信息。

## 端口策略
- 模块管理器对外 gRPC：固定监听 `0.0.0.0:17000`。
- 普通模块对外 gRPC：随机选择 17001–17999（进程内避免冲突）。
- 模块内部 gRPC：使用 `./socket/<模块名>.sock`（实际监听为绝对路径形式的 unix domain socket）。

## 测试
本仓库在 `test/` 中提供了针对模块管理器与基础设施的单元测试用例：

- `moduleManager_test`：覆盖模块扫描（`./module`）、manifest/版本解析、依赖启动与 load/unload 生命周期、运行时信息查询、启动配置落盘、gRPC 管理服务的委派逻辑。
- `moduleInterface_test`：覆盖 `ModuleInterface::initLibInfo/grpcServerBuilder/shutdownServers` 等基础设施逻辑，并通过一次真实 RPC 触发 interceptor 路径。
- `logger_test`：覆盖模块日志分目录、追加写入、历史日志压缩与 60 天保留策略。

运行方式：
```bash
ctest --test-dir build -R moduleManager_test --output-on-failure
ctest --test-dir build -R moduleInterface_test --output-on-failure
ctest --test-dir build -R logger_test --output-on-failure
```

说明：
- 测试会使用隔离的工作目录（`build/test_env/...`）避免污染 `package/` 运行目录与本地配置。
- `moduleManager_test` 会构建一个用于测试的“最小假模块”共享库，并放在其隔离工作目录的 `./module/` 下，以覆盖动态加载路径。

## 日志（模块标识）
为便于排查多模块并发运行时的日志来源，日志输出增加了「模块名」标识。

- 日志格式增加 `[<模块名>]` 字段；当无法确定模块上下文时归入 `moduleManager`。
- 模块名来源为 `ModuleInterface::initLibInfo(...)` 设置的 `LIB_NAME`（对应 `metaData_.name`），模块应在 `grpcServerBuilder(...)` 前完成初始化。
- gRPC 自动标记：通过 `grpcServerBuilder(...)` 启动的 gRPC Server 会统一注入 server interceptor，在每个 RPC 处理线程中自动设置模块名上下文；RPC 处理函数里直接使用 `LOG_INFO/LOG_ERROR...` 即可。
- 非 gRPC 线程：统一使用 `ModuleManager::StartModuleThread(模块LibInfo.LIB_NAME, ...)` 创建线程，封装内部会自动设置 `LogModuleScope`，无需在入口手动创建。
- 如模块绕过 `grpcServerBuilder(...)` 自行创建 gRPC Server，需要自行注入拦截器或通过 `StartModuleThread` 包装线程入口。

## 日志（轮转与保留）
- 日志文件：`./log/<模块名>/<模块名>.log`（追加写入，重启不会清空）
- 每日 00:00 轮转；轮转后的文件名形如 `./log/<模块名>/<模块名>_YYYY-MM-DD_HH-MM-SS_N.log.gz`
- 历史日志自动压缩为 `.gz`，默认保留最近 60 天
- 需要调整策略时修改 `src/core/ModuleManager/Logger.cc`（`kRotationSizeBytes` / `kRetentionDays`）
