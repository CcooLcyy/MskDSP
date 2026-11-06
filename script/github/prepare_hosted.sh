#!/usr/bin/env bash
# 仅在 GitHub 托管临时主机上准备原生工具链，不在开发工作机安装依赖。
set -euo pipefail
[[ "${GITHUB_ACTIONS:-}" == true && "${RUNNER_ENVIRONMENT:-}" == github-hosted ]] \
  || { echo '错误：工具准备仅允许 GitHub 托管 Runner' >&2; exit 1; }
case "${ARCH:-}:$(uname -m)" in
  x64:x86_64|arm64:aarch64|arm64:arm64) ;;
  *) echo '错误：Runner 必须与目标原生架构一致' >&2; exit 1 ;;
esac
echo '释放托管临时主机的预装 SDK 空间，保留系统及 Docker'
sudo rm -rf -- /usr/share/dotnet /usr/local/lib/android /opt/ghc
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  gcc-14 g++-14 cmake ninja-build ccache curl unzip zip tar \
  pkg-config autoconf automake libtool bison flex python3 python3-venv
echo '原生构建工具准备完成'
gcc-14 --version
g++-14 --version
df -h . /dev/shm
