#!/usr/bin/env bash
# 只上传本次冻结产物到既有平台，不执行设备安装。
set -euo pipefail
: "${PLATFORM_UPLOAD_TOKEN:?缺少平台上传令牌}"
: "${PLATFORM_GRPC_ENDPOINT:?缺少托管 Runner 可达的平台 gRPC 地址}"
: "${PLATFORM_PUBLIC_BASE_URL:?缺少托管 Runner 可达的平台公开清单地址}"
: "${LOWER_JOB_ARCH:?缺少发布架构}"
case "${LOWER_JOB_ARCH}" in arm64|x64) ;; *) echo '错误：发布架构不合法' >&2; exit 1 ;; esac
tar -xf "${RUNNER_TEMP}/lower-package/lower-package-${LOWER_JOB_ARCH}.tar"
python3 -m venv "${RUNNER_TEMP}/lower-publish-python"
source "${RUNNER_TEMP}/lower-publish-python/bin/activate"
python -m pip install grpcio grpcio-tools
if [[ -n "${PLATFORM_GRPC_CA_B64:-}" ]]; then
  umask 077
  export PLATFORM_GRPC_CA_FILE="${RUNNER_TEMP}/lower-platform-ca.pem"
  printf '%s' "${PLATFORM_GRPC_CA_B64}" | base64 -d > "${PLATFORM_GRPC_CA_FILE}"
  trap 'rm -f -- "${PLATFORM_GRPC_CA_FILE:-}"' EXIT
fi
python script/gitlab/init_platform_proto.py
python script/gitlab/platform_client.py --metadata "release/${LOWER_JOB_ARCH}/metadata.json" --proto-root .ci/platform-proto
