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
BUILD_DIR="build-${ARCH}"
RELEASE_DIR="release/${ARCH}"
LOG_DIR="logs/${ARCH}"
mkdir -p "${RELEASE_DIR}" "${LOG_DIR}"
# 保留失败阶段输出；GitLab artifacts.when=always 收集日志和 CTest 报告。
exec > >(tee -a "${LOG_DIR}/${MODE}.log") 2>&1

build_and_test() {
  case "$(uname -m):${ARCH}" in
    aarch64:arm64|arm64:arm64|x86_64:x64) ;;
    *) fail '工具镜像架构与构建目标不一致' ;;
  esac
  log "准备 ${ARCH} 主分支构建与 protobuf 子模块"
  python3 script/gitlab/lower_metadata.py build --arch "${ARCH}" --output "${RELEASE_DIR}/build_identity.json"
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
  export VCPKG_ROOT="${ROOT}/.cache/vcpkg/${ARCH}/source"
  export VCPKG_DOWNLOADS="${ROOT}/.cache/vcpkg/${ARCH}/downloads"
  export VCPKG_DEFAULT_BINARY_CACHE="${ROOT}/.cache/vcpkg/${ARCH}/archives"
  export VCPKG_BINARY_SOURCES="clear;files,${VCPKG_DEFAULT_BINARY_CACHE},readwrite"
  export VCPKG_OVERLAY_TRIPLETS="${ROOT}/cmake/vcpkg-triplets"
  export VCPKG_DISABLE_METRICS=1 VCPKG_MAX_CONCURRENCY=8
  export CCACHE_DIR="${ROOT}/.cache/ccache/${ARCH}" CCACHE_BASEDIR="${ROOT}"
  export CCACHE_COMPILERCHECK=content CCACHE_MAXSIZE=1G
  mkdir -p "${VCPKG_DOWNLOADS}" "${VCPKG_DEFAULT_BINARY_CACHE}" "${CCACHE_DIR}"
  local baseline
  baseline="$(python3 -c 'import json; print(json.load(open("vcpkg-configuration.json"))["default-registry"]["baseline"])')"
  if [[ ! -d "${VCPKG_ROOT}/.git" ]]; then
    mkdir -p "${VCPKG_ROOT}"
    git -C "${VCPKG_ROOT}" init -q
    git -C "${VCPKG_ROOT}" remote add origin https://github.com/microsoft/vcpkg.git
  fi
  if ! git -C "${VCPKG_ROOT}" cat-file -e "${baseline}^{commit}" 2>/dev/null; then
    git -C "${VCPKG_ROOT}" fetch --depth 1 origin "${baseline}"
  fi
  git -C "${VCPKG_ROOT}" checkout --detach "${baseline}"
  [[ "$(git -C "${VCPKG_ROOT}" rev-parse HEAD)" == "${baseline}" ]] || fail 'vcpkg baseline 不一致'
  if [[ ! -x "${VCPKG_ROOT}/vcpkg" ]]; then
    "${VCPKG_ROOT}/bootstrap-vcpkg.sh" -disableMetrics
  fi
  log "配置 ${ARCH} RelWithDebInfo，完整测试开启，strip=${STRIP_DEBUG}"
  cmake -S . -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14 \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
    -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
    -DMSKDSP_BUILD_TESTS=ON -DMSKDSP_STRIP_DEBUG="${STRIP_DEBUG}" \
    -DVCPKG_HOST_TRIPLET="${TRIPLET}" -DVCPKG_TARGET_TRIPLET="${TRIPLET}" \
    -DProtobuf_PROTOC_EXECUTABLE="${ROOT}/${BUILD_DIR}/vcpkg_installed/${TRIPLET}/tools/protobuf/protoc" \
    -DgRPC_CPP_PLUGIN_EXECUTABLE="${ROOT}/${BUILD_DIR}/vcpkg_installed/${TRIPLET}/tools/grpc/grpc_cpp_plugin"
  ccache --zero-stats
  log "编译 ${ARCH}，并行度 8"
  cmake --build "${BUILD_DIR}" --parallel 8
  log "执行 ${ARCH} 完整 CTest，并行度 2"
  ctest --test-dir "${BUILD_DIR}" --output-on-failure --parallel 2 --no-tests=error \
    --output-junit "${ROOT}/${LOG_DIR}/ctest.xml"
  log '测试成功，安装运行文件并归档独立调试符号'
  cmake --install "${BUILD_DIR}"
  find package -maxdepth 1 -type f -name '*_test' -delete
  if [[ "${ARCH}" == arm64 ]]; then
    [[ -d package/debug ]] || fail 'ARM64 安装结果缺少独立调试符号目录'
    local symbol_name
    symbol_name="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "${RELEASE_DIR}/build_identity.json")"
    tar -C package -czf "${RELEASE_DIR}/mskdsp-${symbol_name}-debugsymbols-linux-${ARCH}.tar.gz" debug
  fi
  log "${ARCH} 编译、完整测试与安装完成"
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
  package) package_release ;;
  *) fail '用法：build_lower.sh build|package arm64|x64' ;;
esac
