#!/usr/bin/env bash
set -euo pipefail

log() {
  echo "[mk_rtu_image] $*"
}

die() {
  echo "[mk_rtu_image] 错误: $*" >&2
  exit 1
}

usage() {
  cat <<'EOF' >&2
用法:
  bash script/mk_rtu_image.sh <version>

或:
  MSKRTU_VERSION=<version> bash script/mk_rtu_image.sh
EOF
  exit 1
}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "缺少命令: $1"
}

VERSION="${1:-${MSKRTU_VERSION:-}}"
if [[ -z "${VERSION}" ]]; then
  usage
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
RTU_ROOT="${PROJECT_ROOT}/MskRTU"
OUTPUT_DIR="${PROJECT_ROOT}/images"
IMAGE_TAG="mskrtu:${VERSION}"
OUTPUT_TAR="${OUTPUT_DIR}/mskrtu-${VERSION}.tar"
BUILD_CTX=""

cleanup() {
  if [[ -n "${BUILD_CTX}" && -d "${BUILD_CTX}" ]]; then
    rm -rf "${BUILD_CTX}"
  fi
}
trap cleanup EXIT

require_cmd podman
if ! podman buildx version >/dev/null 2>&1; then
  die "podman buildx 不可用，请先安装并初始化 buildx"
fi

[[ -d "${RTU_ROOT}/lib" ]] || die "目录不存在: ${RTU_ROOT}/lib"
[[ -d "${RTU_ROOT}/mnt/megsky/bin" ]] || die "目录不存在: ${RTU_ROOT}/mnt/megsky/bin"

for name in MskBase MskCore MskRtu Msk61850; do
  [[ -f "${RTU_ROOT}/mnt/megsky/bin/${name}" ]] || die "缺少可执行文件: ${RTU_ROOT}/mnt/megsky/bin/${name}"
done

BUILD_CTX="$(mktemp -d)"
log "创建临时构建目录: ${BUILD_CTX}"

cp -a "${RTU_ROOT}/lib" "${BUILD_CTX}/"
cp -a "${RTU_ROOT}/mnt" "${BUILD_CTX}/"

for name in MskBase MskCore MskRtu Msk61850; do
  chmod +x "${BUILD_CTX}/mnt/megsky/bin/${name}"
done

cat > "${BUILD_CTX}/start_mskrtu.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

log() {
  echo "[start_mskrtu] $*"
}

stop_all() {
  local pids=("$@")
  for pid in "${pids[@]}"; do
    [[ -n "${pid}" ]] || continue
    if kill -0 "${pid}" 2>/dev/null; then
      kill "${pid}" 2>/dev/null || true
    fi
  done
  wait "${pids[@]}" 2>/dev/null || true
}

export SL330ADIR="${SL330ADIR:-/mnt/megsky}"
export MSKNETDIR="${MSKNETDIR:-/mnt/megsky}"
export LD_LIBRARY_PATH="/lib:/usr/lib:/lib/arm-linux-gnueabi:/usr/lib/arm-linux-gnueabi${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

if hostname MEGSKY >/dev/null 2>&1; then
  log "已设置系统名称(主机名): MEGSKY"
else
  log "无法在容器内设置主机名，请运行容器时增加参数: --hostname MEGSKY"
fi

bins=(MskBase MskCore MskRtu Msk61850)
pids=()

for name in "${bins[@]}"; do
  bin="/mnt/megsky/bin/${name}"
  if [[ ! -x "${bin}" ]]; then
    log "可执行文件不存在或不可执行: ${bin}"
    exit 1
  fi
done

term_handler() {
  log "收到退出信号，正在停止旧程序进程..."
  stop_all "${pids[@]}"
  exit 0
}
trap term_handler SIGINT SIGTERM

for name in "${bins[@]}"; do
  log "启动进程: ${name}"
  "/mnt/megsky/bin/${name}" &
  pids+=("$!")
  sleep 1
done

log "旧程序进程已全部启动，当前主机名: $(hostname)"
set +e
wait -n "${pids[@]}"
exit_code=$?
set -e
log "检测到子进程退出（退出码: ${exit_code}），正在停止其余进程..."
stop_all "${pids[@]}"
exit "${exit_code}"
EOF
chmod +x "${BUILD_CTX}/start_mskrtu.sh"

cat > "${BUILD_CTX}/Dockerfile" <<'EOF'
FROM --platform=linux/arm/v5 docker.io/arm32v5/debian:buster

WORKDIR /mnt/megsky

COPY lib/ /lib/
COPY mnt/ /mnt/
COPY start_mskrtu.sh /usr/local/bin/start_mskrtu.sh

RUN chmod +x /usr/local/bin/start_mskrtu.sh \
    /mnt/megsky/bin/MskBase \
    /mnt/megsky/bin/MskCore \
    /mnt/megsky/bin/MskRtu \
    /mnt/megsky/bin/Msk61850 \
 && mkdir -p /mnt/megsky/log /mnt/megsky/database \
 && printf 'MEGSKY\n' > /etc/hostname

ENV SL330ADIR=/mnt/megsky \
    MSKNETDIR=/mnt/megsky \
    LD_LIBRARY_PATH=/lib:/usr/lib:/lib/arm-linux-gnueabi:/usr/lib/arm-linux-gnueabi

ENTRYPOINT ["/usr/local/bin/start_mskrtu.sh"]
EOF

mkdir -p "${OUTPUT_DIR}"
if [[ -e "${OUTPUT_TAR}" ]]; then
  log "检测到已有产物，先删除: ${OUTPUT_TAR}"
  rm -f "${OUTPUT_TAR}"
fi

log "开始构建镜像: ${IMAGE_TAG}"
podman buildx build --platform linux/arm/v5 -t "${IMAGE_TAG}" "${BUILD_CTX}"

log "导出镜像到: ${OUTPUT_TAR}"
podman save --format docker-archive -o "${OUTPUT_TAR}" "${IMAGE_TAG}"

IMAGE_ID="$(podman images -q "${IMAGE_TAG}" || true)"
if [[ -n "${IMAGE_ID}" ]]; then
  log "清理本地镜像: ${IMAGE_TAG}"
  if ! podman rmi "${IMAGE_TAG}" >/dev/null 2>&1; then
    log "提示: 本地镜像删除失败，可能被占用，可稍后手动清理"
  fi
fi

log "完成，已生成: ${OUTPUT_TAR}"
