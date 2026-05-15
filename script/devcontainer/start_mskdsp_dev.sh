#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

IMAGE_TAG="mskdsp-dev:ubuntu24.04"
CONTAINER_NAME="mskdsp-dev"
SHARE_ROOT="/home/code/share"
CODEX_ROOT="/home/code/.codex"
CODEX_SYNC_ROOT="/codex-sync"
CODEX_HOME_VOLUME="mskdsp-codex-home"
WORKDIR="/data/code/mskdsp"

VOLUMES=(
  "${CODEX_HOME_VOLUME}"
  "mskdsp-vcpkg-root"
  "mskdsp-vcpkg-cache"
  "mskdsp-ccache"
)

RECREATE=0
NO_BUILD=0

log() {
  printf '[mskdsp-dev] %s\n' "$*"
}

fail() {
  printf '[mskdsp-dev] 错误：%s\n' "$*" >&2
  exit 1
}

usage() {
  cat <<'EOF'
用法：
  ./script/devcontainer/start_mskdsp_dev.sh [--recreate] [--no-build]

参数：
  --recreate   删除现有容器后重新创建
  --no-build   跳过镜像构建，直接复用现有镜像
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --recreate)
      RECREATE=1
      ;;
    --no-build)
      NO_BUILD=1
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      fail "未知参数：$1"
      ;;
  esac
  shift
done

command -v docker >/dev/null 2>&1 || fail "未找到 docker，请先安装并启动 Docker"
[ -d "${SHARE_ROOT}" ] || fail "未找到 ${SHARE_ROOT}"
[ -d "${CODEX_ROOT}" ] || fail "未找到 ${CODEX_ROOT}"
[ -f "${CODEX_ROOT}/config.toml" ] || fail "未找到 ${CODEX_ROOT}/config.toml"

for volume_name in "${VOLUMES[@]}"; do
  docker volume create "${volume_name}" >/dev/null
done

if [ "${NO_BUILD}" -eq 0 ]; then
  log "开始构建镜像 ${IMAGE_TAG}"
  docker build -f "${PROJECT_ROOT}/.devcontainer/Dockerfile" -t "${IMAGE_TAG}" "${PROJECT_ROOT}"
fi

container_exists=0
if docker container inspect "${CONTAINER_NAME}" >/dev/null 2>&1; then
  container_exists=1
fi

if [ "${container_exists}" -eq 1 ] && [ "${RECREATE}" -eq 0 ]; then
  current_image_id="$(docker inspect --format '{{.Image}}' "${CONTAINER_NAME}")"
  target_image_id="$(docker image inspect --format '{{.Id}}' "${IMAGE_TAG}")"
  if [ "${current_image_id}" != "${target_image_id}" ]; then
    log "检测到容器镜像已变化，准备重建容器"
    RECREATE=1
  fi
fi

if [ "${container_exists}" -eq 1 ] && [ "${RECREATE}" -eq 0 ]; then
  network_mode="$(docker inspect --format '{{.HostConfig.NetworkMode}}' "${CONTAINER_NAME}")"
  if [ "${network_mode}" != "host" ]; then
    log "检测到开发容器网络模式不是 host，准备重建容器"
    RECREATE=1
  fi
fi

if [ "${container_exists}" -eq 1 ] && [ "${RECREATE}" -eq 0 ]; then
  codex_home_type="$(docker inspect --format '{{range .Mounts}}{{if eq .Destination "/root/.codex"}}{{.Type}}{{end}}{{end}}' "${CONTAINER_NAME}")"
  codex_home_name="$(docker inspect --format '{{range .Mounts}}{{if eq .Destination "/root/.codex"}}{{.Name}}{{end}}{{end}}' "${CONTAINER_NAME}")"
  codex_sync_source="$(docker inspect --format '{{range .Mounts}}{{if eq .Destination "/codex-sync"}}{{.Source}}{{end}}{{end}}' "${CONTAINER_NAME}")"
  codex_sync_rw="$(docker inspect --format '{{range .Mounts}}{{if eq .Destination "/codex-sync"}}{{.RW}}{{end}}{{end}}' "${CONTAINER_NAME}")"
  share_root_source="$(docker inspect --format '{{range .Mounts}}{{if eq .Destination "/home/code/share"}}{{.Source}}{{end}}{{end}}' "${CONTAINER_NAME}")"
  share_root_rw="$(docker inspect --format '{{range .Mounts}}{{if eq .Destination "/home/code/share"}}{{.RW}}{{end}}{{end}}' "${CONTAINER_NAME}")"
  if [ "${codex_home_type}" != "volume" ] || \
     [ "${codex_home_name}" != "${CODEX_HOME_VOLUME}" ] || \
     [ "${codex_sync_source}" != "${CODEX_ROOT}" ] || \
     [ "${codex_sync_rw}" != "false" ] || \
     [ "${share_root_source}" != "${SHARE_ROOT}" ] || \
     [ "${share_root_rw}" != "false" ]; then
    log "检测到 Codex 挂载策略已变化，准备重建容器"
    RECREATE=1
  fi
fi

if [ "${container_exists}" -eq 1 ] && [ "${RECREATE}" -eq 1 ]; then
  log "删除旧容器 ${CONTAINER_NAME}"
  docker rm -f "${CONTAINER_NAME}" >/dev/null
  container_exists=0
fi

if [ "${container_exists}" -eq 0 ]; then
  log "创建并启动容器 ${CONTAINER_NAME}"
  docker_run_args=(
    docker run -d
    --name "${CONTAINER_NAME}"
    --hostname "${CONTAINER_NAME}"
    --network host
    -w "${WORKDIR}"
    -v "${SHARE_ROOT}:/data"
    -v "${SHARE_ROOT}:${SHARE_ROOT}:ro"
    -v "${CODEX_HOME_VOLUME}:/root/.codex"
    -v "${CODEX_ROOT}:${CODEX_SYNC_ROOT}:ro"
    -v mskdsp-vcpkg-root:/data/3rdlibs/vcpkg
    -v mskdsp-vcpkg-cache:/root/.cache/vcpkg
    -v mskdsp-ccache:/root/.cache/ccache
  )

  for proxy_var in HTTP_PROXY HTTPS_PROXY NO_PROXY http_proxy https_proxy no_proxy; do
    if [ -n "${!proxy_var:-}" ]; then
      docker_run_args+=(-e "${proxy_var}=${!proxy_var}")
    fi
  done

  "${docker_run_args[@]}" "${IMAGE_TAG}" >/dev/null
else
  log "启动已有容器 ${CONTAINER_NAME}"
  docker start "${CONTAINER_NAME}" >/dev/null
fi

log "等待容器初始化完成"
for _ in $(seq 1 360); do
  if ! docker container inspect "${CONTAINER_NAME}" >/dev/null 2>&1; then
    fail "容器已退出，请执行 docker logs ${CONTAINER_NAME} 查看原因"
  fi

  running_state="$(docker inspect --format '{{.State.Running}}' "${CONTAINER_NAME}")"
  if [ "${running_state}" != "true" ]; then
    docker logs "${CONTAINER_NAME}" || true
    fail "容器未处于运行状态"
  fi

  if docker exec "${CONTAINER_NAME}" bash -lc 'test -x /data/3rdlibs/vcpkg/vcpkg' >/dev/null 2>&1; then
    log "容器已就绪"
    log "进入容器：docker exec -it ${CONTAINER_NAME} bash"
    exit 0
  fi

  sleep 5
done

docker logs "${CONTAINER_NAME}" || true
fail "等待初始化超时，请查看容器日志"
