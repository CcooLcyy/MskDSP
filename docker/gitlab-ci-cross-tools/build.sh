#!/usr/bin/env bash
# 构建独立的新标签交叉工具镜像，并检查两套工具链身份。
set -euo pipefail
TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE=mskdsp-lower-cross-tools:ubuntu24.04-gcc14-v1
echo '构建下位机 GCC14 ARM64 交叉工具镜像'
docker build --network=host --platform=linux/amd64 --tag "${IMAGE}" \
  --file "${TOOLS_DIR}/Dockerfile" "${TOOLS_DIR}"
echo '核对 x64 工具镜像架构与原生、交叉编译器'
docker image inspect "${IMAGE}" --format '镜像架构：{{.Architecture}}；镜像身份：{{.Id}}'
docker run --rm --network=none --entrypoint bash "${IMAGE}" -lc '
  set -euo pipefail
  test "$(uname -m)" = x86_64
  for compiler in gcc-14 g++-14 aarch64-linux-gnu-gcc-14 aarch64-linux-gnu-g++-14; do
    "$compiler" --version | head -n 1
    "$compiler" -dumpmachine
  done
  test "$(gcc-14 -dumpmachine)" = x86_64-linux-gnu
  test "$(g++-14 -dumpmachine)" = x86_64-linux-gnu
  test "$(aarch64-linux-gnu-gcc-14 -dumpmachine)" = aarch64-linux-gnu
  test "$(aarch64-linux-gnu-g++-14 -dumpmachine)" = aarch64-linux-gnu
  aarch64-linux-gnu-readelf --version | head -n 1
'
echo '交叉工具镜像构建及身份检查通过'
