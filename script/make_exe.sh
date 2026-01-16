#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: $0 <image-tar|version>" >&2
  exit 1
}

die() {
  echo "Error: $*" >&2
  exit 1
}

if [[ $# -ne 1 ]]; then
  usage
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTPUT_PATH="${PROJECT_ROOT}/images/mskdsp"

INPUT_ARG="$1"
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

update_host_artifacts() {
  mkdir -p "${HOST_DIR}/lib"

  local need_copy_mskdsp=0
  local need_copy_lib=0

  if [[ "${NEED_UPDATE}" -eq 1 ]]; then
    rm -f "${HOST_DIR}/MskDSP"
    find "${HOST_DIR}/lib" -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null || true
    need_copy_mskdsp=1
    need_copy_lib=1
  else
    if [[ ! -f "${HOST_DIR}/MskDSP" ]]; then
      need_copy_mskdsp=1
    fi
    if [[ -z "$(ls -A "${HOST_DIR}/lib" 2>/dev/null || true)" ]]; then
      need_copy_lib=1
    fi
  fi

  if [[ "${need_copy_mskdsp}" -eq 0 && "${need_copy_lib}" -eq 0 ]]; then
    return 0
  fi

  local init_name="mskdsp-init-$$"
  docker create --name "${init_name}" "${IMAGE_TAG}" >/dev/null
  if [[ "${need_copy_mskdsp}" -eq 1 ]]; then
    if ! docker cp "${init_name}:${CONTAINER_DIR}/MskDSP" "${HOST_DIR}/MskDSP"; then
      docker rm -f "${init_name}" >/dev/null 2>&1 || true
      die "Failed to update ${HOST_DIR}/MskDSP"
    fi
  fi
  if [[ "${need_copy_lib}" -eq 1 ]]; then
    if ! docker cp "${init_name}:${CONTAINER_DIR}/lib/." "${HOST_DIR}/lib/"; then
      docker rm -f "${init_name}" >/dev/null 2>&1 || true
      die "Failed to update ${HOST_DIR}/lib"
    fi
  fi
  docker rm "${init_name}" >/dev/null
}

ensure_default_conf() {
  local conf_dir="${HOST_DIR}/conf"
  if [[ -d "${conf_dir}" && -n "$(ls -A "${conf_dir}" 2>/dev/null || true)" ]]; then
    return 0
  fi
  mkdir -p "${conf_dir}"
  local init_name="mskdsp-init-$$"
  docker create --name "${init_name}" "${IMAGE_TAG}" >/dev/null
  if ! docker cp "${init_name}:${CONTAINER_DIR}/conf/." "${conf_dir}/"; then
    docker rm -f "${init_name}" >/dev/null 2>&1 || true
    die "Failed to initialize ${conf_dir}"
  fi
  docker rm "${init_name}" >/dev/null
}

start_container() {
  require_cmd docker
  require_cmd python3
  trap cleanup_payload EXIT

  load_image_if_needed
  cleanup_repo_tags
  update_host_artifacts
  ensure_default_conf

  docker rm -f "${CONTAINER_NAME}" >/dev/null 2>&1 || true
  if [[ "${1:-}" == "--" ]]; then
    shift
  fi
  docker run -d \
    --name "${CONTAINER_NAME}" \
    --privileged \
    --restart unless-stopped \
    --network host \
    -v "${HOST_DIR}:${CONTAINER_DIR}" \
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
