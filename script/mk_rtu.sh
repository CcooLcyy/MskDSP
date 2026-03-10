#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "用法: $0 <image-tar|version>" >&2
  exit 1
}

die() {
  echo "[mk_rtu] 错误: $*" >&2
  exit 1
}

log() {
  echo "[mk_rtu] $*"
}

if [[ $# -ne 1 ]]; then
  usage
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTPUT_PATH="${PROJECT_ROOT}/images/mskrtu"

INPUT_ARG="$1"
INPUT_TAR=""
if [[ -f "${INPUT_ARG}" ]]; then
  INPUT_TAR="${INPUT_ARG}"
else
  if [[ "${INPUT_ARG}" == *"/"* || "${INPUT_ARG}" == *.tar ]]; then
    die "未找到镜像 tar: ${INPUT_ARG}"
  fi
  VERSION="${INPUT_ARG}"
  MAKE_IMAGE_SCRIPT="${SCRIPT_DIR}/mk_rtu_image.sh"
  if [[ ! -f "${MAKE_IMAGE_SCRIPT}" ]]; then
    die "未找到 mk_rtu_image.sh: ${MAKE_IMAGE_SCRIPT}"
  fi
  log "根据版本号生成镜像 tar: ${VERSION}"
  bash "${MAKE_IMAGE_SCRIPT}" "${VERSION}"
  INPUT_TAR="${PROJECT_ROOT}/images/mskrtu-${VERSION}.tar"
  if [[ ! -f "${INPUT_TAR}" ]]; then
    die "生成后的镜像 tar 不存在: ${INPUT_TAR}"
  fi
fi

mkdir -p "$(dirname "${OUTPUT_PATH}")"

cat > "${OUTPUT_PATH}" <<'PAYLOAD_SCRIPT'
#!/usr/bin/env bash
set -euo pipefail

SCRIPT_NAME="$(basename "$0")"
CONTAINER_NAME="mskrtu"
HOST_DIR="/data/mskrtu"
CONTAINER_DIR="/mnt/megsky"
PAYLOAD_TAR=""
PAYLOAD_TMP_DIR=""
IMAGE_TAG=""
IMAGE_ID=""
IMAGE_REPO=""

usage() {
  cat <<USAGE
用法: ${SCRIPT_NAME} start [-- <docker run 额外参数>]
      ${SCRIPT_NAME} stop
USAGE
}

die() {
  echo "[mk_rtu_runner] 错误: $*" >&2
  exit 1
}

log() {
  echo "[mk_rtu_runner] $*"
}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "缺少命令: $1"
}

cleanup_payload() {
  if [[ -n "${PAYLOAD_TAR}" && -f "${PAYLOAD_TAR}" ]]; then
    rm -f "${PAYLOAD_TAR}"
  fi
  if [[ -n "${PAYLOAD_TMP_DIR}" && -d "${PAYLOAD_TMP_DIR}" ]]; then
    rm -rf "${PAYLOAD_TMP_DIR}"
  fi
  PAYLOAD_TAR=""
  PAYLOAD_TMP_DIR=""
}

payload_start_line() {
  awk '/^__ARCHIVE_BELOW__$/ {print NR+1; exit 0;}' "$0"
}

extract_payload() {
  local start_line
  start_line="$(payload_start_line)"
  if [[ -z "${start_line}" ]]; then
    die "未找到镜像载荷标记"
  fi
  PAYLOAD_TMP_DIR="$(mktemp -d)"
  PAYLOAD_TAR="${PAYLOAD_TMP_DIR}/image.tar"
  tail -n +"${start_line}" "$0" > "${PAYLOAD_TAR}"
}

parse_manifest() {
  local tar_path="$1"
  local parsed
  if ! parsed="$(python3 - "${tar_path}" <<'PY'
import json
import sys
import tarfile

tar_path = sys.argv[1]
with tarfile.open(tar_path, "r:*") as tf:
    try:
        manifest_file = tf.extractfile("manifest.json")
    except KeyError:
        manifest_file = None
    if manifest_file is None:
        sys.exit("manifest.json not found")
    manifest = json.load(manifest_file)

if not isinstance(manifest, list) or not manifest:
    sys.exit("invalid manifest")

entry = manifest[0]
config = entry.get("Config") or ""
repo_tags = entry.get("RepoTags") or []
repo_tag = repo_tags[0] if repo_tags else ""

print(config)
print(repo_tag)
PY
)"; then
    die "解析镜像 tar 失败"
  fi

  local config
  local repo_tag
  config="$(printf '%s\n' "${parsed}" | sed -n '1p')"
  repo_tag="$(printf '%s\n' "${parsed}" | sed -n '2p')"
  if [[ -z "${config}" || -z "${repo_tag}" ]]; then
    die "manifest.json 中缺少 Config 或 RepoTags"
  fi

  IMAGE_TAG="${repo_tag}"
  IMAGE_ID="sha256:${config%.json}"
  IMAGE_REPO="${IMAGE_TAG%:*}"
}

remove_same_tag_image() {
  docker rm -f "${CONTAINER_NAME}" >/dev/null 2>&1 || true
  if docker image inspect "${IMAGE_TAG}" >/dev/null 2>&1; then
    log "检测到同 tag 镜像，先删除: ${IMAGE_TAG}"
    if ! docker rmi "${IMAGE_TAG}" >/dev/null; then
      die "删除同 tag 镜像失败，请先确认没有其他容器占用: ${IMAGE_TAG}"
    fi
  fi
}

load_payload_image() {
  extract_payload
  parse_manifest "${PAYLOAD_TAR}"
  remove_same_tag_image
  log "开始导入镜像: ${IMAGE_TAG}"
  docker load -i "${PAYLOAD_TAR}" >/dev/null
  cleanup_payload
}

make_init_container() {
  local init_name="mskrtu-init-$$"
  docker create --platform linux/arm/v5 --name "${init_name}" "${IMAGE_TAG}" >/dev/null
  printf '%s\n' "${init_name}"
}

sync_dir_if_empty() {
  local source_dir="$1"
  local target_dir="$2"
  mkdir -p "${target_dir}"
  if [[ -n "$(ls -A "${target_dir}" 2>/dev/null || true)" ]]; then
    log "宿主机目录已存在，保留原内容: ${target_dir}"
    return 0
  fi
  cp -a "${source_dir}/." "${target_dir}/"
  log "已初始化目录到宿主机: ${target_dir}"
}

sync_file_if_missing() {
  local source_file="$1"
  local target_file="$2"
  if [[ -e "${target_file}" ]]; then
    log "宿主机文件已存在，保留原内容: ${target_file}"
    return 0
  fi
  cp -a "${source_file}" "${target_file}"
  log "已初始化文件到宿主机: ${target_file}"
}

prepare_host_content() {
  local init_name
  local init_tmp
  init_name="$(make_init_container)"
  init_tmp="$(mktemp -d)"

  mkdir -p "${init_tmp}/lib" "${init_tmp}/megsky"
  if ! docker cp "${init_name}:/lib/." "${init_tmp}/lib/"; then
    docker rm -f "${init_name}" >/dev/null 2>&1 || true
    rm -rf "${init_tmp}"
    die "从镜像复制 /lib 失败"
  fi
  if ! docker cp "${init_name}:${CONTAINER_DIR}/." "${init_tmp}/megsky/"; then
    docker rm -f "${init_name}" >/dev/null 2>&1 || true
    rm -rf "${init_tmp}"
    die "从镜像复制 ${CONTAINER_DIR} 失败"
  fi
  docker rm -f "${init_name}" >/dev/null 2>&1 || true

  mkdir -p "${HOST_DIR}"
  sync_dir_if_empty "${init_tmp}/lib" "${HOST_DIR}/lib"

  shopt -s nullglob dotglob
  local entry
  for entry in "${init_tmp}/megsky"/*; do
    local base_name
    base_name="$(basename "${entry}")"
    if [[ -d "${entry}" ]]; then
      sync_dir_if_empty "${entry}" "${HOST_DIR}/${base_name}"
    elif [[ -f "${entry}" ]]; then
      sync_file_if_missing "${entry}" "${HOST_DIR}/${base_name}"
    fi
  done
  shopt -u nullglob dotglob

  rm -rf "${init_tmp}"
}

collect_mount_args() {
  local mount_args=()
  mount_args+=("-v" "${HOST_DIR}/lib:/lib")

  shopt -s nullglob dotglob
  local entry
  for entry in "${HOST_DIR}"/*; do
    local base_name
    base_name="$(basename "${entry}")"
    if [[ "${base_name}" == "lib" ]]; then
      continue
    fi
    if [[ -d "${entry}" ]]; then
      mount_args+=("-v" "${entry}:${CONTAINER_DIR}/${base_name}")
    elif [[ -f "${entry}" ]]; then
      mount_args+=("-v" "${entry}:${CONTAINER_DIR}/${base_name}")
    fi
  done
  shopt -u nullglob dotglob

  printf '%s\n' "${mount_args[@]}"
}

start_container() {
  require_cmd docker
  require_cmd python3
  trap cleanup_payload EXIT

  load_payload_image
  prepare_host_content

  docker rm -f "${CONTAINER_NAME}" >/dev/null 2>&1 || true
  if [[ "${1:-}" == "--" ]]; then
    shift
  fi

  local mount_args=()
  while IFS= read -r line; do
    [[ -n "${line}" ]] || continue
    mount_args+=("${line}")
  done < <(collect_mount_args)

  log "以可读写方式覆盖宿主机 lib 目录: ${HOST_DIR}/lib -> /lib"
  log "已映射 ${CONTAINER_DIR} 下所有宿主机目录和顶层文件"
  docker run -d \
    --name "${CONTAINER_NAME}" \
    --platform linux/arm/v5 \
    --hostname MEGSKY \
    --privileged \
    --restart unless-stopped \
    --network host \
    "${mount_args[@]}" \
    "$@" \
    "${IMAGE_TAG}"
}

stop_container() {
  require_cmd docker
  docker rm -f "${CONTAINER_NAME}" >/dev/null 2>&1 || true
}

main() {
  local cmd="${1:-}"
  if [[ -z "${cmd}" ]]; then
    usage
    exit 1
  fi
  shift || true
  case "${cmd}" in
    start)
      start_container "$@"
      ;;
    stop)
      stop_container
      ;;
    *)
      usage
      exit 1
      ;;
  esac
}

main "$@"
exit 0
__ARCHIVE_BELOW__
PAYLOAD_SCRIPT

cat "${INPUT_TAR}" >> "${OUTPUT_PATH}"
chmod +x "${OUTPUT_PATH}"

log "已生成自解压脚本: ${OUTPUT_PATH}"
