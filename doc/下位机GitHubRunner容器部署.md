# 下位机 GitHub Runner 容器部署

## 目标与边界

本目录中的 runner 镜像用于在本地 x86_64 Linux 构建机上执行下位机
`mskdsp` 的 GitHub Actions job。它复刻当前 workflow 使用的 Ubuntu 24.04、
GCC 14、ARM64 交叉编译和 Docker Buildx 环境。

镜像不替代下位机运行镜像，也不把下位机模块部署在 runner 容器内。workflow
仍会使用仓库根目录的 `Dockerfile` 构建 ARM64 下位机业务镜像，再导出为自解压
安装包。

ARM64 构建与发布 job 使用 `lower-builder` 自托管标签；x64 Debug 校验、PR 校验和
Beta 自动晋升仍使用 GitHub 托管 runner。

## 运行前提

- 宿主机为 x86_64 Linux，推荐 Ubuntu 24.04。
- 宿主机 Docker Engine 已启动，当前操作者可执行 `docker`。
- 宿主机已安装并启用 Buildx。
- 如后续业务 Dockerfile 增加执行 ARM64 程序的 `RUN` 指令，宿主机还需为 ARM64
  Docker 构建注册 binfmt/QEMU。例如执行一次：

  ```bash
  docker run --privileged --rm tonistiigi/binfmt --install arm64
  ```

- 构建机能访问 GitHub、vcpkg 源和 `arm64v8/ubuntu:noble` 镜像源。
- 如果使用宿主机上的 Clash Verge/Mihomo 代理，代理应提供 HTTP 端口；本方案默认使用
  `127.0.0.1:7897`。

runner 容器使用 host 网络并挂载 Docker socket，因此容器中的 workflow 可以控制宿主机
Docker，并可访问宿主机的 `127.0.0.1:7897` 代理，等同于拥有宿主机高权限。只能在专用
构建机上运行，且不要调度来自不可信 fork 或外部贡献者的 pull request。

## 镜像内容

`docker/lower-runner/Dockerfile` 固定基于 Ubuntu 24.04，预装：

- GitHub Actions Runner
- Git、GitHub CLI、Python 3、SSH/SCP、Curl、JQ
- CMake、Ninja、GCC/G++ 14、ccache
- ARM64 交叉编译器与 binutils
- Docker CLI 与 Buildx 插件

workflow 仍会执行 `sudo apt-get install` 和按仓库 baseline 初始化 vcpkg；镜像预装
工具只是避免每个 job 从空系统开始。runner 用户具有免密码 sudo 权限，以兼容现有
workflow。

## 构建与注册

复制 `docker/lower-runner/.env.example` 为 `docker/lower-runner/.env`，再填写只属于
本机构建机的配置。该文件已被 `.gitignore` 忽略，不应提交：

```dotenv
GITHUB_URL=https://github.com/<owner>/<repository>
RUNNER_TOKEN=<GitHub 页面生成的一次性注册 token>
RUNNER_NAME=mskdsp-lower-builder-01
RUNNER_LABELS=lower-builder,x64
RUNNER_HTTP_PROXY=http://127.0.0.1:7897
RUNNER_HTTPS_PROXY=http://127.0.0.1:7897
RUNNER_NO_PROXY=localhost,127.0.0.1,::1
```

Compose 使用 host 网络，因此不需要为 runner 发布端口；`127.0.0.1` 代理地址指向宿主机。
如果不使用代理，可将 `RUNNER_HTTP_PROXY` 和 `RUNNER_HTTPS_PROXY` 设为空。镜像内已将
Git 固定为 HTTP/1.1，并设置连接与低速传输超时。

构建机访问 GitHub release asset 超时时，可在 `.env` 中将
`RUNNER_DOWNLOAD_BASE_URL` 改为本机认可的代理地址。该参数只影响镜像构建阶段，
代理地址属于外部服务，使用前应按本机网络策略评估。镜像默认固定使用 runner
`2.337.0`，并关闭运行时自动更新，避免更新包下载失败导致正在执行的 job 被取消；
升级 runner 时先修改 `RUNNER_VERSION` 并重新构建镜像，确认可用后再启动容器。

构建并启动 runner 容器：

```bash
docker compose --env-file docker/lower-runner/.env \
  -f docker/lower-runner/compose.yml up -d --build
```

停止 runner 容器：

```bash
docker compose --env-file docker/lower-runner/.env \
  -f docker/lower-runner/compose.yml down
```

入口脚本首次启动时注册一个长期 runner，后续重启从 `lower-runner-state` 命名卷
恢复注册状态，不再需要重复使用注册 token。只有删除该命名卷或重新注册时才需要
在 `.env` 中提供新的短期 `RUNNER_TOKEN`。若额外设置专用的 `RUNNER_REMOVE_TOKEN`，
容器停止时会尝试主动注销；长期运行通常留空。

`lower-runner-state`、`lower-runner-work` 与 `lower-runner-docker` 是命名卷：前者
持久化 runner 注册状态；`lower-runner-work` 持久化工作目录，保留
workflow 在 `${GITHUB_WORKSPACE}/.cache` 下创建的 vcpkg 与 ccache 内容；后者保存
Buildx 客户端配置。删除 Compose 容器不会删除这两个卷。

## 验证与接入

容器启动后，在仓库的 `Settings -> Actions -> Runners` 中确认它显示为 `Idle` 且标签
包含 `self-hosted`、`Linux`、`X64` 与 `lower-builder`。使用以下命令检查日志：

```bash
docker compose --env-file docker/lower-runner/.env \
  -f docker/lower-runner/compose.yml logs -f
```

目标 ARM64 job 已使用以下标签：

```yaml
runs-on: [self-hosted, linux, x64, lower-builder]
```

建议仅把 `main`、`beta/**`、nightly 与 release 的 ARM64 打包 job 切换到这个标签；
面向外部 pull request 的校验继续使用 GitHub 托管 runner。

## 失败处理

- `Docker socket 不是 socket`：确认 Compose 中的 `/var/run/docker.sock` 挂载指向宿主
  Docker Engine socket。
- `permission denied`：入口会按 socket GID 创建 Docker 用户组；若宿主使用 rootless
  Docker，请改为挂载该用户的 rootless socket，并以同一 UID 运行 runner。
- 未来增加 ARM64 `RUN` 指令后出现 `exec format error`：注册宿主 binfmt/QEMU，并确认
  `docker buildx inspect --bootstrap` 的平台列表包含 `linux/arm64`。当前业务 Dockerfile
  只复制文件，通常不需要模拟执行 ARM64 程序。
- runner 无法注册：重新生成 token。注册 token 是短期凭据，容器镜像和命名卷都不保存它。
