# 仓库指南

## 项目结构与模块组织
核心代码位于 `src/`：`core/ModuleManager` 负责管理模块生命周期并暴露 gRPC 服务；`DataCenter` 与 `IEC104` 是插件式共享库，通过管理器进行链接；`main.cc` 在后台线程启动管理器。Protobuf 协议位于 `protobuf/`，生成的 gRPC 桩由 `dspProto` 目标统一链接。第三方代码位于 `3rdlibs/siren/`（以子工程方式构建）。构建产物输出到 `package/`（`package/MskDSP` 可执行文件、`package/lib` 共享库、`package/conf` 生成的配置头文件）。辅助脚本位于 `script/`。

## 构建、测试与开发命令
依赖通过 CMake + vcpkg manifest（`vcpkg*.json`）管理，包含 gRPC、Protobuf、Boost、GTest。典型流程：
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build --parallel
ctest --test-dir build        # 当定义了测试时
```

VSCode CMake Tools（用户确认可用的配置命令）：
```bash
/usr/bin/cmake -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE -DCMAKE_C_COMPILER:FILEPATH=/usr/bin/gcc-14 -DCMAKE_CXX_COMPILER:FILEPATH=/usr/bin/g++-14 -DCMAKE_TOOLCHAIN_FILE=/data/3rdlibs/vcpkg/scripts/buildsystems/vcpkg.cmake --no-warn-unused-cli -S /data/code/mskdsp -B /data/code/mskdsp/build -G "Unix Makefiles"
```

如需安装/落地构建产物，执行 `cmake --build build --target install`；二进制与库会出现在 `package/` 下。生产构建使用带调试信息的 Release（`-DCMAKE_BUILD_TYPE=RelWithDebInfo`）。

## 编码风格与命名约定
格式由 `.clang-format`（Google base）约束：2 空格缩进、无 Tab、不限制行宽、brace 列表更紧凑。使用现代 C++23，并优先采用 RAII；指针对齐风格为右贴（`Type* ptr`）。gRPC/Proto 文件中 Message/Service 使用 PascalCase；生成头文件通过 `dspProto` 目标引入与使用。模块库名称需与 `cmake/LibInfo.cmake` 中的设置保持一致。

## 改动原则
在满足需求与修复问题的前提下，改动尽量小且聚焦；避免无关重构、批量格式化或大范围重命名。

- 在开始任何实际改动（包括代码、文档、配置文件）之前，先与用户讨论目标/范围/方案；仅在用户明确确认“可以修改”后，才进行文件编辑或执行会写入仓库的命令。
- 除非用户允许，否则不要执行任何 `git ...` 命令（包括 `git status/diff/add/restore/commit/reset` 等）；需要 git 操作时要询问并请求用户统一。
- 新增的 C++ 模板代码文件使用 `.hpp` 后缀（例如 `Foo.hpp`）。
- 本项目有配套上位机：接口实现需考虑上位机调用场景；且上位机开发会参考本项目文档，新增/变更接口时需同步补充上位机侧的设计建议与使用说明。

## 测试规范
单元测试使用 GTest（`3rdlibs/siren` 在 Debug 下启用；顶层测试放在 `test/` 或模块内 `test` 目录）。测试文件命名为 `*_test.cc`，通过 `add_test` 注册，使用 `ctest --output-on-failure` 运行。覆盖模块生命周期边界（load/unload、端口复用）以及 protobuf/grpc 协议兼容性。优先确定性测试；避免绑定固定服务端口，必要时通过随机可用端口进行隔离。

## 提交与 PR 规范
近期提交历史使用方括号前缀（如 `[feature]`）+ 简短中文摘要。保持该风格（可用 `[fix]`、`[refactor]` 等），首行尽量控制在 ~72 字符内。PR 建议包含：范围/目的、各模块关键变更、已执行的构建/测试命令、端口/配置变更。关联相关 issue；协议/互操作变更附带截图或日志。

## 架构说明
模块以共享库形式由管理器动态加载；版本元信息由 `cmake/LibInfo.h.in` 生成到各模块的 `include/` 中。gRPC 服务是主要集成面：先更新 `.proto`，再通过 CMake 重新生成。`package/conf` 仅保留模板纳入版本控制，避免提交本地运行时产物。
