#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
echo '构建下位机独立打包工具镜像'
docker build --tag mskdsp-lower-package-tools:bookworm-v1 \
  --file "${ROOT}/docker/gitlab-ci-tools/Dockerfile" "${ROOT}/docker/gitlab-ci-tools"
