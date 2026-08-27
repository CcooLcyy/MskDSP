#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENTRYPOINT="${SCRIPT_DIR}/entrypoint.sh"

fail() {
  printf '测试失败：%s\n' "$*" >&2
  exit 1
}

expect_failure() {
  local expected="$1"
  shift

  local output
  if output="$("$@" 2>&1)"; then
    fail "命令应失败：$*"
  fi
  [[ "${output}" == *"${expected}"* ]] || fail "未找到预期错误：${expected}，实际输出：${output}"
}

bash -n "${ENTRYPOINT}"

# 验证缺少仓库地址时拒绝注册，避免向错误的 GitHub 目标发送 token。
expect_failure "GITHUB_URL 不能为空" env -i PATH="${PATH}" RUNNER_TOKEN=test RUNNER_TEST_MODE=1 bash "${ENTRYPOINT}"

# 验证缺少一次性 token 时拒绝启动，防止镜像或卷中持久化注册凭据。
expect_failure "RUNNER_TOKEN 不能为空" env -i PATH="${PATH}" GITHUB_URL=https://github.com/example/repo RUNNER_TEST_MODE=1 bash "${ENTRYPOINT}"

# 验证默认长期 runner 配置不启用 ephemeral，避免 job 完成后自动移除。
output="$(env -i PATH="${PATH}" GITHUB_URL=https://github.com/example/repo RUNNER_TOKEN=test RUNNER_LABELS=lower-builder,x64 RUNNER_EPHEMERAL=false RUNNER_TEST_MODE=1 bash "${ENTRYPOINT}")"
[[ "${output}" != *"--ephemeral"* ]] || fail "长期 runner 不应启用 ephemeral"
[[ "${output}" == *"--labels lower-builder\\,x64"* ]] || fail "未传递 runner 标签"

printf '入口脚本测试通过。\n'
