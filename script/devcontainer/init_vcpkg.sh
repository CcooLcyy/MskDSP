#!/usr/bin/env bash
set -euo pipefail

log() {
  printf '[init_vcpkg] %s\n' "$*"
}

fail() {
  printf '[init_vcpkg] 错误：%s\n' "$*" >&2
  exit 1
}

REPO_ROOT="${MSKDSP_REPO_ROOT:-/data/code/mskdsp}"
CONFIG_FILE="${REPO_ROOT}/vcpkg-configuration.json"
VCPKG_ROOT="${VCPKG_ROOT:-/data/3rdlibs/vcpkg}"
VCPKG_DOWNLOADS="${VCPKG_DOWNLOADS:-/root/.cache/vcpkg/downloads}"
VCPKG_DEFAULT_BINARY_CACHE="${VCPKG_DEFAULT_BINARY_CACHE:-/root/.cache/vcpkg/archives}"
CCACHE_DIR="${CCACHE_DIR:-/root/.cache/ccache}"
VCPKG_REPOSITORY="https://github.com/microsoft/vcpkg.git"
VCPKG_TOOL_PATCH_MARKER=".mskdsp-vcpkg-tool-patched"
CODEX_HOME="${CODEX_HOME:-/root/.codex}"
CODEX_SYNC_ROOT="${CODEX_SYNC_ROOT:-/codex-sync}"

export VCPKG_ROOT
export VCPKG_DOWNLOADS
export VCPKG_DEFAULT_BINARY_CACHE
export CCACHE_DIR
export VCPKG_DISABLE_METRICS=1
export GIT_TERMINAL_PROMPT=0
export CODEX_HOME
export CODEX_SYNC_ROOT

[ -f "${CONFIG_FILE}" ] || fail "未找到 ${CONFIG_FILE}，无法确定 vcpkg baseline"

VCPKG_BASELINE="$(
  python3 - "${CONFIG_FILE}" <<'PY'
import json
import sys

config_path = sys.argv[1]
with open(config_path, "r", encoding="utf-8") as fh:
    config = json.load(fh)

baseline = config.get("default-registry", {}).get("baseline", "").strip()
if not baseline:
    raise SystemExit(1)

print(baseline)
PY
)" || fail "解析 ${CONFIG_FILE} 中的 default-registry.baseline 失败"

log "目标 vcpkg baseline: ${VCPKG_BASELINE}"
mkdir -p "${VCPKG_ROOT}" "${VCPKG_DOWNLOADS}" "${VCPKG_DEFAULT_BINARY_CACHE}" "${CCACHE_DIR}"

clear_vcpkg_root() {
  find "${VCPKG_ROOT}" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +
}

link_codex_sync_entry() {
  local relative_path="$1"
  local source_path="${CODEX_SYNC_ROOT}/${relative_path}"
  local target_path="${CODEX_HOME}/${relative_path}"

  [ -e "${source_path}" ] || return 0

  mkdir -p "$(dirname "${target_path}")"
  rm -rf "${target_path}"
  ln -s "${source_path}" "${target_path}"
}

sync_codex_auth() {
  local source_path="${CODEX_SYNC_ROOT}/auth.json"
  local target_path="${CODEX_HOME}/auth.json"

  [ -f "${source_path}" ] || return 0

  mkdir -p "${CODEX_HOME}"
  if [ -L "${target_path}" ]; then
    rm -f "${target_path}"
  fi

  if [ ! -f "${target_path}" ] || ! cmp -s "${source_path}" "${target_path}"; then
    install -m 600 "${source_path}" "${target_path}"
  fi
}

prepare_codex_home() {
  [ -d "${CODEX_SYNC_ROOT}" ] || return 0

  mkdir -p "${CODEX_HOME}"
  link_codex_sync_entry "config.toml"
  link_codex_sync_entry "AGENTS.md"
  link_codex_sync_entry "skills"
  sync_codex_auth
  log "已准备 Codex 配置同步：${CODEX_SYNC_ROOT} -> ${CODEX_HOME}"
}

prepare_codex_home

read_tool_metadata() {
  local key="$1"
  local metadata_file="${VCPKG_ROOT}/scripts/vcpkg-tool-metadata.txt"
  sed -n "s/^${key}=//p" "${metadata_file}" | head -n 1
}

build_patched_vcpkg_tool() {
  local tool_tag="$1"
  local tool_sha="$2"
  local tmp_dir
  local source_dir
  local zip_path
  local marker_path="${VCPKG_ROOT}/${VCPKG_TOOL_PATCH_MARKER}"

  tmp_dir="$(mktemp -d /tmp/mskdsp-vcpkg-tool.XXXXXX)"
  source_dir="${tmp_dir}/vcpkg-tool-${tool_tag}"
  zip_path="${tmp_dir}/vcpkg-tool-${tool_tag}.zip"

  cleanup() {
    rm -rf "${tmp_dir}"
  }
  trap cleanup RETURN

  log "检测到当前容器环境可能触发 vcpkg-tool 负耗时崩溃，开始构建本地修补版工具（tag=${tool_tag}）"
  curl -L --fail --retry 3 \
    -o "${zip_path}" \
    "https://github.com/microsoft/vcpkg-tool/archive/${tool_tag}.zip"
  printf '%s  %s\n' "${tool_sha}" "${zip_path}" | sha512sum -c - >/dev/null
  unzip -q "${zip_path}" -d "${tmp_dir}"

  python3 - "${source_dir}/src/vcpkg/metrics.cpp" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
old = """    void MetricsSubmission::track_elapsed_us(double value)
    {
        if (!isfinite(value) || value <= 0.0)
        {
            Checks::unreachable(VCPKG_LINE_INFO);
        }

        elapsed_us = value;
    }
"""
new = """    void MetricsSubmission::track_elapsed_us(double value)
    {
        if (!isfinite(value) || value <= 0.0)
        {
            elapsed_us = 1.0;
            return;
        }

        elapsed_us = value;
    }
"""
if old not in text:
    raise SystemExit("未找到 metrics.cpp 修补目标")
path.write_text(text.replace(old, new, 1), encoding="utf-8")
PY

  cmake -S "${source_dir}" -B "${tmp_dir}/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DVCPKG_DEVELOPMENT_WARNINGS=OFF
  cmake --build "${tmp_dir}/build" --target vcpkg --parallel
  cp "${tmp_dir}/build/vcpkg" "${VCPKG_ROOT}/vcpkg"
  chmod +x "${VCPKG_ROOT}/vcpkg"
  "${VCPKG_ROOT}/vcpkg" version >/dev/null 2>&1 || fail "修补版 vcpkg-tool 构建完成后仍不可用"
  printf '%s\n' "${tool_tag}" > "${marker_path}"
  log "已切换到本地修补版 vcpkg-tool（tag=${tool_tag}）"
}

current_head=""
reuse_existing=0
rebuild_reason=""

if [ -z "$(find "${VCPKG_ROOT}" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null)" ]; then
  rebuild_reason="首次初始化"
elif ! git -C "${VCPKG_ROOT}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  rebuild_reason="baseline 变更重建：检测到 ${VCPKG_ROOT} 不是 git 仓库"
else
  current_head="$(git -C "${VCPKG_ROOT}" rev-parse HEAD 2>/dev/null || true)"
  if [ "${current_head}" != "${VCPKG_BASELINE}" ]; then
    rebuild_reason="baseline 变更重建：当前 HEAD=${current_head:-未知}，目标 baseline=${VCPKG_BASELINE}"
  elif ! "${VCPKG_ROOT}/vcpkg" version >/dev/null 2>&1; then
    rebuild_reason="baseline 变更重建：已命中 baseline，但 vcpkg 工具不可用"
  else
    reuse_existing=1
  fi
fi

if [ "${reuse_existing}" -eq 1 ]; then
  log "已复用现有 vcpkg：${VCPKG_ROOT}（HEAD=${current_head}）"
else
  log "${rebuild_reason}"
  clear_vcpkg_root
  git clone "${VCPKG_REPOSITORY}" "${VCPKG_ROOT}"
  git -C "${VCPKG_ROOT}" checkout "${VCPKG_BASELINE}"
  "${VCPKG_ROOT}/bootstrap-vcpkg.sh" -disableMetrics
  current_head="$(git -C "${VCPKG_ROOT}" rev-parse HEAD)"
  [ "${current_head}" = "${VCPKG_BASELINE}" ] || fail "vcpkg 初始化后 HEAD=${current_head}，与 baseline=${VCPKG_BASELINE} 不一致"
  "${VCPKG_ROOT}/vcpkg" version >/dev/null 2>&1 || fail "vcpkg 初始化完成后工具仍不可用"
  log "vcpkg 初始化完成：${VCPKG_ROOT}（HEAD=${current_head}）"
fi

tool_tag="$(read_tool_metadata VCPKG_TOOL_RELEASE_TAG)"
tool_sha="$(read_tool_metadata VCPKG_TOOL_SOURCE_SHA)"
[ -n "${tool_tag}" ] || fail "读取 vcpkg-tool release tag 失败"
[ -n "${tool_sha}" ] || fail "读取 vcpkg-tool source sha 失败"

if [ ! -f "${VCPKG_ROOT}/${VCPKG_TOOL_PATCH_MARKER}" ] || \
   [ "$(cat "${VCPKG_ROOT}/${VCPKG_TOOL_PATCH_MARKER}" 2>/dev/null || true)" != "${tool_tag}" ]; then
  build_patched_vcpkg_tool "${tool_tag}" "${tool_sha}"
else
  log "已复用本地修补版 vcpkg-tool（tag=${tool_tag}）"
fi

exec "$@"
