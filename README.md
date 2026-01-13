# MskDSP

## 项目简介
MskDSP 是一个基于 C++23 的模块化系统：核心由 `ModuleManager` 管理模块生命周期，并通过 gRPC 对外提供服务；具体业务能力以共享库模块的形式接入。

## 模块机制概览
- 模块以共享库形式接入：构建产物输出到 `package/lib/`，运行时由模块管理器从工作目录的 `./lib` 扫描发现并动态加载。
- 加载约定：模块库需要导出 `create()` 工厂函数，返回 `ModuleInterface::ModuleInterface*`（示例：`src/DataCenter/DataCenter.cc`、`src/IEC104/IEC104.cc`）。
- 服务约定：每个模块基于 `ModuleInterface` 同时启动两套 gRPC Server：内部 `unix socket`（`./socket/<模块名>.sock`）+ 对外 `0.0.0.0:<端口>`（默认范围 7001–7999）。
- 管理端口：模块管理器对外 gRPC 固定监听 `0.0.0.0:7000`。

## 开发指南

### 现有模块目录
- [src/DataCenter/](./src/DataCenter/)：跨协议数据总线/数据转发枢纽（以逻辑点名/tag 进行对齐，[文档](./src/DataCenter/doc/README.md)）
- [src/IEC104/](./src/IEC104/)：IEC 60870-5-104 协议相关能力（示例模块，[文档](./src/IEC104/doc/README.md)）
- [src/DLT645/](./src/DLT645/)：DLT645 协议相关能力（示例模块，[文档](./src/DLT645/doc/README.md)）
- [src/Modbus/](./src/Modbus/)：Modbus 协议相关能力（示例模块，[文档](./src/Modbus/doc/README.md)）

### 新增模块
新增模块可以参考现有示例 `src/DataCenter/`、`src/IEC104/`、`src/DLT645/` 与 `src/Modbus/`，核心步骤如下：

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
4. 配置 CMake（参考 `src/DataCenter/CMakeLists.txt`）：
   - 在 `src/<NewModule>/cmake/LibInfo.cmake` 中设置 `LIB_NAME` 与版本号。
   - `configure_file(${CMAKE_SOURCE_DIR}/cmake/LibInfo.h.in ...)` 生成 `<LIB_NAME>LibInfo.h` 供 `initLibInfo()` 使用。
   - `add_library(${LIB_NAME} SHARED ...)` 并链接 `moduleManager`，设置 `VERSION/SOVERSION` 以生成形如 `lib<name>.so.<version>` 的文件名。
   - 在 `src/CMakeLists.txt` 中 `add_subdirectory(<NewModule>)` 参与构建。
5. 构建后，将生成的 `.so` 放在运行时工作目录的 `./lib`（默认即 `package/lib/`），通过 gRPC 调用 `StartModule` 启动模块。

## 需求（持续更新）
该部分用于沉淀/迭代项目需求（你可以在此持续补充）。

- TODO：一句话描述项目要解决的问题
- TODO：核心功能清单
- TODO：运行环境与部署形态（Linux/Windows、容器/裸机等）
- TODO：对外接口（gRPC/IEC104/其他）
- TODO：性能与稳定性指标

## 目录结构
- `src/`：核心与模块源码（`core/ModuleManager`、`DataCenter`、`IEC104` 等）
- `protobuf/`：Protobuf/gRPC 协议定义与生成代码（通过 `dspProto` 目标链接）
- `3rdlibs/`：第三方依赖子工程（如 `siren`）
- `test/`：单元测试（GTest）
- `script/`：辅助脚本
- `package/`：构建输出目录（可执行文件、共享库、配置等）

## 构建与测试
依赖通过 CMake + vcpkg manifest 管理（见 `vcpkg.json` / `vcpkg-configuration.json`）。

### 构建
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build --parallel
```

### 运行
构建产物默认输出到 `package/`。程序使用相对路径访问 `./lib`、`./conf`、`./socket`，因此推荐以 `package/` 作为工作目录运行：
```bash
cd package
./MskDSP
```

### 运行测试
```bash
ctest --test-dir build --output-on-failure
```

## 构建产物
- `package/MskDSP`：主程序（Windows 下可能为 `MskDSP.exe`）
- `package/lib/`：模块共享库
- `package/conf/`：配置与生成文件（如运行时保存的 `modConf.bin`）

## 开发约定
统一规范见 `AGENTS.md`（项目结构、编码风格、测试与提交/PR 约定等）。

## 相关文档
- [模块管理器说明](./src/core/ModuleManager/doc/README.md)
