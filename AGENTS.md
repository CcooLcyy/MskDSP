# 仓库指南

## 项目结构与模块组织
核心代码位于 `src/`：`core/ModuleManager` 负责管理模块生命周期并暴露 gRPC 服务；`DataCenter` 与 `IEC104` 是插件式共享库，通过管理器进行链接；`main.cc` 在后台线程启动管理器。Protobuf 协议位于 `protobuf/`，生成的 gRPC 桩由 `dspProto` 目标统一链接。第三方代码位于 `3rdlibs/`（以子工程方式构建）。构建产物输出到 `package/`（`package/MskDSP` 可执行文件、`package/lib` 共享库、`package/conf` 生成的配置头文件）；`package/conf/configPusher/` 下所有配置仅供 configPusher 下发/示例使用，不作为具体模块运行时配置来源。辅助脚本位于 `script/`。

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
格式由 `.clang-format`（Google base）约束：2 空格缩进、无 Tab、不限制行宽、brace 列表更紧凑。使用现代 C++23，并优先采用 RAII；指针/引用对齐遵循 `.clang-format`，当前为贴变量名风格（`Type *ptr` / `Type &ref`）。gRPC/Proto 文件中 Message/Service 使用 PascalCase；生成头文件通过 `dspProto` 目标引入与使用。模块库名称需与 `cmake/LibInfo.cmake` 中的设置保持一致。

- Protobuf 注释规范：`protobuf/*.proto` 中所有 `service` 与每个 `rpc` 声明必须添加 `//` 注释，详细说明该 RPC 的用途/调用场景、关键语义（是否 stream、是否幂等、是否有落盘/启停/清理缓存等副作用）、以及主要错误码与边界条件；上位机对接将以该注释为依据。
- Protobuf 脚手架默认 `Ping`：新模块脚手架会在 `.proto` 中生成默认的 `Ping(Empty)->Empty` RPC；当该 service 开始实现其他业务 RPC 后，应删除该默认 `Ping` RPC（避免长期保留占位接口）。
- 协议点表设计需支持每点 `scale/offset/deadband`：`scale/offset` 用于工程量换算（`value = raw * scale + offset`，`scale=0` 视为 1），`deadband` 为工程量单位（<=0 不过滤）；BOOL 点忽略这三个参数。

## 改动原则
在满足需求与修复问题的前提下，改动尽量小且聚焦；避免无关重构、批量格式化或大范围重命名。

- 在开始任何实际改动（包括代码、文档、配置文件）之前，先与用户讨论目标/范围/方案；仅在用户明确确认“可以修改”后，才进行文件编辑或执行会写入仓库的命令。
- 回复使用中文，必要时可使用英文术语。
- 修改完代码后，不要自动执行编译/构建/测试动作（如 `cmake --build ...`、`ctest` 等）；因为其他 AGENT 可能同步修改导致构建失败；仅在用户明确要求时才执行。
- 除非用户允许，否则不要执行任何 `git ...` 命令（包括 `git status/diff/add/restore/commit/reset` 等）；需要 git 操作时要询问并请求用户统一。
- 如需查找日志，默认在 `package/log` 目录中进行查找。
- 所有模块配置统一由 configPusher 通过 gRPC 下发；涉及配置来源或下发流程的变更时先询问用户是否需要修改，否则默认继续使用 configPusher。
- 所有代码改动也要增加日志。
- 所有协议实现需要增加日志，显示收发报文内容。
- 所有新增/修改的注释、日志、字符串的错误信息必须使用中文。
- 回复中如有“启动”描述，需要明确是启动模块还是启动模块内的功能（例如启动连接/任务）。
- 新增的 C++ 模板代码文件使用 `.hpp` 后缀（例如 `Foo.hpp`）。
- 新增模块请优先使用脚手架 `bash script/new_module.sh <NewModule>` 生成骨架（目录结构/CMake/LibInfo 等），避免从零手工创建或复制粘贴现有模块。
- 本项目有配套上位机：接口实现需考虑上位机调用场景；且上位机开发会参考本项目文档，新增/变更接口时需同步补充上位机侧的设计建议与使用说明。
- 实现代码时应遵循高内聚、低耦合与单一职责原则：避免把“控制面/数据面/外部依赖适配”长期堆叠在同一个类/文件中；当功能增长导致职责变杂时，优先拆分与抽象边界。

## 测试规范
单元测试使用 GTest（顶层测试放在 `test/` 或模块内 `test` 目录）。测试文件命名为 `*_test.cc`，通过 `add_test` 注册，使用 `ctest --output-on-failure` 运行。覆盖模块生命周期边界（load/unload、端口复用）以及 protobuf/grpc 协议兼容性。优先确定性测试；避免绑定固定服务端口，必要时通过随机可用端口进行隔离。

- 所有自有测试用例（`test/` 与 `src/**/test`）需要在每个 `TEST/TEST_F/TEST_P` 前增加一行注释，明确该用例验证的功能点/边界条件；`3rdlibs/` 下的第三方测试不要求遵守本条。
- 后续模块测试若需依赖 DataCenter，一律使用 `test/support/FakeDataCenter.hpp` 的 `FakeDataCenterState/MakeStub` 进行 gmock，不启动真实 DataCenter 服务。
  例如：
  ```cpp
  #include "support/FakeDataCenter.hpp"

  FakeDataCenterState state;
  auto stub = MakeStub(&state);
  mgr.setDataCenterStub(stub);
  ```

## 提交与 PR 规范
近期提交历史使用方括号前缀（如 `[feature]`）+ 简短中文摘要。保持该风格（可用 `[fix]`、`[refactor]` 等），首行尽量控制在 ~72 字符内。PR 建议包含：范围/目的、各模块关键变更、已执行的构建/测试命令、端口/配置变更。关联相关 issue；协议/互操作变更附带截图或日志。
- 当用户说“git 提交”且已授权执行 git 命令时，先查看仓库改动，按改动点分批提交，并且不要提交任何 JSON 配置文件（如 `*.json`）。

## 架构说明
模块以共享库形式由管理器动态加载；版本元信息由 `cmake/LibInfo.h.in` 生成到各模块的 `include/` 中。gRPC 服务是主要集成面：先更新 `.proto`，再通过 CMake 重新生成。`package/conf` 仅保留模板纳入版本控制，避免提交本地运行时产物。
