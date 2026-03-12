#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "用法: $0 [镜像tar包路径|版本号]" >&2
  exit 1
}

die() {
  echo "Error: $*" >&2
  exit 1
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

INPUT_ARG=""
if [[ $# -eq 0 ]]; then
  VERSION_FILE="${PROJECT_ROOT}/VERSION"
  if [[ ! -f "${VERSION_FILE}" ]]; then
    die "未找到版本文件: ${VERSION_FILE}"
  fi
  INPUT_ARG="$(
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
  if [[ -z "${INPUT_ARG}" ]]; then
    die "版本文件内容为空: ${VERSION_FILE}"
  fi
  echo "未传入参数，默认使用版本文件 ${VERSION_FILE} 中的版本: ${INPUT_ARG}"
elif [[ $# -eq 1 ]]; then
  INPUT_ARG="$1"
else
  usage
fi

rm -rf ../package/module
rm -rf ../package/lib
echo "清理旧的 package/module 和 package/lib 目录"
rm -rf ../package/log
rm -rf ../package/socket
echo "清理旧的 package/log 和 package/socket 目录"
rm -rf ../package/debug
echo "清理旧的 package/debug 目录"

OUTPUT_PATH="${PROJECT_ROOT}/images/mskdsp"

INPUT_TAR=""
if [[ -f "${INPUT_ARG}" ]]; then
  INPUT_TAR="${INPUT_ARG}"
else
  if [[ "${INPUT_ARG}" == *"/"* || "${INPUT_ARG}" == *.tar ]]; then
    die "Input tar not found: ${INPUT_ARG}"
  fi
  VERSION="${INPUT_ARG}"
  MAKE_IMAGE_SCRIPT="${SCRIPT_DIR}/make_image.sh"
  if [[ ! -f "${MAKE_IMAGE_SCRIPT}" ]]; then
    die "make_image.sh not found: ${MAKE_IMAGE_SCRIPT}"
  fi
  bash "${MAKE_IMAGE_SCRIPT}" "${VERSION}"
  INPUT_TAR="${PROJECT_ROOT}/images/mskdsp-${VERSION}.tar"
  if [[ ! -f "${INPUT_TAR}" ]]; then
    die "Generated tar not found: ${INPUT_TAR}"
  fi
fi

mkdir -p "$(dirname "${OUTPUT_PATH}")"

cat > "${OUTPUT_PATH}" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

SCRIPT_NAME="$(basename "$0")"
CONTAINER_NAME="mskdsp"
HOST_DIR="/data/mskdsp"
CONTAINER_DIR="/opt/mskdsp"
IMAGE_TAG=""
IMAGE_ID=""
PAYLOAD_TAR=""
PAYLOAD_TMP_DIR=""
NEED_UPDATE=0
CLEANUP_REPO=0
IMAGE_REPO=""

usage() {
  cat <<USAGE
Usage: ${SCRIPT_NAME} start [-- <docker run extra args>]
       ${SCRIPT_NAME} stop
USAGE
}

die() {
  echo "Error: $*" >&2
  exit 1
}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "Missing command: $1"
}

cleanup_payload() {
  if [[ -n "${PAYLOAD_TAR}" && -f "${PAYLOAD_TAR}" ]]; then
    rm -f "${PAYLOAD_TAR}"
  fi
  if [[ -n "${PAYLOAD_TMP_DIR}" ]]; then
    rmdir "${PAYLOAD_TMP_DIR}" 2>/dev/null || true
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
    die "Payload marker not found."
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
        mf = tf.extractfile("manifest.json")
    except KeyError:
        mf = None
    if mf is None:
        sys.exit("manifest.json not found in tar")
    manifest = json.load(mf)

if not isinstance(manifest, list) or not manifest:
    sys.exit("invalid manifest")

entry = manifest[0]
config = entry.get("Config") or ""
repo_tags = entry.get("RepoTags") or []
repo_tag = repo_tags[0] if repo_tags else ""

print(config)
print(repo_tag)
PY
)"
  then
    die "Failed to parse manifest.json"
  fi

  local config
  local repo_tag
  config="$(printf '%s\n' "${parsed}" | sed -n '1p')"
  repo_tag="$(printf '%s\n' "${parsed}" | sed -n '2p')"
  if [[ -z "${config}" || -z "${repo_tag}" ]]; then
    die "manifest.json missing Config or RepoTags"
  fi

  IMAGE_TAG="${repo_tag}"
  IMAGE_ID="sha256:${config%.json}"
  IMAGE_REPO="${IMAGE_TAG%:*}"
}

load_image_if_needed() {
  extract_payload
  parse_manifest "${PAYLOAD_TAR}"

  local existing_id
  existing_id="$(docker images -q --no-trunc "${IMAGE_TAG}" 2>/dev/null | head -n1 || true)"
  if [[ -n "${existing_id}" && "${existing_id}" == "${IMAGE_ID}" ]]; then
    cleanup_payload
    NEED_UPDATE=0
    return 0
  fi

  docker load -i "${PAYLOAD_TAR}" >/dev/null
  CLEANUP_REPO=1
  local new_id
  new_id="$(docker images -q --no-trunc "${IMAGE_TAG}" 2>/dev/null | head -n1 || true)"
  if [[ -n "${existing_id}" && -n "${new_id}" && "${existing_id}" != "${new_id}" ]]; then
    docker rmi "${existing_id}" >/dev/null 2>&1 || true
  fi

  NEED_UPDATE=1
  cleanup_payload
}

cleanup_repo_tags() {
  if [[ "${CLEANUP_REPO}" -ne 1 || -z "${IMAGE_REPO}" ]]; then
    return 0
  fi
  while IFS= read -r tag; do
    [[ -n "${tag}" ]] || continue
    if [[ "${tag}" != "${IMAGE_TAG}" ]]; then
      docker rmi "${tag}" >/dev/null 2>&1 || true
    fi
  done < <(docker images --format '{{.Repository}}:{{.Tag}}' "${IMAGE_REPO}" 2>/dev/null | sort -u)
}

ensure_module_dir() {
  local module_dir="${HOST_DIR}/module"
  local need_copy_module=0

  if [[ "${NEED_UPDATE}" -eq 1 ]]; then
    if [[ -d "${module_dir}" ]]; then
      find "${module_dir}" -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null || true
    fi
    need_copy_module=1
  else
    if [[ -z "$(ls -A "${module_dir}" 2>/dev/null || true)" ]]; then
      need_copy_module=1
    fi
  fi

  if [[ "${need_copy_module}" -eq 0 ]]; then
    return 0
  fi

  mkdir -p "${module_dir}"
  if [[ "${NEED_UPDATE}" -eq 1 ]]; then
    echo "镜像已更新，重新同步模块目录到宿主机: ${module_dir}"
  else
    echo "模块目录为空，初始化模块到宿主机: ${module_dir}"
  fi
  local init_name="mskdsp-init-$$"
  docker create --name "${init_name}" "${IMAGE_TAG}" >/dev/null
  if ! docker cp "${init_name}:${CONTAINER_DIR}/module/." "${module_dir}/"; then
    docker rm -f "${init_name}" >/dev/null 2>&1 || true
    die "模块目录初始化失败: ${module_dir}"
  fi
  docker rm "${init_name}" >/dev/null
}

ensure_default_conf() {
  local conf_dir="${HOST_DIR}/conf"
  if [[ -d "${conf_dir}" && -n "$(ls -A "${conf_dir}" 2>/dev/null || true)" ]]; then
    return 0
  fi
  mkdir -p "${conf_dir}"
  echo "配置目录为空，初始化默认配置到宿主机: ${conf_dir}"
  local init_name="mskdsp-init-$$"
  docker create --name "${init_name}" "${IMAGE_TAG}" >/dev/null
  if ! docker cp "${init_name}:${CONTAINER_DIR}/conf/." "${conf_dir}/"; then
    docker rm -f "${init_name}" >/dev/null 2>&1 || true
    die "初始化配置目录失败: ${conf_dir}"
  fi
  docker rm "${init_name}" >/dev/null
}

ensure_log_dir() {
  local log_dir="${HOST_DIR}/log"
  if [[ -d "${log_dir}" ]]; then
    return 0
  fi
  mkdir -p "${log_dir}"
  echo "已创建日志目录: ${log_dir}"
}

start_container() {
  require_cmd docker
  require_cmd python3
  trap cleanup_payload EXIT

  load_image_if_needed
  cleanup_repo_tags
  ensure_module_dir
  ensure_default_conf
  ensure_log_dir

  docker rm -f "${CONTAINER_NAME}" >/dev/null 2>&1 || true
  if [[ "${1:-}" == "--" ]]; then
    shift
  fi
  echo "运行容器仅映射 conf/module/log 目录"
  docker run -d \
    --name "${CONTAINER_NAME}" \
    --privileged \
    --restart unless-stopped \
    --network host \
    -v "${HOST_DIR}/conf:${CONTAINER_DIR}/conf" \
    -v "${HOST_DIR}/module:${CONTAINER_DIR}/module" \
    -v "${HOST_DIR}/log:${CONTAINER_DIR}/log" \
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
EOF

cat "${INPUT_TAR}" >> "${OUTPUT_PATH}"
chmod +x "${OUTPUT_PATH}"

echo "Generated ${OUTPUT_PATH}"
