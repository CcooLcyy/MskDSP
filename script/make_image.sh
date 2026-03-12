#!/usr/bin/env bash
set -euo pipefail

log() {
  echo "[make_image] $*"
}

CONTAINER_NAME="x64"
WORKDIR="/data/code/mskdsp"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
VERSION_FILE="${PROJECT_ROOT}/VERSION"

VERSION="${1:-${MSKDSP_VERSION:-}}"
VERSION_SOURCE=""
if [[ -n "${1:-}" ]]; then
  VERSION_SOURCE="命令行参数"
elif [[ -n "${MSKDSP_VERSION:-}" ]]; then
  VERSION_SOURCE="环境变量 MSKDSP_VERSION"
else
  if [[ ! -f "${VERSION_FILE}" ]]; then
    echo "用法: $0 [版本号]" >&2
    echo "未传入版本号时，可通过环境变量 MSKDSP_VERSION 或根目录 VERSION 文件提供版本。" >&2
    exit 1
  fi
  VERSION="$(
    awk '
      {
        sub(/\r$/, "")
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", $0)
        if ($0 != "") {
          print $0
          exit
        }
      }
    ' "${VERSION_FILE}"
  )"
  if [[ -z "${VERSION}" ]]; then
    echo "版本文件内容为空: ${VERSION_FILE}" >&2
    exit 1
  fi
  VERSION_SOURCE="版本文件 ${VERSION_FILE}"
fi
log "版本来源: ${VERSION_SOURCE}，版本号: ${VERSION}"

IMAGE_TAG="mskdsp:${VERSION}"
OUTPUT_DIR="${PROJECT_ROOT}/images"
OUTPUT_TAR="${OUTPUT_DIR}/mskdsp-${VERSION}.tar"

log "Using container: ${CONTAINER_NAME}"
log "Configuring in container..."
podman exec -e VCPKG_ROOT=/data/3rdlibs/vcpkg -w "${WORKDIR}" "${CONTAINER_NAME}" cmake --preset arm64

log "Building in container..."
podman exec -e VCPKG_ROOT=/data/3rdlibs/vcpkg -w "${WORKDIR}" "${CONTAINER_NAME}" cmake --build --preset arm64

log "安装产物到仓库 package 目录..."
podman exec -e VCPKG_ROOT=/data/3rdlibs/vcpkg -w "${WORKDIR}" "${CONTAINER_NAME}" \
  cmake --install "${WORKDIR}/build-arm64" --prefix "${WORKDIR}/package"

log "Building image: ${IMAGE_TAG}"
podman buildx build --platform linux/arm64 -t "${IMAGE_TAG}" "${PROJECT_ROOT}"

log "Saving image to ${OUTPUT_TAR}"
mkdir -p "${OUTPUT_DIR}"
if [[ -e "${OUTPUT_TAR}" ]]; then
  log "Existing tar found; removing ${OUTPUT_TAR}"
  rm -f "${OUTPUT_TAR}"
fi
podman save --format docker-archive -o "${OUTPUT_TAR}" "${IMAGE_TAG}"

log "Removing local image: ${IMAGE_TAG}"
IMAGE_ID="$(podman images -q "${IMAGE_TAG}")"
if [[ -n "${IMAGE_ID}" ]]; then
  if ! podman rmi "${IMAGE_TAG}"; then
    echo "[make_image] Failed to remove image ${IMAGE_TAG}. It may be in use." >&2
    exit 1
  fi
else
  log "Image not found; skipping removal."
fi

log "Done."
