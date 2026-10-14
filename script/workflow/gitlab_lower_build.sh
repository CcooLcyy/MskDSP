#!/usr/bin/env bash
set -euo pipefail
# 凭据仅通过进程环境传给 Git，禁止逐行回显。
set +x

die() { echo "错误: $*" >&2; exit 1; }

finish() {
  local result=$?
  unset GIT_CONFIG_COUNT GIT_CONFIG_KEY_0 GIT_CONFIG_VALUE_0
  if command -v ccache >/dev/null 2>&1; then
    ccache --show-stats || echo "ccache 统计输出失败，跳过收尾统计" >&2
  fi
  exit "${result}"
}
trap finish EXIT

# 整体函数先加载，再切换手动指定的旧分支或旧 tag，避免脚本被 checkout 替换。
main() {
  local project_root driver_sha triplet package_arch strip_debug target_revision
  project_root="$(git rev-parse --show-toplevel)"
  cd "${project_root}"
  driver_sha="$(git rev-parse HEAD)"
  ARCH_SLUG="${ARCH_SLUG:?必须指定 ARCH_SLUG}"
  CHANNEL="${CHANNEL:?必须指定 CHANNEL}"
  MAKE_PACKAGE="${MAKE_PACKAGE:-false}"
  BUILD_PARALLEL="${BUILD_PARALLEL:-2}"
  case "${ARCH_SLUG}" in
    arm64) triplet=arm64-linux-dynamic; package_arch=aarch64; strip_debug=ON ;;
    x64) triplet=x64-linux-dynamic; package_arch=x86-64; strip_debug=OFF ;;
    *) die "不支持的构建架构: ${ARCH_SLUG}" ;;
  esac
  case "${CHANNEL}" in ci|beta|stable) ;; *) die "不支持的渠道: ${CHANNEL}" ;; esac
  [[ "${MAKE_PACKAGE}" == true || "${MAKE_PACKAGE}" == false ]] || die "MAKE_PACKAGE 必须为 true 或 false"
  [[ "${BUILD_PARALLEL}" =~ ^[1-9][0-9]*$ ]] || die "构建并行度必须是正整数"
  [[ "${CHANNEL}" == ci || "${ARCH_SLUG}" == arm64 ]] || die "Beta 和 Stable 与 GitHub 保持一致，仅生成 ARM64 构建"
  TARGET_REF="${TARGET_REF:-${CI_COMMIT_REF_NAME:-}}"
  if [[ "${CHANNEL}" == beta ]]; then
    TARGET_REF="${BETA_REF:-${TARGET_REF}}"
    [[ "${TARGET_REF}" == beta/?* ]] || die "Beta 目标必须是 beta/ 开头的分支"
  elif [[ "${CHANNEL}" == stable ]]; then
    TARGET_REF="${RELEASE_TAG:-${CI_COMMIT_TAG:-${TARGET_REF}}}"
    [[ "${TARGET_REF}" == v?* ]] || die "Stable 目标必须是 v 开头的正式 tag"
  fi
  [[ "${TARGET_REF}" =~ ^[A-Za-z0-9_./-]+$ ]] || die "目标引用包含不支持的字符"
  git check-ref-format "refs/heads/${TARGET_REF}" >/dev/null || die "目标引用格式不合法"
  if [[ "${CHANNEL}" == stable ]]; then
    target_revision="refs/tags/${TARGET_REF}"
  else
    target_revision="refs/heads/${TARGET_REF}"
  fi
  # MR 保留 GitLab 已检出的提交；手动 Beta/Stable 明确检出指定目标。
  if [[ "${CHANNEL}" == stable || ( "${CHANNEL}" == beta && -n "${BETA_REF:-}" ) ]]; then
    echo "检出 ${CHANNEL} 目标引用: ${TARGET_REF}"
    git fetch --no-recurse-submodules origin "${target_revision}"
    git checkout --detach FETCH_HEAD
  fi

  for command in git python3 cmake ctest ninja gcc-14 g++-14 ccache file readelf; do
    command -v "${command}" >/dev/null 2>&1 || die "构建镜像缺少工具: ${command}"
  done
  case "${ARCH_SLUG}:$(uname -m)" in
    arm64:aarch64|arm64:arm64|x64:x86_64|x64:amd64) ;;
    *) die "构建镜像架构与 ARCH_SLUG 不一致，不能执行目标单元测试" ;;
  esac
  BUILD_DIR="build-${ARCH_SLUG}"
  export VCPKG_ROOT="${VCPKG_ROOT:-${project_root}/.cache/vcpkg-root/${ARCH_SLUG}}"
  export VCPKG_DEFAULT_BINARY_CACHE="${project_root}/.cache/vcpkg/${ARCH_SLUG}"
  export VCPKG_BINARY_SOURCES="clear;files,${VCPKG_DEFAULT_BINARY_CACHE},readwrite"
  export VCPKG_OVERLAY_TRIPLETS="${project_root}/cmake/vcpkg-triplets"
  export VCPKG_DOWNLOADS="${project_root}/.cache/vcpkg-downloads"
  export CCACHE_DIR="${project_root}/.cache/ccache/${ARCH_SLUG}"
  export CCACHE_BASEDIR="${project_root}"
  export CCACHE_COMPILERCHECK=content CCACHE_MAXSIZE=1G
  mkdir -p "${VCPKG_DEFAULT_BINARY_CACHE}" "${VCPKG_DOWNLOADS}" "${CCACHE_DIR}"

  echo "使用 GitLab job token 拉取 protobuf 的固定 gitlink 提交"
  [[ -n "${CI_JOB_TOKEN:-}" && -n "${CI_SERVER_URL:-}" ]] || die "缺少 GitLab 子模块访问环境"
  local proto_url
  proto_url="${MSKDSP_PROTO_GITLAB_URL:-${CI_SERVER_URL%/}/mskdsp/mskdspproto.git}"
  [[ "${proto_url}" == "${CI_SERVER_URL%/}/"* ]] || die "protobuf 镜像必须位于当前 GitLab，避免凭据流向其他服务器"
  git submodule sync --recursive
  git config submodule.protobuf.url "${proto_url}"
  export GIT_CONFIG_COUNT=1
  export GIT_CONFIG_KEY_0="http.${CI_SERVER_URL%/}/.extraheader"
  GIT_CONFIG_VALUE_0="$(python3 - <<'PY'
import base64
import os
print("Authorization: Basic " + base64.b64encode(("gitlab-ci-token:" + os.environ["CI_JOB_TOKEN"]).encode()).decode())
PY
  )"
  export GIT_CONFIG_VALUE_0
  git submodule update --init --recursive protobuf
  unset GIT_CONFIG_COUNT GIT_CONFIG_KEY_0 GIT_CONFIG_VALUE_0

  local baseline
  baseline="$(python3 - <<'PY'
import json
import re
with open("vcpkg-configuration.json", encoding="utf-8") as source:
    baseline = json.load(source)["default-registry"]["baseline"]
if not re.fullmatch(r"[0-9a-f]{40}", baseline):
    raise SystemExit("vcpkg baseline 格式不合法")
print(baseline)
PY
  )"
  if [[ ! -d "${VCPKG_ROOT}/.git" ]]; then
    echo "克隆固定 baseline 使用的 vcpkg 源码"
    git clone https://github.com/microsoft/vcpkg.git "${VCPKG_ROOT}"
  fi
  git -C "${VCPKG_ROOT}" cat-file -e "${baseline}^{commit}" 2>/dev/null || git -C "${VCPKG_ROOT}" fetch origin "${baseline}"
  git -C "${VCPKG_ROOT}" checkout --detach "${baseline}"
  "${VCPKG_ROOT}/bootstrap-vcpkg.sh" -disableMetrics

  export ARCH_SLUG CHANNEL MAKE_PACKAGE TARGET_REF
  export SOURCE_SHA="$(git rev-parse HEAD)" DRIVER_SHA="${driver_sha}"
  export PACKAGE_ARCH="${package_arch}"
  echo "生成与 GitHub 一致的构建和交付元数据"
  python3 - <<'PY'
import datetime
import os
import re
import subprocess
from pathlib import Path
from zoneinfo import ZoneInfo

arch, channel, ref = (os.environ[name] for name in ("ARCH_SLUG", "CHANNEL", "TARGET_REF"))
platform = f"linux-{arch}"
version = Path("VERSION").read_text(encoding="utf-8").strip()
now = datetime.datetime.now(ZoneInfo("Asia/Shanghai"))
stamp, date = now.strftime("%Y%m%d-%H%M%S"), now.strftime("%Y%m%d")
short = os.environ["SOURCE_SHA"][:7]
branch = ref.replace("/", "-")
data = {name: os.environ[name] for name in ("ARCH_SLUG", "CHANNEL", "MAKE_PACKAGE", "TARGET_REF", "SOURCE_SHA", "DRIVER_SHA", "PACKAGE_ARCH")}
data.update(SOURCE_REF=ref, PLATFORM=platform, DOCKER_PLATFORM="linux/arm64" if arch == "arm64" else "linux/amd64",
            R2_PREFIX=("mskdsp-lower" if arch == "arm64" else "mskdsp-lower-x64") + "/" + channel,
            RELEASE_TAG="", BETA_LINE="", BETA_TAG="", BETA_TAG_PREFIX="", NOTES_START_TAG="")
if channel == "stable":
    image = f"{ref}-{platform}"
    debug = f"mskdsp-{ref}-debugsymbols-{platform}"
    data["RELEASE_TAG"] = ref
elif channel == "beta":
    image = f"{version}-{branch}-{stamp}-{short}-{platform}"
    debug = f"mskdsp-{version}-{branch}-{stamp}-{short}-debugsymbols-{platform}"
    line = ref.removeprefix("beta/")
    data.update(BETA_LINE=line, BETA_TAG_PREFIX=f"beta-{line}-", BETA_TAG=f"beta-{line}-{stamp}-{short}")
    result = subprocess.run(["git", "describe", "--tags", "--match", "v*", "--abbrev=0"], text=True, capture_output=True)
    if result.returncode == 0:
        data["NOTES_START_TAG"] = result.stdout.strip()
else:
    image = f"{version}-{branch}-{channel}-{date}-{short}-{platform}"
    debug = f"mskdsp-{version}-{branch}-{channel}-{date}-{short}-debugsymbols-{platform}"
if not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}", image):
    raise SystemExit("生成的 Docker 版本标签包含不支持的字符或超过 128 字符")
data.update(IMAGE_VERSION=image, ARCHIVE_NAME="mskdsp-" + image, DEBUG_ARCHIVE_NAME=debug)
# 只输出可同时用于 dotenv 与 shell 的简单值，禁止转义、命令替换、空格和凭据。
for key, value in data.items():
    if not re.fullmatch(r"[A-Za-z0-9_./-]*", value):
        raise SystemExit(f"构建元数据包含不安全字符: {key}")
Path("build.env").write_text("".join(f"{key}={value}\n" for key, value in data.items()), encoding="utf-8")
print(f"已生成交付版本: {image}")
PY

  echo "配置 ${ARCH_SLUG} RelWithDebInfo，测试开启，剥离符号=${strip_debug}"
  cmake -S . -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14 \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
    -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
    -DMSKDSP_BUILD_TESTS=ON -DMSKDSP_STRIP_DEBUG="${strip_debug}" \
    -DVCPKG_HOST_TRIPLET="${triplet}" -DVCPKG_TARGET_TRIPLET="${triplet}" \
    -DProtobuf_PROTOC_EXECUTABLE="${project_root}/${BUILD_DIR}/vcpkg_installed/${triplet}/tools/protobuf/protoc" \
    -DgRPC_CPP_PLUGIN_EXECUTABLE="${project_root}/${BUILD_DIR}/vcpkg_installed/${triplet}/tools/grpc/grpc_cpp_plugin"
  ccache --zero-stats
  echo "编译 ${ARCH_SLUG}，并行度=${BUILD_PARALLEL}"
  cmake --build "${BUILD_DIR}" --parallel "${BUILD_PARALLEL}"
  echo "执行单元测试，固定并行度 2，生成 GitLab JUnit 报告"
  ctest --test-dir "${BUILD_DIR}" --output-on-failure --parallel 2 \
    --no-tests=error --output-junit ctest-junit.xml
  if [[ "${MAKE_PACKAGE}" == true ]]; then
    echo "安装运行产物并检查 IEC61850 交付包"
    cmake --install "${BUILD_DIR}"
    find package -maxdepth 1 -type f -name '*_test' -delete
    bash script/check_iec61850_package.sh package "${package_arch}" false true
  else
    echo "当前任务仅编译与测试，不生成安装包"
  fi
}

main "$@"
