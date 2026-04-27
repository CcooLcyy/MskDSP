# MskDSP

## 项目简介
MskDSP 是一个基于 C++23 的模块化系统：核心由 `ModuleManager` 管理模块生命周期，并通过 gRPC 对外提供服务；具体业务能力以共享库模块的形式接入。

## 模块机制概览
- 模块以共享库形式接入：构建产物输出到 `package/module/`，运行时由模块管理器从工作目录的 `./module` 扫描发现并动态加载。
- 加载约定：模块库需要导出 `create()` 工厂函数，返回 `ModuleInterface::ModuleInterface*`（示例：`src/DataCenter/DataCenter.cc`、`src/IEC104/IEC104.cc`）。
- 依赖约定：模块需导出 `GetModuleManifestPb`，返回 `ModuleManagerProto::ModuleManifest`（模块名/版本/依赖）；ModuleManager 启动时构建依赖图，`StartModule` 会自动拉起依赖。
- 服务约定：每个模块基于 `ModuleInterface` 同时启动两套 gRPC Server：内部 `unix socket`（`./socket/<模块名>.sock`）+ 对外 `0.0.0.0:<端口>`（默认范围 17001–17999）。
- 线程与日志：模块内部线程统一使用 `ModuleManager::StartModuleThread(模块LibInfo.LIB_NAME, ...)` 创建，自动绑定日志模块名上下文。
- 管理端口：模块管理器对外 gRPC 固定监听 `0.0.0.0:17000`。

## 开发指南

### 现有模块目录
- [src/DataCenter/](./src/DataCenter/)：跨协议数据总线/数据转发枢纽（以逻辑点名/tag 进行对齐，[文档](./src/DataCenter/doc/README.md)）
- [src/IEC104/](./src/IEC104/)：IEC 60870-5-104 协议相关能力（示例模块，[文档](./src/IEC104/doc/README.md)）
- [src/DLT645/](./src/DLT645/)：DLT645 协议相关能力（示例模块，[文档](./src/DLT645/doc/README.md)）
- [src/ModbusRTU/](./src/ModbusRTU/)：ModbusRTU 协议相关能力（示例模块，[文档](./src/ModbusRTU/doc/README.md)）
- [src/AGVC/AGC/](./src/AGVC/AGC/)：AGC 自动功率控制（总设定拆分/派生点计算，通过 DataCenter 路由与上下游联动，[文档](./src/AGVC/AGC/doc/README.md)）
- [src/AGVC/AVC/](./src/AGVC/AVC/)：AVC 自动电压控制（集中式电压-无功控制、默认点与控制组管理，[文档](./src/AGVC/AVC/doc/README.md)）
- [src/Calc/](./src/Calc/)：Calc 类型化运算模块（分组化四则/逻辑运算，通过 DataCenter Route 接入已有点，[文档](./src/Calc/doc/README.md)）
- [src/ConfigPusher/](./src/ConfigPusher/)：配置编排与初始化下发能力（[文档](./src/ConfigPusher/doc/README.md)）
- [src/MQTTManager/](./src/MQTTManager/)：MQTT 通道管理与协议适配能力

### 新增模块
新增模块可以参考现有示例 `src/DataCenter/`、`src/IEC104/`、`src/DLT645/` 与 `src/ModbusRTU/`，核心步骤如下：

可使用脚手架快速生成骨架：
```bash
bash script/new_module.sh <NewModule>
```

1. 新建目录：在 `src/` 下创建 `src/<NewModule>/`（建议包含 `include/`、`cmake/` 子目录）。
2. 实现模块类：
   - 继承 `ModuleInterface::ModuleInterface`，实现 `start(std::stop_token)`。
   - 在构造函数中调用 `initLibInfo(<LibInfo变量>)` 初始化模块元信息（库名/版本/端口等）。
   - 在 `start()` 中创建并注册 gRPC Service，然后调用 `grpcServerBuilder(service)` 启动服务。
3. 导出工厂函数：提供 `extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface* create()` 供管理器动态加载。
4. 导出 manifest：提供 `extern "C" BOOST_SYMBOL_EXPORT bool GetModuleManifestPb(const uint8_t **data, size_t *size)`，填充模块名/版本/依赖与版本约束（示例见 `src/ModbusRTU/ModbusRTU.cc`）。
5. 配置 CMake（参考 `src/DataCenter/CMakeLists.txt`）：
   - 在 `src/<NewModule>/cmake/LibInfo.cmake` 中设置 `LIB_NAME` 与版本号。
   - `configure_file(${CMAKE_SOURCE_DIR}/cmake/LibInfo.h.in ...)` 生成 `<LIB_NAME>LibInfo.h` 供 `initLibInfo()` 使用。
   - `add_library(${LIB_NAME} SHARED ...)` 并链接 `moduleManager`，设置 `VERSION/SOVERSION` 以生成形如 `lib<name>.so.<version>` 的文件名。
   - 在 `src/CMakeLists.txt` 中 `add_subdirectory(<NewModule>)` 参与构建。
6. 构建后，将生成的 `.so` 放在运行时工作目录的 `./module`（默认即 `package/module/`），通过 gRPC 调用 `StartModule` 启动模块。

脚手架补充内容说明（与当前规范保持一致）：
- 自动生成 `GetModuleManifestPb` 模板与版本信息（依赖默认留空）。
- `protobuf/*.proto` 生成 `service/rpc` 注释，便于对接与校验。
- 模块模板内置中文日志（模块启动/停止、Ping 请求）。
- 模块文档骨架包含“线程与日志”约定说明。

## 模块依赖与启动
ModuleManager 使用模块 manifest 构建依赖图，并自动处理依赖启动/级联停止。

涉及上位机菜单结构、页面职责与操作流程的统一设计说明，已收口到 `doc/上位机设计指导.md`。

## 目录结构
- `src/`：核心与模块源码（`core/ModuleManager`、`DataCenter`、`IEC104` 等）
- `protobuf/`：Protobuf/gRPC 协议定义与生成代码（通过 `dspProto` 目标链接）
- `3rdlibs/`：第三方依赖子工程
- `test/`：单元测试（GTest）
- `script/`：辅助脚本
- `package/`：构建输出目录（可执行文件、共享库、配置等）

## 构建与测试
依赖通过 CMake + vcpkg manifest 管理（见 `vcpkg.json` / `vcpkg-configuration.json`）。

### 开发容器（推荐）
推荐使用长期驻留的开发容器 `mskdsp-dev` 承担依赖安装与配置动作，Codex 仍直接在 WSL 仓库路径中读写代码。

```bash
# 1) 构建开发镜像
docker build -f .devcontainer/Dockerfile -t mskdsp-dev:ubuntu24.04 .

# 2) 准备卷
docker volume create mskdsp-vcpkg-root
docker volume create mskdsp-vcpkg-cache
docker volume create mskdsp-ccache

# 3) 启动长期驻留容器
docker run -d --name mskdsp-dev \
  -w /data/code/mskdsp \
  -v /home/code/share:/data \
  -v mskdsp-vcpkg-root:/data/3rdlibs/vcpkg \
  -v mskdsp-vcpkg-cache:/root/.cache/vcpkg \
  -v mskdsp-ccache:/root/.cache/ccache \
  mskdsp-dev:ubuntu24.04

# 4) 进入容器
docker exec -it mskdsp-dev bash
```

进入容器后，建议串行执行：

```bash
cmake --preset x64
cmake --preset arm64
```

说明：

- `/data/3rdlibs/vcpkg` 会由容器专用卷接管，不再复用宿主机同路径内容
- 容器启动时会自动按 `vcpkg-configuration.json` 的 baseline 校验或重建 `vcpkg`
- 首次初始化后会在容器内构建一个本地修补版 `vcpkg-tool`，规避 WSL/Docker 环境下已复现的 `metrics.cpp` 负耗时崩溃
- 进入容器 shell 时会自动清理基础镜像继承的全局 `CC/CXX`，避免污染 `arm64` 交叉编译
- VSCode 默认使用 `Attach to Running Container` 连接 `mskdsp-dev`，不要把 `Reopen in Container` 作为主流程
- 详细说明见 [doc/开发容器.md](./doc/开发容器.md)

### 构建
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build --parallel
```

### 运行
构建产物默认输出到 `package/`。程序使用相对路径访问 `./module`、`./conf`、`./socket`，因此推荐以 `package/` 作为工作目录运行：
```bash
cd package
./MskDSP
```

## Docker 打包与运行
本项目提供根目录 `Dockerfile`，用于将 `package/` 下的运行产物打包成运行镜像（仅拷贝 `MskDSP`、`module/`、`lib/` 与 `conf/`，同目录的 `*_test` 不会进入镜像）。

1) 先构建生成 `package/` 产物（交叉编译或本机编译均可）。
2) 构建镜像（arm64）：
```bash
docker build --platform=linux/arm64 -t mskdsp:arm64 .
```
3) 运行（使用 host 网络，不需要端口映射）：
```bash
mkdir -p ./log
docker run --network host --rm --log-driver none \
  -v "$(pwd)/log:/opt/mskdsp/log" \
  mskdsp:arm64
```

注意：`Dockerfile` 基于 `localhost/arm64v8/ubuntu:noble`，确保该基础镜像已存在或按需调整镜像名。

说明：
- 建议 Docker 场景使用 `--log-driver none`，直接关闭 Docker 对容器标准输出/标准错误的日志收集，避免生成 `/var/lib/docker/containers/<id>/<id>-json.log`。
- 使用该参数后，`docker logs` 不再提供输出；请先将宿主机当前目录的 `./log` 挂载到容器内 `/opt/mskdsp/log`，正式运行日志可直接查看宿主机 `./log` 目录。

### 启动自加载配置
可选配置文件：`./conf/module_manager.jsonc`（JSONC，支持注释），用于控制 ModuleManager 启动时的模块自加载行为。

- 关键字段：
  - `boot_config_mode`
    - `CONFIG_PUSHER`：允许 `ConfigPusher` 在启动后读取 JSONC 并执行配置下发
    - `UPPER`：`ModuleManager` 会按持久化配置文件痕迹自动启动对应模块，`ConfigPusher` 若被手动启动则仅提供服务、不执行配置下发
  - `auto_start_modules`：显式自动加载模块列表
- 模板：`package/conf/module_manager.jsonc`
- 建议：使用 `CONFIG_PUSHER` 时，自启动列表仅填写 `ConfigPusher`
- 更完整的模式说明、回退语义与 `UPPER` 文件痕迹清单，见 `src/core/ModuleManager/doc/README.md`

示例：
```jsonc
{
  // 启动配置模式：允许 ConfigPusher 在启动后执行配置下发。
  "boot_config_mode": "CONFIG_PUSHER",
  // ModuleManager 启动时自动加载的模块列表。
  "auto_start_modules": ["ConfigPusher"]
}
```

### 运行测试
```bash
ctest --test-dir build --output-on-failure
```
如未开启测试构建（`MSKDSP_BUILD_TESTS=OFF`），则不会生成/执行任何测试用例。

### 测试覆盖率
开启 `MSKDSP_BUILD_TESTS` 后会构建测试并启用覆盖率编译选项（要求使用 GCC/Clang 编译，并安装 `gcovr`）。默认值：`Debug=ON`，其他构建类型为 `OFF`。覆盖率报告需要显式构建 `coverage` 目标生成；普通 `cmake --build ...` 不会自动生成覆盖率报告。报告默认仅统计本项目 `src/` 下的实现文件与模板实现（`.cc/.cpp/.c/.hpp`），不包含纯声明头文件（`.h`）。
```bash
cmake -S . -B build-cov -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DMSKDSP_BUILD_TESTS=ON
cmake --build build-cov --parallel
cmake --build build-cov --target coverage
```
报告输出：`build-cov/coverage/index.html`
如本机存在多个 GCC 版本，可通过 `-DGCOV_EXECUTABLE=/usr/bin/gcov-<版本>` 指定与编译器匹配的 `gcov`；`gcovr` 路径可用 `-DGCOVR_EXECUTABLE=/path/to/gcovr` 指定。

#### 常见问题
- `MSKDSP_BUILD_TESTS=ON` 只会构建测试并启用覆盖率编译选项；如需生成覆盖率报告，请显式执行 `cmake --build <build-dir> --target coverage`。
- 若遇到 `GCOV returncode was 3`，通常是 `gcov` 版本与编译器不匹配；用 `-DGCOV_EXECUTABLE=/usr/bin/gcov-<gcc主版本>` 指定即可。
- 若遇到 `AssertionError: Got function ... on multiple lines`，通常是同一源文件被多个 target 编译（例如库 + 测试）；需要在 gcovr 使用函数合并策略（当前工程已在 coverage 目标中内置）。
- 若遇到 `Cannot open source file ...` 且路径指向已不存在的源码（例如模块目录改名后），说明 build 目录残留旧对象文件；建议清理 build 目录后重新配置/编译。
- 若遇到 `ccache ... Permission denied`，可设置 `CCACHE_DIR` 到可写目录，或在 CMakePresets 中移除/置空 `CMAKE_*_COMPILER_LAUNCHER`。

## 构建产物
- `package/MskDSP`：主程序（Windows 下可能为 `MskDSP.exe`）
- `package/module/`：模块共享库
- `package/lib/`：运行时依赖共享库
- `package/conf/`：配置与生成文件（如运行时保存的 `modConf.bin`）

## 开发约定
统一规范见 `AGENTS.md`（项目结构、编码风格、开发方式、测试与提交/PR 约定等）。
其中新增功能、业务需求与缺陷修复默认遵循“文档 -> 测试 -> 实现”的闭环推进方式。

## 相关文档
- [模块管理器说明](./src/core/ModuleManager/doc/README.md)
- [集成测试工具说明](./tools/README.md)
- [开发容器说明](./doc/开发容器.md)
- [工作机连接与 APP 更新](./doc/工作机连接与APP更新.md)
