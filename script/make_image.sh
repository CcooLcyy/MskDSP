#!/usr/bin/env bash
set -euo pipefail

log() {
  echo "[make_image] $*"
}

VERSION="${1:-${MSKDSP_VERSION:-}}"
if [[ -z "${VERSION}" ]]; then
  echo "Usage: $0 <version>" >&2
  echo "Or set MSKDSP_VERSION environment variable." >&2
  exit 1
fi

CONTAINER_NAME="x64"
WORKDIR="/data/code/mskdsp"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
IMAGE_TAG="mskdsp:${VERSION}"
OUTPUT_DIR="${PROJECT_ROOT}/images"
OUTPUT_TAR="${OUTPUT_DIR}/mskdsp-${VERSION}.tar"

log "Using container: ${CONTAINER_NAME}"
log "Configuring in container..."
podman exec -e VCPKG_ROOT=/data/3rdlibs/vcpkg -w "${WORKDIR}" "${CONTAINER_NAME}" cmake --preset arm64

log "Building in container..."
podman exec -e VCPKG_ROOT=/data/3rdlibs/vcpkg -w "${WORKDIR}" "${CONTAINER_NAME}" cmake --build --preset arm64

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
