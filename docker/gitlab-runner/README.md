# 下位机 GitLab Runner

本目录提供同机独立 Runner 的部署模板。服务器使用独立目录 `/home/daniel/mskdsp-lower-ci/runner`，配置和缓存与平台 Runner 分离；项目专用标签仅为 `lower-ci`，关闭无标签作业并限制为受保护分支。

在 GitLab 下位机项目创建项目级 Runner，再用其认证令牌注册。将 `config.template.toml` 复制为服务器私有的 `config/config.toml`，替换令牌后设置权限 0600；不要提交实际配置或令牌。复制 `compose.yml` 后执行 `docker compose up -d`。

Runner 管理容器需要访问 Docker socket 以创建作业容器；作业容器不挂载该 socket，且不使用特权模式。作业通过独立 BuildKit 服务制作镜像。下位机 Runner 每次只接一个作业，单作业限制 4 CPU、24 GiB 内存且禁止交换；主工程和 vcpkg 默认各使用 4 路并行，可分别通过 `CMAKE_BUILD_PARALLEL_LEVEL`、`VCPKG_MAX_CONCURRENCY` 设置正整数。

部署前由仓库公共部署工具准备命名卷 `mskdsp-ci-ram`：Docker local 驱动，`type=tmpfs`、`device=tmpfs`、`o=size=24g,noswap,mode=1777`。下位机与通用 Linux Runner 的作业和归档 helper 共用该卷，挂载到 `/builds`；24 GiB 是共享文件容量上限，作业进程与 tmpfs 占用仍共用容器内存限额。不要只给 Runner 管理容器配置 tmpfs，也不要在卷不存在时直接接单，以免 Docker 自动创建普通磁盘卷。

模板内置 `ci-ram-lease` 服务，使用 `alpine:3.20` 执行长时间休眠。Runner 在源码检出前创建服务，服务自动继承共享卷挂载并持续运行到任务清理，保持 helper、主脚本和归档阶段之间的挂载连续性；仅依靠阶段容器会在它们全部停止时卸载 tmpfs 并丢失源码。该服务限制为 64 MiB、0.1 CPU 且禁止交换，不开放端口，因此设置 `wait_for_services_timeout=-1` 关闭端口健康等待。公共配置工具只接受此受管服务，发现其他服务时拒绝自动修改。

公共准备脚本由仓库统一维护，通过宿主机 `/home/daniel/ci-ram/tools` 只读挂载，`pre_build_script` 加载 `/opt/ci-ram/pre-build.sh`。该脚本负责挂载、空间与目录检查、临时目录及跨 Linux Runner 的共享构建锁；下位机构建脚本不重复维护这些逻辑。部署与验收见 `doc/CI共享内存构建方案.md`。

启用自定义检出目录，固定 `GIT_CLONE_PATH=/builds/lower/$CI_PROJECT_PATH`，保持构建和独立 x64 测试的绝对路径一致。`GIT_STRATEGY=clone` 配合 `FF_ENABLE_JOB_CLEANUP=true`，由 Runner 在 artifact 上传后清理工程，再停止并清理保活服务；不要在 `after_script` 提前删除 `build-x64/`。最后运行中的消费者停止并释放挂载时，tmpfs 内容自动释放；异常保活服务或其他容器仍在运行时，由公共诊断与回收流程处理。

宿主机 `/home/daniel/mskdsp-lower-ci/cache-direct` 继续挂载到 `/ci-cache`，vcpkg 源码、下载、二进制缓存和 ccache 保留在持久磁盘，作业结束不清理这些缓存。BuildKit 持久缓存也继续保留。

每个下位机作业的 `/dev/shm` 上限为 1 GiB，计入上述内存限制。完整测试仅将临时写入工作目录放入该内存文件系统，种子模块仍从磁盘读取；避免共享磁盘归档干扰 SQLite/SOE 单测的既有等待预算。

ARM64 主工程使用 x64 GCC14 交叉编译，不运行 ARM64 测试；x64 完整 CTest 通过后才允许双架构打包发布。准备流程及验收要求见 `doc/GitLab-CI接入方案.md`；工具镜像升级时应使用新标签，保留现有平台工具镜像。
