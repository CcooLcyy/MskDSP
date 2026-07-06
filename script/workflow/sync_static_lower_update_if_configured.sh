#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
ORIGINAL_ARGS=("$@")

BASE_URL=""
SSH_HOST=""
SSH_PORT=""
SSH_USER=""
REMOTE_ROOT=""

while [[ $# -gt 0 ]]; do
  case "$1" in
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
    --package-file|--checksum-file|--manifest-file|--channel|--platform)
      shift 2
      ;;
    -h|--help)
      exec bash "${SCRIPT_DIR}/sync_static_lower_update.sh" "${ORIGINAL_ARGS[@]}"
      ;;
    *)
      shift
      ;;
  esac
done

missing=()
[[ -n "${BASE_URL}" ]] || missing+=("STATIC_UPDATE_BASE_URL")
[[ -n "${SSH_HOST}" ]] || missing+=("STATIC_UPDATE_SSH_HOST")
[[ -n "${SSH_PORT}" ]] || missing+=("STATIC_UPDATE_SSH_PORT")
[[ -n "${SSH_USER}" ]] || missing+=("STATIC_UPDATE_SSH_USER")
[[ -n "${REMOTE_ROOT}" ]] || missing+=("STATIC_UPDATE_REMOTE_ROOT")
[[ -n "${STATIC_UPDATE_SSH_KEY:-}" ]] || missing+=("STATIC_UPDATE_SSH_KEY")

if [[ ${#missing[@]} -gt 0 ]]; then
  joined="$(IFS=,; echo "${missing[*]}")"
  echo "::warning::跳过下位机静态更新源同步，缺少发布配置: ${joined}。构建产物仍会上传到 workflow artifact。"
  exit 0
fi

exec bash "${SCRIPT_DIR}/sync_static_lower_update.sh" "${ORIGINAL_ARGS[@]}"
