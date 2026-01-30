# 脚本使用说明

本目录用于集中说明 `script/` 下的辅助脚本。所有示例均在仓库根目录执行。

## make_exe.sh（生成自解压脚本）

### 用途
将单个镜像 `tar` 打包为自解压脚本，生成固定输出 `images/mskdsp`，用于在目标机器上导入镜像并启动/停止容器。

### 用法
指定镜像 tar：
```
bash script/make_exe.sh /path/to/mskdsp.tar
```
或直接指定版本号（会先调用 `make_image.sh` 生成 tar）：
```
bash script/make_exe.sh <version>
```
输出固定为：
```
images/mskdsp
```

### 生成脚本的使用
启动容器：
```
./images/mskdsp start [-- <docker run 额外参数>]
```
停止并删除容器：
```
./images/mskdsp stop
```

### 运行行为说明
- 自解压脚本内部包含镜像 tar，运行时会解出到临时文件并在使用后删除。
- 读取 tar 内 `manifest.json` 获取 `repo:tag` 与镜像 config ID：
  - 若本机已有镜像 ID 与 tar 一致，则跳过 `docker load`。
- 若 ID 不一致，会导入新镜像并删除旧镜像 ID。
- 当发生 `docker load` 时，会清理同一仓库下除当前 tag 外的其他 tag（无论 ID 是否相同）。
- 启动时会确保宿主机目录存在：`/data/mskdsp` 和 `/data/mskdsp/lib` 会自动创建（若不存在）。
- 若 `/data/mskdsp/conf` 不存在或为空，会从镜像 `/opt/mskdsp/conf` 复制默认配置。
- **仅当镜像 ID 更新时**，才会更新宿主机产物：
  - 删除 `/data/mskdsp/MskDSP`
  - 删除 `/data/mskdsp/lib/` 下所有内容
  - 从镜像的 `/opt/mskdsp` 复制最新 `MskDSP` 与 `lib`
- **若镜像 ID 未变化但宿主机缺少文件**（如 `MskDSP` 不存在或 `lib` 为空），会补齐缺失项，不会动其他目录。
- 默认 `docker run` 参数：
  - `--privileged`
  - `--restart unless-stopped`
  - `--network host`
  - `-v /data/mskdsp:/opt/mskdsp`
  - `--name mskdsp`
- 额外参数透传到 `docker run`，需用 `--` 分隔，例如：
```
./images/mskdsp start -- -e FOO=bar
```

### 依赖与注意事项
- 运行环境需具备 `docker` 与 `python3`。
- `--privileged` 赋予容器更高权限，后续可按需改为 `--device` 等更细粒度配置。
- 若你修改了 tar，请重新执行 `make_exe.sh` 生成新的 `images/mskdsp`。
- 使用版本号模式时会调用 `make_image.sh`，因此需要 `podman`、`podman buildx` 及相关构建环境。

## make_image.sh（构建并导出镜像）

### 用途
在容器内执行构建，并用 `podman buildx` 生成镜像，最终导出为 `images/mskdsp-<version>.tar`。

### 用法
```
bash script/make_image.sh <version>
```
或通过环境变量：
```
MSKDSP_VERSION=<version> bash script/make_image.sh
```

### 行为说明
- 使用容器 `x64` 在 `/data/code/mskdsp` 内执行 CMake 配置与构建。
- 生成镜像标签：`mskdsp:<version>`。
- 输出 tar：`images/mskdsp-<version>.tar`（存在则先删除）。
- 构建完成后会尝试删除本地镜像 `mskdsp:<version>`。

### 依赖与注意事项
- 依赖 `podman` 与 `podman buildx`。
- 依赖容器 `x64` 与 `VCPKG_ROOT` 等环境（脚本内默认 `/data/3rdlibs/vcpkg`）。
- 输出 tar 可用于 `make_exe.sh` 生成自解压脚本。

## new_module.sh（模块脚手架）

### 用途
生成模块骨架（源码、CMake、文档与 proto），并自动加入 `src/CMakeLists.txt`。

### 用法
```
bash script/new_module.sh <ModuleName> [--lib-name <LibName>] [--no-proto] [--force]
```

### 关键参数
- `--lib-name`：自定义库名（默认根据模块名推导）
- `--no-proto`：不生成 `protobuf/<ModuleName>.proto`
- `--force`：覆盖已有目录/协议文件

### 行为说明
- 生成目录 `src/<ModuleName>/`，含 `include/`、`cmake/`、`doc/`。
- 自动追加 `add_subdirectory(<ModuleName>)` 到 `src/CMakeLists.txt`。
- 默认生成 `Ping(Empty)->Empty` RPC（开始实现业务 RPC 后应删除）。
- 生成 `GetModuleManifestPb` 模板（含版本信息），依赖项默认留空。
- 生成的 `.proto` 自带 `service/rpc` 注释，满足接口注释规范。
- 生成的模块模板已包含中文日志（模块启动/停止、Ping 请求）。
- 模块文档骨架包含“线程与日志”约定说明。

### 注意事项
- `protobuf/` 是子模块时会提示影响子模块工作区。
- 生成的 `doc/README.md` 仅为骨架，需自行完善。

## strip_debug.sh（剥离调试符号）

### 用途
从 `package/` 下的 ELF 文件剥离调试符号，并将调试信息保存到指定目录。

### 用法
```
bash script/strip_debug.sh <package_dir> <debug_dir> <strip_tool> <objcopy_tool>
```

### 行为说明
- 仅处理 ELF 文件，跳过 `*_test*` 与已存在的 `.debug` 文件。
- 对每个 ELF 生成 `<debug_dir>/<relpath>.debug`。
- 原文件会被 `strip`，并添加 `gnu debuglink` 指向 debug 文件。

### 依赖与注意事项
- 依赖 `file`、`strip`、`objcopy` 工具。
- `debug_dir` 会被创建（如不存在）。

## 通用约定
- 脚本本身不涉及线程创建；模块线程与日志的统一规则见 `src/core/ModuleManager/doc/README.md`。
