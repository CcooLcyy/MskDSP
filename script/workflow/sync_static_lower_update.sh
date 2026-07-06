#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'USAGE'
用法:
  bash script/workflow/sync_static_lower_update.sh \
    --package-file images/mskdsp-xxx \
    --checksum-file SHA256SUMS \
    --manifest-file latest.json \
    --channel stable \
    --platform linux-arm64 \
    --base-url https://update.example/mskdsp-lower \
    --ssh-host host \
    --ssh-port 22 \
    --ssh-user user \
    --remote-root /var/www/update/mskdsp-lower
USAGE
  exit 1
}

die() {
  echo "错误: $*" >&2
  exit 1
}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "缺少命令: $1"
}

validate_path_part() {
  local name="$1"
  local value="$2"
  [[ -n "${value}" ]] || die "${name} 不能为空"
  [[ "${value}" =~ ^[A-Za-z0-9._-]+$ ]] || die "${name} 只能包含字母、数字、点、下划线和短横线: ${value}"
}

PACKAGE_FILE=""
CHECKSUM_FILE=""
MANIFEST_FILE=""
CHANNEL=""
PLATFORM="linux-arm64"
BASE_URL=""
SSH_HOST=""
SSH_PORT="22"
SSH_USER=""
REMOTE_ROOT=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --package-file)
      PACKAGE_FILE="${2:-}"
      shift 2
      ;;
    --checksum-file)
      CHECKSUM_FILE="${2:-}"
      shift 2
      ;;
    --manifest-file)
      MANIFEST_FILE="${2:-}"
      shift 2
      ;;
    --channel)
      CHANNEL="${2:-}"
      shift 2
      ;;
    --platform)
      PLATFORM="${2:-}"
      shift 2
      ;;
    --base-url)
      BASE_URL="${2:-}"
      shift 2
      ;;
    --ssh-host)
      SSH_HOST="${2:-}"
      shift 2
      ;;
    --ssh-port)
      SSH_PORT="${2:-}"
      shift 2
      ;;
    --ssh-user)
      SSH_USER="${2:-}"
      shift 2
      ;;
    --remote-root)
      REMOTE_ROOT="${2:-}"
      shift 2
      ;;
    -h|--help)
      usage
      ;;
    *)
      die "未知参数: $1"
      ;;
  esac
done

[[ -f "${PACKAGE_FILE}" ]] || die "未找到安装包: ${PACKAGE_FILE}"
[[ -f "${CHECKSUM_FILE}" ]] || die "未找到校验文件: ${CHECKSUM_FILE}"
[[ -f "${MANIFEST_FILE}" ]] || die "未找到清单文件: ${MANIFEST_FILE}"
[[ -n "${BASE_URL}" ]] || die "base-url 不能为空"
[[ -n "${SSH_HOST}" ]] || die "ssh-host 不能为空"
[[ -n "${SSH_PORT}" ]] || die "ssh-port 不能为空"
[[ -n "${SSH_USER}" ]] || die "ssh-user 不能为空"
[[ -n "${REMOTE_ROOT}" ]] || die "remote-root 不能为空"
[[ -n "${STATIC_UPDATE_SSH_KEY:-}" ]] || die "STATIC_UPDATE_SSH_KEY 不能为空"

validate_path_part "channel" "${CHANNEL}"
validate_path_part "platform" "${PLATFORM}"

if [[ "${REMOTE_ROOT}" == *"'"* ]]; then
  die "remote-root 不能包含单引号"
fi

require_cmd ssh
require_cmd scp
require_cmd curl
require_cmd python3

TMP_DIR="$(mktemp -d)"
KEY_PATH="${TMP_DIR}/mskdsp-lower-static-update-key"
trap 'rm -rf "${TMP_DIR}"' EXIT

printf '%s\n' "${STATIC_UPDATE_SSH_KEY//$'\r'/}" > "${KEY_PATH}"
chmod 600 "${KEY_PATH}"

TARGET="${SSH_USER}@${SSH_HOST}"
REMOTE_CHANNEL_DIR="${REMOTE_ROOT%/}/${CHANNEL}"
REMOTE_ASSET_DIR="${REMOTE_CHANNEL_DIR}/${PLATFORM}"
NORMALIZED_BASE_URL="${BASE_URL%/}"
MANIFEST_URL="${NORMALIZED_BASE_URL}/${CHANNEL}/latest.json"

SSH_OPTIONS=(
  -i "${KEY_PATH}"
  -p "${SSH_PORT}"
  -o StrictHostKeyChecking=accept-new
  -o IdentitiesOnly=yes
)
SCP_OPTIONS=(
  -i "${KEY_PATH}"
  -P "${SSH_PORT}"
  -o StrictHostKeyChecking=accept-new
  -o IdentitiesOnly=yes
)

echo "准备远端下位机静态更新目录: ${REMOTE_ASSET_DIR}"
ssh "${SSH_OPTIONS[@]}" "${TARGET}" "mkdir -p '${REMOTE_ASSET_DIR}' '${REMOTE_CHANNEL_DIR}'"

echo "上传下位机安装包和校验文件"
scp "${SCP_OPTIONS[@]}" "${PACKAGE_FILE}" "${CHECKSUM_FILE}" "${TARGET}:${REMOTE_ASSET_DIR}/"

echo "最后上传 latest.json"
scp "${SCP_OPTIONS[@]}" "${MANIFEST_FILE}" "${TARGET}:${REMOTE_CHANNEL_DIR}/latest.json"

echo "校验静态清单可访问: ${MANIFEST_URL}"
curl -fsSL "${MANIFEST_URL}" -o "${TMP_DIR}/latest.json" >/dev/null

ASSET_URL="$(
  python3 - "${MANIFEST_FILE}" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as fh:
    manifest = json.load(fh)
print(manifest["asset"]["url"])
PY
)"
CHECKSUM_URL="$(
  python3 - "${MANIFEST_FILE}" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as fh:
    manifest = json.load(fh)
print(manifest["checksum"]["url"])
PY
)"

echo "校验安装包 URL 可访问: ${ASSET_URL}"
curl -fsIL "${ASSET_URL}" >/dev/null

echo "校验校验文件 URL 可访问: ${CHECKSUM_URL}"
curl -fsIL "${CHECKSUM_URL}" >/dev/null

echo "下位机静态更新源同步完成: ${CHANNEL}/${PLATFORM}"
