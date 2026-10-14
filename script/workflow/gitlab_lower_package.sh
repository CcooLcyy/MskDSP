#!/usr/bin/env bash
set -euo pipefail

die() { echo "错误: $*" >&2; exit 1; }

cd "$(git rev-parse --show-toplevel)"
[[ -f build.env ]] || die "缺少构建任务传递的 build.env"
expected_arch="${ARCH_SLUG:?必须指定 ARCH_SLUG}"
expected_channel="${CHANNEL:?必须指定 CHANNEL}"
# 源引用和产物名来自构建任务；source 前先校验键名与值，拒绝任意 shell 代码。
python3 - <<'PY'
import re
from pathlib import Path
keys = set("ARCH_SLUG CHANNEL MAKE_PACKAGE TARGET_REF SOURCE_SHA DRIVER_SHA PACKAGE_ARCH SOURCE_REF PLATFORM DOCKER_PLATFORM R2_PREFIX RELEASE_TAG BETA_LINE BETA_TAG BETA_TAG_PREFIX NOTES_START_TAG IMAGE_VERSION ARCHIVE_NAME DEBUG_ARCHIVE_NAME".split())
seen = set()
for line in Path("build.env").read_text(encoding="utf-8").splitlines():
    key, separator, value = line.partition("=")
    if not separator or key not in keys or key in seen or not re.fullmatch(r"[A-Za-z0-9_./-]*", value):
        raise SystemExit("build.env 存在未知、重复或不安全的元数据")
    seen.add(key)
if seen != keys:
    raise SystemExit("build.env 缺少必需的元数据")
PY
source build.env
[[ "${ARCH_SLUG}" == "${expected_arch}" && "${CHANNEL}" == "${expected_channel}" ]] || die "构建产物架构或渠道与打包任务不一致"
[[ "${MAKE_PACKAGE}" == true ]] || die "本次构建没有生成可发布的安装目录"
[[ "${SOURCE_SHA}" =~ ^[0-9a-f]{40}$ ]] || die "构建提交摘要不合法"
[[ "${ARCHIVE_NAME}" == "mskdsp-${IMAGE_VERSION}" && "${ARCHIVE_NAME}" != */* && "${DEBUG_ARCHIVE_NAME}" != */* ]] || die "产物名称不合法"
case "${ARCH_SLUG}:${PLATFORM}:${DOCKER_PLATFORM}:${PACKAGE_ARCH}" in
  arm64:linux-arm64:linux/arm64:aarch64) base_image=arm64v8/ubuntu:noble ;;
  x64:linux-x64:linux/amd64:x86-64) base_image=amd64/ubuntu:noble ;;
  *) die "构建元数据中的架构映射不一致" ;;
esac
for command in docker python3 tar sha256sum file readelf; do
  command -v "${command}" >/dev/null 2>&1 || die "打包镜像缺少工具: ${command}"
done
docker info >/dev/null || die "无法访问 Docker，需要配置独立打包 Runner 的 Docker socket"
docker buildx version >/dev/null || die "打包 Runner 未安装 Docker Buildx"
bash script/check_iec61850_package.sh package "${PACKAGE_ARCH}" false true
if [[ -d package/debug ]]; then
  echo "先保存独立调试符号包，避免 make_exe 清理 package/debug"
  tar -C package -czf "${DEBUG_ARCHIVE_NAME}.tar.gz" debug
fi

temporary_dir="$(mktemp -d)"
builder_name="mskdsp-${CI_JOB_ID:-$$}"
builder_created=false
image_ref="mskdsp:${IMAGE_VERSION}"
source_make_exe="script/.gitlab-source-make-exe-${CI_JOB_ID:-$$}.sh"
cleanup() {
  local result=$?
  if [[ "${builder_created}" == true ]]; then
    docker buildx rm "${builder_name}" >/dev/null 2>&1 || echo "清理本任务 Buildx 实例失败" >&2
  fi
  docker image rm "${image_ref}" >/dev/null 2>&1 || true
  rm -f "${source_make_exe}"
  rm -rf "${temporary_dir}"
  exit "${result}"
}
trap cleanup EXIT

# 构建任务可能选择旧 tag；从其提交读取 Dockerfile，同时保留当前流水线驱动脚本。
git cat-file -e "${SOURCE_SHA}^{commit}" 2>/dev/null || git fetch --no-recurse-submodules origin "${SOURCE_SHA}"
git show "${SOURCE_SHA}:Dockerfile" | \
  sed -e "s#--platform=linux/arm64#--platform=${DOCKER_PLATFORM}#" \
      -e "s#localhost/arm64v8/ubuntu:noble#${base_image}#" > "${temporary_dir}/Dockerfile"
git show "${SOURCE_SHA}:script/make_exe.sh" > "${source_make_exe}"
echo "构建 ${DOCKER_PLATFORM} 运行镜像，使用独立 Buildx 实例"
docker buildx create --name "${builder_name}" --driver docker-container >/dev/null
builder_created=true
mkdir -p images
image_tar="images/${ARCHIVE_NAME}.tar"
docker buildx build --builder "${builder_name}" --platform "${DOCKER_PLATFORM}" \
  --load -t "${image_ref}" -f "${temporary_dir}/Dockerfile" .
docker save -o "${image_tar}" "${image_ref}"
echo "生成与 GitHub 相同的 gzip 自解压安装包"
(
  cd script
  bash "./${source_make_exe#script/}" "../${image_tar}"
)
assets=("images/${ARCHIVE_NAME}")
if [[ -f "${DEBUG_ARCHIVE_NAME}.tar.gz" ]]; then assets+=("${DEBUG_ARCHIVE_NAME}.tar.gz"); fi
sha256sum "${assets[@]}" > SHA256SUMS
public_base="${R2_PUBLIC_BASE_URL:-https://update.clsclear.top}"
if [[ "${ARCH_SLUG}" == arm64 ]]; then product_prefix=mskdsp-lower; else product_prefix=mskdsp-lower-x64; fi
echo "生成 ${CHANNEL}/${PLATFORM} 静态更新清单"
python3 script/workflow/write_lower_update_manifest.py \
  --channel "${CHANNEL}" --platform "${PLATFORM}" --version "${IMAGE_VERSION}" \
  --image-tar "${image_tar}" --artifact "images/${ARCHIVE_NAME}" \
  --checksums SHA256SUMS --output latest.json \
  --base-url "${public_base%/}/${product_prefix}" \
  --repository "${CI_PROJECT_PATH:?缺少 GitLab 项目路径}" \
  --source-ref "${SOURCE_REF}" --source-sha "${SOURCE_SHA}"
rm -f "${image_tar}"
echo "安装包、调试符号、SHA256SUMS 和 latest.json 已生成，等待发布任务"
