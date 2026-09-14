#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'USAGE'
用法:
  bash script/workflow/upload_r2.sh \
    --endpoint https://<account-id>.r2.cloudflarestorage.com \
    --bucket mskdsp-update \
    --prefix mskdsp-lower/stable \
    --public-base-url https://pub-xxxx.r2.dev \
    --platform linux-arm64 \
    --package-file images/mskdsp-package \
    --checksum-file SHA256SUMS \
    --manifest-file latest.json \
    [--debug-file debug.tar.gz]
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

ENDPOINT=""
BUCKET=""
PREFIX=""
PUBLIC_BASE_URL=""
PLATFORM=""
PACKAGE_FILE=""
CHECKSUM_FILE=""
MANIFEST_FILE=""
DEBUG_FILE=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --endpoint) ENDPOINT="${2:-}"; shift 2 ;;
    --bucket) BUCKET="${2:-}"; shift 2 ;;
    --prefix) PREFIX="${2:-}"; shift 2 ;;
    --public-base-url) PUBLIC_BASE_URL="${2:-}"; shift 2 ;;
    --platform) PLATFORM="${2:-}"; shift 2 ;;
    --package-file) PACKAGE_FILE="${2:-}"; shift 2 ;;
    --checksum-file) CHECKSUM_FILE="${2:-}"; shift 2 ;;
    --manifest-file) MANIFEST_FILE="${2:-}"; shift 2 ;;
    --debug-file) DEBUG_FILE="${2:-}"; shift 2 ;;
    -h|--help) usage ;;
    *) usage ;;
  esac
done

require_cmd aws

[[ -n "$ENDPOINT" ]] || die "--endpoint 不能为空"
[[ -n "$BUCKET" ]] || die "--bucket 不能为空"
[[ -n "$PREFIX" ]] || die "--prefix 不能为空"
[[ -n "$PUBLIC_BASE_URL" ]] || die "--public-base-url 不能为空"
[[ "$PUBLIC_BASE_URL" == http://* || "$PUBLIC_BASE_URL" == https://* ]] || die "--public-base-url 必须使用 http 或 https"
[[ -n "$PLATFORM" ]] || die "--platform 不能为空"
[[ -n "${AWS_ACCESS_KEY_ID:-}" ]] || die "缺少 AWS_ACCESS_KEY_ID，请配置 R2_ACCESS_KEY_ID secret"
[[ -n "${AWS_SECRET_ACCESS_KEY:-}" ]] || die "缺少 AWS_SECRET_ACCESS_KEY，请配置 R2_SECRET_ACCESS_KEY secret"
[[ -f "$PACKAGE_FILE" ]] || die "未找到安装包: $PACKAGE_FILE"
[[ -f "$CHECKSUM_FILE" ]] || die "未找到校验文件: $CHECKSUM_FILE"
[[ -f "$MANIFEST_FILE" ]] || die "未找到清单文件: $MANIFEST_FILE"

S3_ARGS=(--endpoint-url "$ENDPOINT" --region auto)
DEST="s3://${BUCKET}/${PREFIX}"

upload() {
  local source="$1"
  local key="$2"
  local content_type="$3"
  local cache_control="$4"
  echo "上传 R2 对象: ${key}"
  aws s3 cp "$source" "${DEST}/${key}" "${S3_ARGS[@]}" \
    --content-type "$content_type" \
    --cache-control "$cache_control"
}

# 先上传版本化资产，避免 latest.json 指向尚未完成的对象。
upload "$PACKAGE_FILE" "${PLATFORM}/$(basename "$PACKAGE_FILE")" \
  "application/octet-stream" "public,max-age=31536000,immutable"
upload "$CHECKSUM_FILE" "${PLATFORM}/$(basename "$CHECKSUM_FILE")" \
  "text/plain; charset=utf-8" "public,max-age=300"
if [[ -n "$DEBUG_FILE" ]]; then
  [[ -f "$DEBUG_FILE" ]] || die "未找到调试符号包: $DEBUG_FILE"
  upload "$DEBUG_FILE" "${PLATFORM}/$(basename "$DEBUG_FILE")" \
    "application/gzip" "public,max-age=31536000,immutable"
fi

# 清单中的 URL 已由调用方按公共开发地址生成；最后上传清单。
upload "$MANIFEST_FILE" "latest.json" \
  "application/json; charset=utf-8" "no-store"

# 清单已发布后删除同一通道/平台的旧版本，避免 R2 中长期堆积旧包。
CURRENT_NAMES=("$(basename "$PACKAGE_FILE")" "$(basename "$CHECKSUM_FILE")")
if [[ -n "$DEBUG_FILE" ]]; then CURRENT_NAMES+=("$(basename "$DEBUG_FILE")"); fi
mapfile -t OLD_KEYS < <(
  aws s3api list-objects-v2 "${S3_ARGS[@]}" \
    --bucket "$BUCKET" \
    --prefix "${PREFIX}/${PLATFORM}/" \
    --query 'Contents[].Key' \
    --output text | tr '\t' '\n' | sed '/^None$/d;/^$/d'
)
for key in "${OLD_KEYS[@]}"; do
  name="${key##*/}"
  keep=false
  for current in "${CURRENT_NAMES[@]}"; do
    if [[ "$name" == "$current" ]]; then keep=true; break; fi
  done
  if [[ "$keep" != true ]]; then
    echo "清理 R2 旧对象: ${key}"
    aws s3 rm "s3://${BUCKET}/${key}" "${S3_ARGS[@]}"
  fi
done

echo "R2 上传完成: ${PUBLIC_BASE_URL%/}/${PREFIX}/latest.json"
