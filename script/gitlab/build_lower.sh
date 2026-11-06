#!/usr/bin/env bash
# 两种原生架构完整测试，保留 ARM64 交叉编译入口；发布只消费冻结产物。
set -euo pipefail

log() { printf '[lower-ci] %s\n' "$*"; }
fail() { log "错误：$*" >&2; exit 1; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"
MODE="${1:-}"
ARCH="${2:-}"
case "${ARCH}" in
  arm64) TRIPLET=arm64-linux-dynamic; PLATFORM=linux/arm64; BASE_IMAGE=arm64v8/ubuntu:noble; STRIP_DEBUG=ON ;;
  x64) TRIPLET=x64-linux-dynamic; PLATFORM=linux/amd64; BASE_IMAGE=amd64/ubuntu:noble; STRIP_DEBUG=ON ;;
  *) fail '用法：build_lower.sh build arm64|x64 | test arm64|x64 | cross-build arm64 | package arm64|x64' ;;
esac
case "${MODE}:${ARCH}" in
  build:x64|build:arm64|test:x64|test:arm64|cross-build:arm64|package:arm64|package:x64) ;;
  *) fail '用法：build_lower.sh build arm64|x64 | test arm64|x64 | cross-build arm64 | package arm64|x64' ;;
esac
HOST_TRIPLET="${TRIPLET}"
CACHE_ARCH="${ARCH}"
TARGET_CC=gcc-14
TARGET_CXX=g++-14
BUILD_TESTS=ON
if [[ "${MODE}" == cross-build ]]; then
  [[ "${ARCH}" == arm64 ]] || fail '交叉编译模式仅支持 ARM64'
  TRIPLET=arm64-linux-dynamic-cross
  HOST_TRIPLET=x64-linux-dynamic
  CACHE_ARCH=arm64-cross
  TARGET_CC=aarch64-linux-gnu-gcc-14
  TARGET_CXX=aarch64-linux-gnu-g++-14
  BUILD_TESTS=OFF
fi
BUILD_DIR="build-${ARCH}"
RELEASE_DIR="release/${ARCH}"
LOG_DIR="logs/${ARCH}"
mkdir -p "${RELEASE_DIR}" "${LOG_DIR}"
# 保留失败阶段输出；GitLab artifacts.when=always 收集日志和 CTest 报告。
exec > >(tee -a "${LOG_DIR}/${MODE}.log") 2>&1

fetch_protobuf() {
  # 协议为公开 GitHub 固定子模块，隔离旧作业认证和进程级地址重定向。
  python3 - <<'PY'
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path("script/gitlab").resolve()))
from init_platform_proto import public_protocol_environment

environment = public_protocol_environment(os.environ)
# Runner可能清除父仓库注册信息但保留子仓库；先注册才能同步已有origin。
for stage, command in (("初始化", ["git", "submodule", "init", "--", "protobuf"]),
                       ("同步地址", ["git", "submodule", "sync", "--recursive", "--", "protobuf"]),
                       ("更新提交", ["git", "submodule", "update", "--init", "--recursive", "--", "protobuf"])):
    result = subprocess.run(command, env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(f"protobuf 子模块{stage}失败，请检查 GitHub 网络与固定提交；底层输出已隐藏")
print("已从公开 GitHub 获取固定 protobuf 子模块，未携带作业令牌")
PY
}

finish_package() {
  find package -maxdepth 1 -type f -name '*_test' -delete
}

archive_debug_symbols() {
  [[ -d package/debug && ! -L package/debug ]] || fail "${ARCH} 安装结果缺少独立调试符号目录"
  [[ -n "$(find package/debug -type f -name '*.debug' -size +0c -print -quit)" ]] \
    || fail "${ARCH} 独立调试符号目录为空或缺少非空符号文件"
  local symbol_name symbol_file symbol_dir="symbols/${ARCH}"
  symbol_name="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "${RELEASE_DIR}/build_identity.json")"
  symbol_file="mskdsp-${symbol_name}-debugsymbols-linux-${ARCH}.tar.gz"
  mkdir -p "${symbol_dir}"
  log "归档 ${ARCH} 独立调试符号，保留原始构建身份"
  tar -C package -czf "${symbol_dir}/${symbol_file}" debug
  tar -tzf "${symbol_dir}/${symbol_file}" >/dev/null
  cp -- "${RELEASE_DIR}/build_identity.json" "${symbol_dir}/build_identity.json"
  (cd "${symbol_dir}"; sha256sum "${symbol_file}" build_identity.json > SHA256SUMS
    sha256sum --check --strict SHA256SUMS)
  log "${ARCH} 独立调试符号及原始构建身份校验完成：${symbol_dir}"
}

run_complete_ctest() {
  (
    local test_env="${ROOT}/${BUILD_DIR}/test_env"
    local disk_env="${test_env}.disk"
    local testing_env="${ROOT}/${BUILD_DIR}/Testing"
    local disk_testing="${testing_env}.disk"
    local ram_env="" result=0
    [[ ! -e "${disk_env}" && ! -L "${disk_env}" ]] || { log '错误：测试磁盘备份已存在，拒绝覆盖'; exit 1; }
    [[ ! -e "${disk_testing}" && ! -L "${disk_testing}" ]] || { log '错误：CTest 日志备份已存在，拒绝覆盖'; exit 1; }
    [[ ! -L "${test_env}" ]] || { log '错误：原测试工作区不是独立目录'; exit 1; }
    [[ ! -L "${testing_env}" ]] || { log '错误：原 CTest 日志目录不是独立目录'; exit 1; }
    restore_ctest_workspace() {
      local original_status="$1" cleanup_status=0
      trap - EXIT
      # 先保存成功及失败的 CTest 日志，再恢复磁盘目录；不丢弃本次诊断。
      if [[ -L "${testing_env}" && "$(readlink -- "${testing_env}")" == "${ram_env}/.Testing" ]]; then
        cp -a "${ram_env}/.Testing/." "${disk_testing}/" || cleanup_status=1
        rm -- "${testing_env}" || cleanup_status=1
      fi
      if [[ -d "${disk_testing}" ]]; then
        if [[ ! -e "${testing_env}" && ! -L "${testing_env}" ]]; then
          mv -- "${disk_testing}" "${testing_env}" || cleanup_status=1
        else
          log '错误：无法安全恢复 CTest 日志目录'
          cleanup_status=1
        fi
      fi
      if [[ -L "${test_env}" && "$(readlink -- "${test_env}")" == "${ram_env}" ]]; then
        rm -- "${test_env}" || cleanup_status=1
      fi
      if [[ -d "${disk_env}" ]]; then
        if [[ ! -e "${test_env}" && ! -L "${test_env}" ]]; then
          mv -- "${disk_env}" "${test_env}" || cleanup_status=1
        else
          log '错误：无法安全恢复测试磁盘工作区'
          cleanup_status=1
        fi
      fi
      # 仅删除本次 mktemp 创建的 RAM 目录；不跟随符号链接或处理其他路径。
      if [[ -n "${ram_env}" ]]; then
        if [[ "${ram_env}" == /dev/shm/msk.?????? \
              && "$(dirname -- "${ram_env}")" == /dev/shm && ! -L "${ram_env}" ]]; then
          rm -rf -- "${ram_env}" || cleanup_status=1
        else
          log '错误：测试 RAM 目录不在本次允许的清理范围'
          cleanup_status=1
        fi
      fi
      if [[ "${original_status}" == 0 && "${cleanup_status}" != 0 ]]; then
        original_status="${cleanup_status}"
      fi
      exit "${original_status}"
    }
    trap 'restore_ctest_workspace "$?"' EXIT
    ram_env="$(mktemp -d /dev/shm/msk.XXXXXX)"
    mkdir -p "${test_env}"
    mv -- "${test_env}" "${disk_env}"
    # 目录位于 tmpfs；ELF 种子通过绝对链接保留在磁盘，兼容 /dev/shm 的 noexec 挂载。
    cp -as "${disk_env}/." "${ram_env}/"
    # 模块扫描会跳过文件符号链接，因此整目录映射磁盘种子，保留 Dummy .so 的普通文件身份。
    local seed_module ram_module
    while IFS= read -r -d '' seed_module; do
      ram_module="${ram_env}/${seed_module#"${disk_env}/"}"
      [[ "${ram_module}" == "${ram_env}/"* && "$(realpath -- "${ram_module}")" == "${ram_module}" ]] \
        || { log '错误：模块种子目录不在本次 RAM 工作区'; exit 1; }
      rm -rf -- "${ram_module}"
      ln -s -- "${seed_module}" "${ram_module}"
    done < <(find "${disk_env}" -type d -name module -prune -print0)
    ln -s "${ram_env}" "${test_env}"
    mkdir "${ram_env}/.tmp"
    export TMPDIR="${ram_env}/.tmp"
    mkdir -p "${testing_env}"
    mv -- "${testing_env}" "${disk_testing}"
    mkdir "${ram_env}/.Testing"
    cp -a "${disk_testing}/." "${ram_env}/.Testing/"
    ln -s "${ram_env}/.Testing" "${testing_env}"
    log "执行 ${ARCH} 完整 CTest，并行度 2，运行工作区位于 RAM"
    ctest --test-dir "${BUILD_DIR}" --output-on-failure --parallel 2 --no-tests=error \
      --output-junit "${ROOT}/${LOG_DIR}/ctest.xml" || result="$?"
    exit "${result}"
  )
}

configure_local_registry() {
  local baseline="$1"
  # 核对浅克隆中的基线目录；实际所选版本的树仍由 vcpkg 严格解析，不能因无关旧端口缺树拒绝。
  python3 - "${VCPKG_ROOT}" "${baseline}" <<'PY'
from pathlib import Path
import subprocess
import sys

root, baseline = Path(sys.argv[1]), sys.argv[2]
required = [baseline + ":ports", baseline + ":versions"]
result = subprocess.run(["git", "-C", str(root), "cat-file", "--batch-check=%(objectname) %(objecttype)"],
                        input="\n".join(sorted(required)) + "\n", text=True, capture_output=True)
rows = result.stdout.splitlines()
if result.returncode or len(rows) != len(required) or any(row.split()[-1:] != ["tree"] for row in rows):
    raise SystemExit("本地 registry 缺少基线引用的树对象，请准备完整基线缓存后重试")
print("已核对本地 registry 固定基线的 ports 与 versions 树对象")
PY
  # 仅当前 CI 进程继承；不修改全局或仓库配置，protobuf 的临时认证仍独立配置。
  unset GIT_CONFIG_PARAMETERS
  export GIT_CONFIG_COUNT=1
  export GIT_CONFIG_KEY_0="url.file://${VCPKG_ROOT}/.insteadOf"
  export GIT_CONFIG_VALUE_0=https://github.com/microsoft/vcpkg
  export GIT_TERMINAL_PROMPT=0
  log 'vcpkg registry 使用已验证的本地固定基线，避免联网读取 GitHub HEAD'
}

prepare_build_cache() {
  local cache_root="${LOWER_CACHE_ROOT:-${ROOT}/.cache}"
  local cache_mount="${LOWER_CACHE_MOUNT:-}"
  [[ "${cache_root}" == /* ]] || fail '构建缓存根目录必须为绝对路径'
  cache_root="$(realpath -m -- "${cache_root}")"
  if [[ -n "${cache_mount}" ]]; then
    [[ "${cache_mount}" == /* ]] || fail '构建缓存挂载目录必须为绝对路径'
    cache_mount="$(realpath -m -- "${cache_mount}")"
    mountpoint -q -- "${cache_mount}" || fail "持久缓存挂载不存在：${cache_mount}"
    case "${cache_root}/" in
      "${ROOT}/"*) fail '持久缓存目录不能位于项目工作区内' ;;
    esac
    case "${cache_root}/" in
      "${cache_mount%/}/"*) ;;
      *) fail '缓存根目录解析后不在指定挂载内' ;;
    esac
  fi
  export VCPKG_ROOT="${cache_root}/vcpkg/${CACHE_ARCH}/source"
  export VCPKG_DOWNLOADS="${cache_root}/vcpkg/${CACHE_ARCH}/downloads"
  export VCPKG_DEFAULT_BINARY_CACHE="${cache_root}/vcpkg/${CACHE_ARCH}/archives"
  export CCACHE_DIR="${cache_root}/ccache/${CACHE_ARCH}"
  mkdir -p "${VCPKG_DOWNLOADS}" "${VCPKG_DEFAULT_BINARY_CACHE}" "${CCACHE_DIR}"
  log "构建缓存根目录：${cache_root}，架构：${CACHE_ARCH}"
  if [[ -d "${VCPKG_ROOT}/.git" ]]; then
    log '复用已有 vcpkg 源码与持久编译缓存'
  else
    log '首次准备 vcpkg 源码，已有二进制及编译缓存继续复用'
  fi
}

build_package() {
  export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
  export VCPKG_MAX_CONCURRENCY="${VCPKG_MAX_CONCURRENCY:-4}"
  [[ "${CMAKE_BUILD_PARALLEL_LEVEL}" =~ ^[1-9][0-9]*$ ]] || fail 'CMAKE_BUILD_PARALLEL_LEVEL 必须为正整数'
  [[ "${VCPKG_MAX_CONCURRENCY}" =~ ^[1-9][0-9]*$ ]] || fail 'VCPKG_MAX_CONCURRENCY 必须为正整数'
  log "构建资源：主工程并行度 ${CMAKE_BUILD_PARALLEL_LEVEL}，vcpkg 并行度 ${VCPKG_MAX_CONCURRENCY}"
  if [[ "${MODE}" == cross-build ]]; then
    [[ "$(uname -m)" == x86_64 ]] || fail 'ARM64 交叉编译必须在 x64 工具镜像执行'
    [[ "$("${TARGET_CC}" -dumpmachine)" == aarch64-linux-gnu ]] || fail '交叉编译器目标架构不一致'
    [[ "$("${TARGET_CXX}" -dumpfullversion)" == 14.* ]] || fail '交叉编译器必须为 GCC14'
    [[ "$(gcc-14 -dumpfullversion)" == 14.* ]] || fail '主机协议工具编译器必须为 GCC14'
    [[ "$(cc -dumpmachine)" == x86_64-linux-gnu && "$(c++ -dumpmachine)" == x86_64-linux-gnu \
       && "$(cc -dumpfullversion)" == 14.* && "$(c++ -dumpfullversion)" == 14.* ]] \
      || fail '交叉镜像默认主机 cc/c++ 必须为原生 x64 GCC14'
    # make 端口仅在 CC/CXX 未定义时使用检测到的目标工具链，不能全局导出主机编译器。
    unset CC CXX
    log "主机 GCC $(gcc-14 -dumpfullversion)，ARM64 交叉 GCC $("${TARGET_CXX}" -dumpfullversion)"
  else
    case "$(uname -m):${ARCH}" in
      aarch64:arm64|arm64:arm64|x86_64:x64) ;;
      *) fail '工具镜像架构与构建目标不一致' ;;
    esac
    local compiler_target=x86_64-linux-gnu
    [[ "${ARCH}" != arm64 ]] || compiler_target=aarch64-linux-gnu
    [[ "$("${TARGET_CC}" -dumpmachine)" == "${compiler_target}" \
       && "$("${TARGET_CXX}" -dumpmachine)" == "${compiler_target}" \
       && "$("${TARGET_CC}" -dumpfullversion)" == 14.* \
       && "$("${TARGET_CXX}" -dumpfullversion)" == 14.* ]] || fail '原生编译器架构或 GCC14 版本不一致'
    # 同时约束 vcpkg 的非 CMake 端口，避免依赖误用 Runner 的默认编译器。
    export CC="${TARGET_CC}" CXX="${TARGET_CXX}"
  fi
  prepare_build_cache
  log "准备 ${ARCH} 主分支构建与 protobuf 子模块"
  python3 script/gitlab/lower_metadata.py build --arch "${ARCH}" --output "${RELEASE_DIR}/build_identity.json"
  fetch_protobuf
  export VCPKG_BINARY_SOURCES="clear;files,${VCPKG_DEFAULT_BINARY_CACHE},readwrite"
  export VCPKG_OVERLAY_TRIPLETS="${ROOT}/cmake/vcpkg-triplets"
  export VCPKG_DISABLE_METRICS=1
  # 仅重定向官方 GitHub archive，保留 REF 和 SHA512；失败不退回已知不通的首跳。
  # 占位参数由 vcpkg 自行做 shell 转义，外层再次加引号会破坏含空格的目标路径。
  export X_VCPKG_ASSET_SOURCES="clear;x-script,python3 \"${ROOT}/script/gitlab/download_asset.py\" {url} {sha512} {dst};x-block-origin"
  export CCACHE_BASEDIR="${ROOT}"
  export CCACHE_COMPILERCHECK=content CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-8G}"
  log "编译缓存：${CCACHE_DIR}，每架构上限 ${CCACHE_MAXSIZE}，调试与文件路径统一到 /usr/src/mskdsp"
  local baseline
  baseline="$(python3 -c 'import json; print(json.load(open("vcpkg-configuration.json"))["default-registry"]["baseline"])')"
  if [[ ! -d "${VCPKG_ROOT}/.git" ]]; then
    mkdir -p "${VCPKG_ROOT}"
    git -C "${VCPKG_ROOT}" init -q
    git -C "${VCPKG_ROOT}" remote add origin https://github.com/microsoft/vcpkg.git
  fi
  # 版本求解会读取同版本的旧 port revision，必须保留固定基线的完整可达历史。
  if [[ "$(git -C "${VCPKG_ROOT}" rev-parse --is-shallow-repository)" == true ]]; then
    log '补全 vcpkg 固定基线历史，供本地 registry 解析历史端口树'
    git -C "${VCPKG_ROOT}" fetch --unshallow origin "${baseline}"
  elif ! git -C "${VCPKG_ROOT}" cat-file -e "${baseline}^{commit}" 2>/dev/null; then
    git -C "${VCPKG_ROOT}" fetch origin "${baseline}"
  fi
  git -C "${VCPKG_ROOT}" checkout --detach "${baseline}"
  [[ "$(git -C "${VCPKG_ROOT}" rev-parse HEAD)" == "${baseline}" ]] || fail 'vcpkg baseline 不一致'
  if [[ ! -x "${VCPKG_ROOT}/vcpkg" ]]; then
    "${VCPKG_ROOT}/bootstrap-vcpkg.sh" -disableMetrics
  fi
  configure_local_registry "${baseline}"
  log "配置 ${ARCH} RelWithDebInfo，测试程序编译=${BUILD_TESTS}，strip=${STRIP_DEBUG}"
  log '启用官方 codeload 归档下载与严格 SHA512 校验'
  local extra_arguments=()
  if [[ "${GITHUB_ACTIONS:-}" == true && "${RUNNER_ENVIRONMENT:-}" == github-hosted ]]; then
    # 托管主机空间有限，逐端口清理中间构建树；保留下载、二进制缓存和已安装测试依赖。
    extra_arguments+=("-DVCPKG_INSTALL_OPTIONS=--clean-buildtrees-after-build;--clean-packages-after-build")
  fi
  if [[ "${MODE}" == cross-build ]]; then
    extra_arguments+=("-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=${ROOT}/cmake/toolchains/aarch64-linux-gcc14.cmake")
  fi
  # 流水线和架构使用独立工作区；统一调试及 __FILE__ 路径，安全复用跨目录编译缓存。
  local prefix_flags="-fdebug-prefix-map=${ROOT}=/usr/src/mskdsp -ffile-prefix-map=${ROOT}=/usr/src/mskdsp"
  cmake -S . -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER="${TARGET_CC}" -DCMAKE_CXX_COMPILER="${TARGET_CXX}" \
    "-DCMAKE_C_FLAGS=${CFLAGS:+${CFLAGS} }${prefix_flags}" \
    "-DCMAKE_CXX_FLAGS=${CXXFLAGS:+${CXXFLAGS} }${prefix_flags}" \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
    -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
    -DMSKDSP_BUILD_TESTS="${BUILD_TESTS}" -DMSKDSP_STRIP_DEBUG="${STRIP_DEBUG}" \
    -DVCPKG_HOST_TRIPLET="${HOST_TRIPLET}" -DVCPKG_TARGET_TRIPLET="${TRIPLET}" \
    -DProtobuf_PROTOC_EXECUTABLE="${ROOT}/${BUILD_DIR}/vcpkg_installed/${HOST_TRIPLET}/tools/protobuf/protoc" \
    -DgRPC_CPP_PLUGIN_EXECUTABLE="${ROOT}/${BUILD_DIR}/vcpkg_installed/${HOST_TRIPLET}/tools/grpc/grpc_cpp_plugin" \
    "${extra_arguments[@]}"
  ccache --zero-stats
  log "编译 ${ARCH}，并行度 ${CMAKE_BUILD_PARALLEL_LEVEL}"
  cmake --build "${BUILD_DIR}" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL}"
  cmake --install "${BUILD_DIR}"
  archive_debug_symbols
  if [[ "${MODE}" == cross-build ]]; then
    finish_package
    log 'ARM64 交叉编译、安装及独立调试符号归档完成，打包等待 x64 完整测试通过'
  else
    python3 - "${ROOT}" "${RELEASE_DIR}/build_context.json" "${TRIPLET}" "${HOST_TRIPLET}" <<'PY'
import json
from pathlib import Path
import sys
Path(sys.argv[2]).write_text(json.dumps({"project_dir": sys.argv[1], "target_triplet": sys.argv[3],
                                      "host_triplet": sys.argv[4]}) + "\n", encoding="utf-8")
PY
    log "${ARCH} 原生编译、安装及独立符号归档完成，保留依赖等待完整 CTest"
  fi
}

test_native() {
  case "$(uname -m):${ARCH}" in
    x86_64:x64|aarch64:arm64|arm64:arm64) ;;
    *) fail "${ARCH} 测试必须在对应原生主机执行" ;;
  esac
  [[ -f "${BUILD_DIR}/CTestTestfile.cmake" && -f package/MskDSP && -f "${RELEASE_DIR}/build_context.json" ]] \
    || fail '缺少完整原生构建目录或测试产物'
  python3 - "${ROOT}" "${RELEASE_DIR}" "${ARCH}" "${TRIPLET}" "${HOST_TRIPLET}" <<'PY'
import json
import os
from pathlib import Path
import sys
sys.path.insert(0, str(Path("script/gitlab").resolve()))
from lower_metadata import package_identity
release = Path(sys.argv[2])
identity = json.loads((release / "build_identity.json").read_text(encoding="utf-8"))
# 仅验证原构建来源，不使用测试作业身份覆盖 build_job_id。
package_identity(identity, os.environ)
if identity.get("platform") != "linux-" + sys.argv[3]:
    raise SystemExit("原生测试构建产物架构不一致")
context = json.loads((release / "build_context.json").read_text(encoding="utf-8"))
if context != {"project_dir": sys.argv[1], "target_triplet": sys.argv[4], "host_triplet": sys.argv[5]}:
    raise SystemExit("原生测试路径或构建来源不一致，拒绝执行其他构建的测试")
print("已核对原生构建身份与测试绝对路径：" + sys.argv[3])
PY
  fetch_protobuf
  run_complete_ctest
  finish_package
  log "${ARCH} 完整 CTest 成功，保留原构建身份并完成交付目录清理"
}

package_release() {
  local backend="${LOWER_PACKAGE_BACKEND:-remote-buildkit}"
  case "${backend}" in
    remote-buildkit)
      : "${BUILDKIT_HOST:?缺少 BuildKit 服务地址}"
      : "${BUILDKIT_CA_B64:?缺少 BuildKit CA}"
      : "${BUILDKIT_CERT_B64:?缺少 BuildKit 客户端证书}"
      : "${BUILDKIT_KEY_B64:?缺少 BuildKit 客户端私钥}"
      [[ "${BUILDKIT_HOST}" == tcp://* ]] || fail 'BuildKit 必须通过 mTLS TCP 地址访问'
      ;;
    local-docker)
      [[ "${GITHUB_ACTIONS:-}" == true && "${RUNNER_ENVIRONMENT:-}" == github-hosted ]] \
        || fail '本机 Docker 打包仅用于 GitHub 托管任务'
      case "$(uname -m):${ARCH}" in
        x86_64:x64|aarch64:arm64|arm64:arm64) ;;
        *) fail '本机 Docker 打包必须在对应原生主机执行' ;;
      esac
      ;;
    *) fail '未知安装包构建后端' ;;
  esac
  [[ -f package/MskDSP && -f "${RELEASE_DIR}/build_identity.json" ]] || fail '缺少编译产物或原始构建身份'
  local temporary version image_tar
  temporary="$(mktemp -d)"
  trap 'rm -rf "${temporary:-}"' EXIT
  chmod 700 "${temporary}"
  if [[ "${backend}" == remote-buildkit ]]; then
    umask 077
    printf '%s' "${BUILDKIT_CA_B64}" | base64 -d > "${temporary}/ca.pem"
    printf '%s' "${BUILDKIT_CERT_B64}" | base64 -d > "${temporary}/cert.pem"
    printf '%s' "${BUILDKIT_KEY_B64}" | base64 -d > "${temporary}/key.pem"
    umask 022
  fi
  python3 script/gitlab/lower_metadata.py package --identity "${RELEASE_DIR}/build_identity.json" \
    --output "${RELEASE_DIR}/package_identity.json"
  version="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["package_version"])' "${RELEASE_DIR}/package_identity.json")"
  mkdir -p images
  image_tar="images/mskdsp-${version}.tar"
  sed -e "s#--platform=linux/arm64#--platform=${PLATFORM}#" \
      -e "s#localhost/arm64v8/ubuntu:noble#${BASE_IMAGE}#" Dockerfile > "${temporary}/Dockerfile"
  if [[ "${backend}" == local-docker ]]; then
    log "通过本机原生 Docker 生成 ${ARCH} 安装镜像，不使用内网 BuildKit"
    docker build --platform "${PLATFORM}" --file "${temporary}/Dockerfile" --tag "mskdsp:${version}" "${ROOT}"
    docker save --output "${image_tar}" "mskdsp:${version}"
  else
    log "通过独立 BuildKit 服务生成 ${ARCH} Docker 归档"
    buildctl --addr "${BUILDKIT_HOST}" \
      --tlscacert "${temporary}/ca.pem" --tlscert "${temporary}/cert.pem" --tlskey "${temporary}/key.pem" \
      build --frontend dockerfile.v0 --local context="${ROOT}" --local dockerfile="${temporary}" \
      --opt "platform=${PLATFORM}" --output "type=docker,name=mskdsp:${version},dest=${image_tar}"
  fi
  log '封装既有 gzip 自解压安装包'
  bash script/make_exe.sh "${image_tar}"
  mv "images/mskdsp-${version}" "${RELEASE_DIR}/"
  python3 script/gitlab/lower_metadata.py freeze --identity "${RELEASE_DIR}/package_identity.json" \
    --artifact "${RELEASE_DIR}/mskdsp-${version}" --image-tar "${image_tar}" \
    --output "${RELEASE_DIR}/metadata.json"
  (cd "${RELEASE_DIR}"; sha256sum "mskdsp-${version}" > SHA256SUMS)
  rm -f "${image_tar}"
  rm -rf "${temporary}"
  trap - EXIT
  log "${ARCH} 安装包、镜像身份及 SHA256 已冻结，可由发布作业直接上传"
}

case "${MODE}" in
  build) build_package ;;
  cross-build) build_package ;;
  test) test_native ;;
  package) package_release ;;
  *) fail '用法：build_lower.sh build arm64|x64 | test arm64|x64 | cross-build arm64 | package arm64|x64' ;;
esac
