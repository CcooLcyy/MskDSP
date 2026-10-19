# 下位机 GCC14 ARM64 交叉工具镜像

镜像标签为 `mskdsp-lower-cross-tools:ubuntu24.04-gcc14-v1`，基于既有
`mskdsp-ci-tools:ubuntu24.04-x64`，保留原生 `gcc-14` / `g++-14`，新增
Ubuntu 24.04 官方 noble-updates/noble-security 中的
`gcc-14-aarch64-linux-gnu`、`g++-14-aarch64-linux-gnu` 和 ARM64 binutils。
交叉 GCC 固定为 `14.2.0-4ubuntu2~24.04.1cross1`，与现有原生 GCC 的
14.2.0 主次版本一致。升级时使用新镜像标签并重新验证。

在已加载基础镜像的 x64 Runner 主机执行：

```sh
bash docker/gitlab-ci-cross-tools/build.sh
```

脚本使用宿主网络访问官方 APT 源，保留签名校验。它仅创建上述新标签，
不更改现有工具镜像、Runner 或平台服务。Dockerfile 安装后会验证：

- 原生 C/C++ 编译器的 `-dumpmachine` 为 `x86_64-linux-gnu`。
- 交叉 C/C++ 编译器的 `-dumpmachine` 为 `aarch64-linux-gnu`。
- 原生 C++ 程序可编译、链接、执行；交叉 C++ 程序可编译、链接，ELF 为 AArch64。
- ARM64 `readelf`、`objcopy`、`strip` 可用。

此镜像负责在 x64 上交叉编译。完整 ARM64 CTest 仍在
`mskdsp-ci-tools:ubuntu24.04-arm64` 中通过 QEMU 执行，不以交叉编译烟测代替。

独立测试命令：

```sh
python3 -m unittest discover -s script/gitlab -p 'test_cross_tools_image.py' -v
CROSS_TOOLS_DOCKER_TEST=1 python3 -m unittest discover -s script/gitlab -p 'test_cross_tools_image.py' -v
```

第二条在已构建镜像、可调用 Docker 的 x64 主机运行，额外编译并核对两种 ELF 架构。

## 实测记录

2026-10-19，在独立服务器目录
`/home/daniel/mskdsp-lower-ci/tools/cross-tools` 构建并验证：

| 项目 | 结果 |
| --- | --- |
| 镜像架构 | amd64 |
| 原生 GCC/G++ | Ubuntu 14.2.0-4ubuntu2~24.04.1，目标 x86_64-linux-gnu |
| 交叉 GCC/G++ 软件包 | 14.2.0-4ubuntu2~24.04.1cross1，目标 aarch64-linux-gnu |
| ARM64 binutils | 2.42-4ubuntu2.10 |
| 原生 C++ | 编译、链接、执行通过，ELF 为 X86-64 |
| 交叉 C++ | 编译、链接通过，ELF 为 AArch64 |
| 独立测试 | 3 项全部通过，包含实际 Docker 镜像烟测 |

软件包候选版本由官方 APT 源实际查询及模拟安装确认；构建时未升级已有软件包，
仅增加六个交叉工具链相关软件包。此次验证不运行下位机程序或 ARM64 CTest。
