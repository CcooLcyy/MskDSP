#!/usr/bin/env bash
set -euo pipefail

RUNNER_HOME="${RUNNER_HOME:-/opt/actions-runner}"
RUNNER_STATE_DIRECTORY="${RUNNER_STATE_DIRECTORY:-/var/lib/actions-runner}"
RUNNER_NAME="${RUNNER_NAME:-$(hostname)}"
RUNNER_LABELS="${RUNNER_LABELS:-lower-builder,x64}"
RUNNER_GROUP="${RUNNER_GROUP:-Default}"
RUNNER_WORK_DIRECTORY="${RUNNER_WORK_DIRECTORY:-_work}"
RUNNER_EPHEMERAL="${RUNNER_EPHEMERAL:-true}"
RUNNER_DISABLE_UPDATE="${RUNNER_DISABLE_UPDATE:-false}"
RUNNER_REMOVE_TOKEN="${RUNNER_REMOVE_TOKEN:-}"
CONFIGURED=false
RUNNER_PID=""
STATE_FILES=(.runner .credentials .credentials_rsaparams .path)

fail() {
  printf '错误：%s\n' "$*" >&2
  exit 1
}

require_value() {
  local name="$1"
  local value="$2"
  [[ -n "${value}" ]] || fail "${name} 不能为空"
}

configure_docker_group() {
  local socket=/var/run/docker.sock
  local socket_gid

  [[ -e "${socket}" ]] || return 0
  [[ -S "${socket}" ]] || fail "Docker socket 不是 socket：${socket}"

  socket_gid="$(stat --format '%g' "${socket}")"
  local socket_group
  socket_group="$(getent group "${socket_gid}" | cut --delimiter=: --fields=1 || true)"
  if [[ -z "${socket_group}" ]]; then
    socket_group="docker-host-${socket_gid}"
    groupadd --gid "${socket_gid}" "${socket_group}"
  fi
  usermod --append --groups "${socket_group}" runner
}

build_config_args() {
  CONFIG_ARGS=(
    --unattended
    --url "${GITHUB_URL}"
    --token "${RUNNER_TOKEN}"
    --name "${RUNNER_NAME}"
    --runnergroup "${RUNNER_GROUP}"
    --labels "${RUNNER_LABELS}"
    --work "${RUNNER_WORK_DIRECTORY}"
    --replace
  )
  if [[ "${RUNNER_EPHEMERAL}" == "true" ]]; then
    CONFIG_ARGS+=(--ephemeral)
  fi
  if [[ "${RUNNER_DISABLE_UPDATE}" == "true" ]]; then
    CONFIG_ARGS+=(--disableupdate)
  fi
}

restore_runner_state() {
  mkdir --parents "${RUNNER_STATE_DIRECTORY}"
  for state_file in "${STATE_FILES[@]}"; do
    if [[ -f "${RUNNER_STATE_DIRECTORY}/${state_file}" ]]; then
      install --owner runner --group runner --mode 0600 \
        "${RUNNER_STATE_DIRECTORY}/${state_file}" "${RUNNER_HOME}/${state_file}"
    fi
  done
}

persist_runner_state() {
  mkdir --parents "${RUNNER_STATE_DIRECTORY}"
  for state_file in "${STATE_FILES[@]}"; do
    if [[ -f "${RUNNER_HOME}/${state_file}" ]]; then
      install --owner runner --group runner --mode 0600 \
        "${RUNNER_HOME}/${state_file}" "${RUNNER_STATE_DIRECTORY}/${state_file}"
    fi
  done
}

runner_state_is_configured() {
  [[ -s "${RUNNER_STATE_DIRECTORY}/.runner" ]] && \
    [[ -s "${RUNNER_STATE_DIRECTORY}/.credentials" ]]
}

cleanup_runner() {
  if [[ "${CONFIGURED}" != "true" || -z "${RUNNER_REMOVE_TOKEN}" ]]; then
    return
  fi

  printf '正在注销 GitHub Actions runner：%s\n' "${RUNNER_NAME}"
  gosu runner "${RUNNER_HOME}/config.sh" remove --unattended --token "${RUNNER_REMOVE_TOKEN}" || \
    printf '警告：runner 注销失败，GitHub 将在离线超时后清理其记录。\n' >&2
}

stop_runner() {
  if [[ -n "${RUNNER_PID}" ]]; then
    kill --signal TERM "${RUNNER_PID}" 2>/dev/null || true
    wait "${RUNNER_PID}" || true
    RUNNER_PID=""
  fi
  exit 0
}

run_as_runner() {
  cd "${RUNNER_HOME}"
  gosu runner "$@"
}

require_value "GITHUB_URL" "${GITHUB_URL:-}"

if [[ "${RUNNER_TEST_MODE:-false}" == "1" ]]; then
  mkdir --parents "${RUNNER_STATE_DIRECTORY}"
  if runner_state_is_configured; then
    printf '%s\n' 'configured-state-reused'
    exit 0
  fi
  require_value "RUNNER_TOKEN" "${RUNNER_TOKEN:-}"
  build_config_args
  printf '%q ' "${CONFIG_ARGS[@]}"
  printf '\n'
  exit 0
fi

[[ -x "${RUNNER_HOME}/config.sh" ]] || fail "未找到 GitHub Actions Runner：${RUNNER_HOME}/config.sh"
[[ -x "${RUNNER_HOME}/run.sh" ]] || fail "未找到 GitHub Actions Runner：${RUNNER_HOME}/run.sh"

configure_docker_group
mkdir --parents "${RUNNER_HOME}/${RUNNER_WORK_DIRECTORY}" /home/runner/.docker
chown --recursive runner:runner "${RUNNER_HOME}/${RUNNER_WORK_DIRECTORY}" /home/runner/.docker
restore_runner_state

cleanup() {
  persist_runner_state
  cleanup_runner
}
trap cleanup EXIT
trap stop_runner INT TERM

if runner_state_is_configured; then
  CONFIGURED=true
  printf '复用已注册的 GitHub Actions runner：%s\n' "${RUNNER_NAME}"
else
  require_value "RUNNER_TOKEN" "${RUNNER_TOKEN:-}"
  build_config_args
  printf '注册 GitHub Actions runner：%s\n' "${RUNNER_NAME}"
  run_as_runner "${RUNNER_HOME}/config.sh" "${CONFIG_ARGS[@]}"
  CONFIGURED=true
  persist_runner_state
fi

printf 'runner 已就绪，标签：%s\n' "${RUNNER_LABELS}"
run_as_runner "${RUNNER_HOME}/run.sh" &
RUNNER_PID=$!
set +e
wait "${RUNNER_PID}"
runner_exit_code=$?
set -e
RUNNER_PID=""
exit "${runner_exit_code}"
