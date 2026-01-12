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

## 端口策略
- 模块管理器对外 gRPC：固定监听 `0.0.0.0:7000`。
- 普通模块对外 gRPC：随机选择 7001–7999（进程内避免冲突）。
- 模块内部 gRPC：使用 `./socket/<模块名>.sock`（实际监听为绝对路径形式的 unix domain socket）。
