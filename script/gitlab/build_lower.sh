#!/usr/bin/env bash
# 两种架构共用编译测试与打包逻辑，发布作业仅消费冻结产物。
set -euo pipefail

log() { printf '[lower-ci] %s\n' "$*"; }
fail() { log "错误：$*" >&2; exit 1; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"
MODE="${1:-}"
ARCH="${2:-}"
case "${ARCH}" in
  arm64) TRIPLET=arm64-linux-dynamic; PLATFORM=linux/arm64; BASE_IMAGE=arm64v8/ubuntu:noble; STRIP_DEBUG=ON ;;
  x64) TRIPLET=x64-linux-dynamic; PLATFORM=linux/amd64; BASE_IMAGE=amd64/ubuntu:noble; STRIP_DEBUG=OFF ;;
  *) fail '用法：build_lower.sh build|package arm64|x64' ;;
esac
HOST_TRIPLET="${TRIPLET}"
CACHE_ARCH="${ARCH}"
TARGET_CC=gcc-14
TARGET_CXX=g++-14
if [[ "${MODE}" == cross-build ]]; then
  [[ "${ARCH}" == arm64 ]] || fail '交叉编译模式仅支持 ARM64'
  TRIPLET=arm64-linux-dynamic-cross
  HOST_TRIPLET=x64-linux-dynamic
  CACHE_ARCH=arm64-cross
  TARGET_CC=aarch64-linux-gnu-gcc-14
  TARGET_CXX=aarch64-linux-gnu-g++-14
fi
BUILD_DIR="build-${ARCH}"
RELEASE_DIR="release/${ARCH}"
LOG_DIR="logs/${ARCH}"
mkdir -p "${RELEASE_DIR}" "${LOG_DIR}"
# 保留失败阶段输出；GitLab artifacts.when=always 收集日志和 CTest 报告。
exec > >(tee -a "${LOG_DIR}/${MODE}.log") 2>&1

fetch_protobuf() {
  # Git 配置仅存在于子进程环境；不修改 .gitmodules，不将凭据写入磁盘。
  python3 - <<'PY'
import os
import subprocess
from urllib.parse import urlsplit, urlunsplit

server = urlsplit(os.environ["CI_SERVER_URL"])
server_auth = urlunsplit((server.scheme, "gitlab-ci-token:" + os.environ["CI_JOB_TOKEN"] + "@" + server.netloc,
                         server.path.rstrip("/") + "/mskdsp/mskdspproto.git", "", ""))
environment = dict(os.environ, GIT_TERMINAL_PROMPT="0", GIT_CONFIG_COUNT="2",
                   GIT_CONFIG_KEY_0="url." + server_auth + ".insteadOf",
                   GIT_CONFIG_VALUE_0="git@github.com:CcooLcyy/MskDSPProto.git",
                   GIT_CONFIG_KEY_1="url." + server_auth + ".insteadOf",
                   GIT_CONFIG_VALUE_1="https://github.com/CcooLcyy/MskDSPProto.git")
for command in (["git", "submodule", "sync", "--recursive"],
                ["git", "submodule", "update", "--init", "--recursive"]):
    result = subprocess.run(command, env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit("protobuf 子模块获取失败，请检查提交同步与 CI_JOB_TOKEN allowlist；底层输出已隐藏")
print("已通过临时 CI_JOB_TOKEN 配置获取 protobuf 子模块")
PY
}

finish_tested_package() {
  find package -maxdepth 1 -type f -name '*_test' -delete
  if [[ "${ARCH}" == arm64 ]]; then
    [[ -d package/debug ]] || fail 'ARM64 安装结果缺少独立调试符号目录'
    local symbol_name
    symbol_name="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "${RELEASE_DIR}/build_identity.json")"
    tar -C package -czf "${RELEASE_DIR}/mskdsp-${symbol_name}-debugsymbols-linux-${ARCH}.tar.gz" debug
  fi
}

run_complete_ctest() {
  (
    local test_env="${ROOT}/${BUILD_DIR}/test_env"
    local disk_env="${test_env}.disk"
    local ram_env="" result=0
    [[ ! -e "${disk_env}" && ! -L "${disk_env}" ]] || { log '错误：测试磁盘备份已存在，拒绝覆盖'; exit 1; }
    [[ ! -L "${test_env}" ]] || { log '错误：原测试工作区不是独立目录'; exit 1; }
    restore_ctest_workspace() {
      local original_status="$1" cleanup_status=0
      trap - EXIT
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
        if [[ "${ram_env}" == /dev/shm/mskdsp-ctest-"${ARCH}".* \
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
    ram_env="$(mktemp -d "/dev/shm/mskdsp-ctest-${ARCH}.XXXXXX")"
    mkdir -p "${test_env}"
    mv -- "${test_env}" "${disk_env}"
    # 目录位于 tmpfs；ELF 种子通过绝对链接保留在磁盘，兼容 /dev/shm 的 noexec 挂载。
    cp -as "${disk_env}/." "${ram_env}/"
    ln -s "${ram_env}" "${test_env}"
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

build_and_test() {
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
  fi
  log "准备 ${ARCH} 主分支构建与 protobuf 子模块"
  python3 script/gitlab/lower_metadata.py build --arch "${ARCH}" --output "${RELEASE_DIR}/build_identity.json"
  fetch_protobuf
  export VCPKG_ROOT="${ROOT}/.cache/vcpkg/${CACHE_ARCH}/source"
  export VCPKG_DOWNLOADS="${ROOT}/.cache/vcpkg/${CACHE_ARCH}/downloads"
  export VCPKG_DEFAULT_BINARY_CACHE="${ROOT}/.cache/vcpkg/${CACHE_ARCH}/archives"
  export VCPKG_BINARY_SOURCES="clear;files,${VCPKG_DEFAULT_BINARY_CACHE},readwrite"
  export VCPKG_OVERLAY_TRIPLETS="${ROOT}/cmake/vcpkg-triplets"
  export VCPKG_DISABLE_METRICS=1 VCPKG_MAX_CONCURRENCY=8
  # 仅重定向官方 GitHub archive，保留 REF 和 SHA512；失败不退回已知不通的首跳。
  # 占位参数由 vcpkg 自行做 shell 转义，外层再次加引号会破坏含空格的目标路径。
  export X_VCPKG_ASSET_SOURCES="clear;x-script,python3 \"${ROOT}/script/gitlab/download_asset.py\" {url} {sha512} {dst};x-block-origin"
  export CCACHE_DIR="${ROOT}/.cache/ccache/${CACHE_ARCH}" CCACHE_BASEDIR="${ROOT}"
  export CCACHE_COMPILERCHECK=content CCACHE_MAXSIZE=1G
  mkdir -p "${VCPKG_DOWNLOADS}" "${VCPKG_DEFAULT_BINARY_CACHE}" "${CCACHE_DIR}"
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
  log "配置 ${ARCH} RelWithDebInfo，完整测试开启，strip=${STRIP_DEBUG}"
  log '启用官方 codeload 归档下载与严格 SHA512 校验'
  local extra_arguments=()
  if [[ "${MODE}" == cross-build ]]; then
    extra_arguments+=("-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=${ROOT}/cmake/toolchains/aarch64-linux-gcc14.cmake")
  fi
  cmake -S . -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER="${TARGET_CC}" -DCMAKE_CXX_COMPILER="${TARGET_CXX}" \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
    -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
    -DMSKDSP_BUILD_TESTS=ON -DMSKDSP_STRIP_DEBUG="${STRIP_DEBUG}" \
    -DVCPKG_HOST_TRIPLET="${HOST_TRIPLET}" -DVCPKG_TARGET_TRIPLET="${TRIPLET}" \
    -DProtobuf_PROTOC_EXECUTABLE="${ROOT}/${BUILD_DIR}/vcpkg_installed/${HOST_TRIPLET}/tools/protobuf/protoc" \
    -DgRPC_CPP_PLUGIN_EXECUTABLE="${ROOT}/${BUILD_DIR}/vcpkg_installed/${HOST_TRIPLET}/tools/grpc/grpc_cpp_plugin" \
    "${extra_arguments[@]}"
  ccache --zero-stats
  log "编译 ${ARCH}，并行度 8"
  cmake --build "${BUILD_DIR}" --parallel 8
  if [[ "${MODE}" != cross-build ]]; then
    run_complete_ctest
    log '测试成功，安装运行文件并归档独立调试符号'
  else
    log '交叉编译完成，安装运行依赖并保留全部测试程序供 ARM64 作业执行'
  fi
  cmake --install "${BUILD_DIR}"
  if [[ "${MODE}" == cross-build ]]; then
    python3 - "${ROOT}" "${RELEASE_DIR}/build_context.json" <<'PY'
import json
from pathlib import Path
import sys
Path(sys.argv[2]).write_text(json.dumps({"project_dir": sys.argv[1], "target_triplet": "arm64-linux-dynamic-cross",
                                      "host_triplet": "x64-linux-dynamic"}) + "\n", encoding="utf-8")
PY
    log 'ARM64 交叉编译与安装完成，等待独立 ARM64 完整 CTest'
  else
    finish_tested_package
    log "${ARCH} 编译、完整测试与安装完成"
  fi
}

test_arm64() {
  [[ "${ARCH}" == arm64 ]] || fail '独立测试模式仅支持 ARM64'
  case "$(uname -m)" in aarch64|arm64) ;; *) fail 'ARM64 测试必须在 ARM64 工具镜像执行' ;; esac
  [[ -f "${BUILD_DIR}/CTestTestfile.cmake" && -f package/MskDSP ]] || fail '缺少完整交叉编译构建目录或测试产物'
  python3 - "${ROOT}" "${RELEASE_DIR}" <<'PY'
import json
import os
from pathlib import Path
import sys
sys.path.insert(0, str(Path("script/gitlab").resolve()))
from lower_metadata import package_identity
release = Path(sys.argv[2])
identity = json.loads((release / "build_identity.json").read_text(encoding="utf-8"))
# 复用来源验证，但不生成测试作业身份，继续保留原编译作业的 build_job_id。
package_identity(identity, os.environ)
context = json.loads((release / "build_context.json").read_text(encoding="utf-8"))
if context != {"project_dir": sys.argv[1], "target_triplet": "arm64-linux-dynamic-cross", "host_triplet": "x64-linux-dynamic"}:
    raise SystemExit("ARM64 测试路径或交叉编译来源不一致，拒绝执行其他构建的测试")
print("已核对 ARM64 原构建身份与跨镜像绝对路径")
PY
  fetch_protobuf
  run_complete_ctest
  finish_tested_package
  log 'ARM64 完整 CTest 成功，保留原构建身份并完成独立调试符号归档'
}

package_release() {
  : "${BUILDKIT_HOST:?缺少 BuildKit 服务地址}"
  : "${BUILDKIT_CA_B64:?缺少 BuildKit CA}"
  : "${BUILDKIT_CERT_B64:?缺少 BuildKit 客户端证书}"
  : "${BUILDKIT_KEY_B64:?缺少 BuildKit 客户端私钥}"
  [[ "${BUILDKIT_HOST}" == tcp://* ]] || fail 'BuildKit 必须通过 mTLS TCP 地址访问'
  [[ -f package/MskDSP && -f "${RELEASE_DIR}/build_identity.json" ]] || fail '缺少已测试的编译产物'
  local temporary version image_tar
  temporary="$(mktemp -d)"
  trap 'rm -rf "${temporary:-}"' EXIT
  chmod 700 "${temporary}"
  umask 077
  printf '%s' "${BUILDKIT_CA_B64}" | base64 -d > "${temporary}/ca.pem"
  printf '%s' "${BUILDKIT_CERT_B64}" | base64 -d > "${temporary}/cert.pem"
  printf '%s' "${BUILDKIT_KEY_B64}" | base64 -d > "${temporary}/key.pem"
  umask 022
  python3 script/gitlab/lower_metadata.py package --identity "${RELEASE_DIR}/build_identity.json" \
    --output "${RELEASE_DIR}/package_identity.json"
  version="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["package_version"])' "${RELEASE_DIR}/package_identity.json")"
  mkdir -p images
  image_tar="images/mskdsp-${version}.tar"
  sed -e "s#--platform=linux/arm64#--platform=${PLATFORM}#" \
      -e "s#localhost/arm64v8/ubuntu:noble#${BASE_IMAGE}#" Dockerfile > "${temporary}/Dockerfile"
  log "通过独立 BuildKit 服务生成 ${ARCH} Docker 归档"
  buildctl --addr "${BUILDKIT_HOST}" \
    --tlscacert "${temporary}/ca.pem" --tlscert "${temporary}/cert.pem" --tlskey "${temporary}/key.pem" \
    build --frontend dockerfile.v0 --local context="${ROOT}" --local dockerfile="${temporary}" \
    --opt "platform=${PLATFORM}" --output "type=docker,name=mskdsp:${version},dest=${image_tar}"
  log '封装既有 gzip 自解压安装包'
  bash script/make_exe.sh "${image_tar}"
  mv "images/mskdsp-${version}" "${RELEASE_DIR}/"
  python3 script/gitlab/lower_metadata.py freeze --identity "${RELEASE_DIR}/package_identity.json" \
    --artifact "${RELEASE_DIR}/mskdsp-${version}" --image-tar "${image_tar}" \
    --output "${RELEASE_DIR}/metadata.json"
  local checksums=("mskdsp-${version}")
  if [[ "${ARCH}" == arm64 ]]; then
    local symbols=("${RELEASE_DIR}"/*debugsymbols*.tar.gz)
    [[ -f "${symbols[0]}" ]] || fail 'ARM64 构建产物缺少独立调试符号包'
    local symbol
    for symbol in "${symbols[@]}"; do checksums+=("$(basename "${symbol}")"); done
  fi
  (cd "${RELEASE_DIR}"; sha256sum "${checksums[@]}" > SHA256SUMS)
  rm -f "${image_tar}"
  rm -rf "${temporary}"
  trap - EXIT
  log "${ARCH} 安装包、镜像身份及 SHA256 已冻结，可由发布作业直接上传"
}

case "${MODE}" in
  build) build_and_test ;;
  cross-build) build_and_test ;;
  test) test_arm64 ;;
  package) package_release ;;
  *) fail '用法：build_lower.sh build|package arm64|x64' ;;
esac
