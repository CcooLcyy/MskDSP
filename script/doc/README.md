# 脚本使用说明

本目录用于集中说明 `script/` 下的辅助脚本。所有示例均在仓库根目录执行。

## make_exe.sh（生成自解压脚本）

### 用途
将单个镜像 `tar` 打包为自解压脚本，生成输出 `images/mskdsp-<version>`，用于在目标机器上导入镜像并启动/停止容器。

### 用法
指定镜像 tar：
```
bash script/make_exe.sh /path/to/mskdsp.tar
```
或直接指定版本号（会先调用 `make_image.sh` 生成 tar）：
```
bash script/make_exe.sh <version>
```
输出为：
```
images/mskdsp-<version>
```

### 生成脚本的使用
启动容器：
```
./images/mskdsp-<version> start [-- <docker run 额外参数>]
```
停止并删除容器：
```
./images/mskdsp-<version> stop
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
  - 镜像内固定 `TZ=Asia/Shanghai`，并设置 `/etc/localtime` 为北京时间，保证 IEC104 CP56Time2a 日历时间统一按 UTC+8 解析
  - `-v /data/mskdsp:/opt/mskdsp`
  - `--name mskdsp`
- 额外参数透传到 `docker run`，需用 `--` 分隔，例如：
```
./images/mskdsp-<version> start -- -e FOO=bar
```

### 依赖与注意事项
- 运行环境需具备 `docker` 与 `python3`。
- `--privileged` 赋予容器更高权限，后续可按需改为 `--device` 等更细粒度配置。
- 发布包默认未启用独立 time namespace；因此容器内通过 `clock_settime(CLOCK_REALTIME, ...)` 设置的 Unix 系统时钟会作用于宿主机。容器日历时区固定为 Asia/Shanghai；若额外传入时间命名空间隔离参数，需另行设计宿主机同步方案。
- 若你修改了 tar，请重新执行 `make_exe.sh` 生成新的 `images/mskdsp-<version>`。
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

## GitLab CI 与共享镜像身份校验

### 用途
当前发布入口为 `.gitlab-ci.yml` 和 `script/gitlab/`：两种架构完成编译、完整 CTest 与
独立 BuildKit 打包后，生成 `release/<arch>/` 安装包、`metadata.json` 和 `SHA256SUMS`。
发布作业消费冻结文件，通过 `platform_client.py` 续传入库；主干发布 CI，受保护版本标签发布 ARM64 Stable，
不执行设备安装。完整流程见 [GitLab CI 接入方案](../../doc/GitLab-CI接入方案.md)。

共享文件 `script/workflow/write_lower_update_manifest.py` 保留：GitLab 的
`lower_metadata.py` 导入其 `read_image_config_id`，校验 Docker/OCI 归档平台与镜像 config 摘要。
兼容清单生成 CLI 及功能测试继续保留，仅生成 CI/Stable 新清单；当前 CI 的公开 `latest.json` 和下载校验文件由平台生成。
旧 GitHub Actions、R2 上传与 SSH 静态目录同步入口已移除。

静态发布链路面向上位机使用：下位机不直接访问静态文件服务器，上位机读取 `latest.json` 后下载安装包，再下发到下位机执行安装。

### 清单结构
`latest.json` 使用下位机专用格式，不复用 Tauri updater 格式。核心字段包括：
- `product`：固定为 `mskdsp-lower`
- `channel`：当前发布通道为 `stable`、`ci`；`beta` 仅用于历史清单读取兼容
- `platform`：当前为 `linux-arm64`
- `version`：界面展示版本
- `package_version`：完整包版本，通常与镜像 tag 一致
- `image_id`：发布包内 Docker 镜像的不可变 config ID，用于上位机校验目标机当前运行镜像
- `asset`：安装包名称、下载地址、SHA256 与字节大小
- `checksum`：`SHA256SUMS` 下载地址

GitLab 打包阶段从 BuildKit 导出的普通 Docker 归档读取镜像配置，验证经典 `<摘要>.json`
或 OCI `blobs/sha256/<摘要>` 路径与配置原始字节的 SHA256 一致，并核对镜像平台，
再将该 config 摘要写入冻结元数据。归档在安装包封装、元数据计算完成后才删除。
不依赖构建机器的 `docker image inspect .Id`，避免混用 config 和 manifest 摘要。
兼容清单生成 CLI 的 `--image-tar` / `--image-id` 参数继续保留。

上位机通过 SSH 查询目标容器实际引用的不可变镜像，并在 containerd 场景验证 manifest/index 与 config 的关联后统一成 config 摘要，与清单中的 `image_id` 比较；不能直接混比不同类型的摘要。`asset.sha256` 只用于安装包传输完整性校验，不代表当前容器运行身份。缺失关联或多镜像歧义时拒绝校验通过。

### 平台清单与下载

公开地址由平台配置的基地址决定，架构分别使用以下清单入口：
```
<平台基地址>/mskdsp-lower/<channel>/latest.json
<平台基地址>/mskdsp-lower-x64/ci/latest.json
```

实际安装包及 `SHA256SUMS` URL 取自清单，不能再拼接旧 R2 静态目录。
Stable 从受保护版本标签重新构建，并通过该标签自身完整 CI 后发布 ARM64 包；维护分支仅保留验证产物。x64 仅发布 CI 通道。
Beta 不再生成、打包或上传发布新包；共享清单格式与历史清单读取继续兼容。

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

## dev/lower_device_ssh.py（下位机真机免交互 SSH）

### 用途
在没有交互式终端的环境里（Windows 侧自动化、Agent 会话）连接下位机真机 `192.168.1.219:10022`，
用同一套方式完成「连通自检 / 单行命令 / 多行脚本 / root 提权读取」，不必每次重新拼命令。

### 用法
```
python script/dev/lower_device_ssh.py check                      # 连通自检（主机名/架构/用户/组）
python script/dev/lower_device_ssh.py run  "ls -1 /data/mskdsp"  # 单行远端命令
python script/dev/lower_device_ssh.py bash ./remote_check.sh     # 多行脚本（base64 传输）
python script/dev/lower_device_ssh.py sudo "docker ps"           # 需要 root 的读取
```
默认 `user@host:port` 为 `megsky@192.168.1.219:10022`，可用 `--user`/`--host`/`--port` 覆盖。
密码来自 `--password` 或环境变量 `SSH_DEVICE_PASSWORD`；`sudo` 默认复用该密码，可用 `--sudo-password` 单独指定。

### 行为说明
- `bash` 子命令把本地脚本的换行统一为 LF 后再 base64 传输，规避 Windows 侧 `\r` 污染远端 `fi`/`done`。
- `sudo` 子命令用 `sudo -S -p ''`，sudo 密码经 SSH 通道标准输入发送，不进入命令行参数。
- 输出按 UTF-8 解码；远端退出码即脚本退出码。
- `check` 使用只读命令；`run`/`bash`/`sudo` 的远端命令由调用方提供。

### 依赖与注意事项
- 依赖 Python 3 与 `paramiko`（Windows 侧已验证 3.5.1）。
- 脚本内不保存任何凭据。
- 只读优先：脚本不限制远端命令，调用方需保证不改动设备上与任务无关的服务。
- 真机连接信息（含端口与账号）记录在同级文档 `doc/工作机连接与APP更新.md`。

## 通用约定
- 脚本本身不涉及线程创建；模块线程与日志的统一规则见 `src/core/ModuleManager/doc/README.md`。
