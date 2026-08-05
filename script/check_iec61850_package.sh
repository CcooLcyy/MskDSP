#!/usr/bin/env bash

set -euo pipefail

package_dir="${1:-}"
expected_arch="${2:-aarch64}"
allow_tests="${3:-false}"
require_config="${4:-false}"

if [[ -z "${package_dir}" || ! -d "${package_dir}" ]]; then
  echo "用法: $0 <package目录> [aarch64|x86-64] [允许测试 true|false] [要求配置 true|false]" >&2
  exit 2
fi

if [[ "${allow_tests}" != "true" && "${allow_tests}" != "false" ]]; then
  echo "允许测试参数必须是 true 或 false: ${allow_tests}" >&2
  exit 2
fi
if [[ "${require_config}" != "true" && "${require_config}" != "false" ]]; then
  echo "要求配置参数必须是 true 或 false: ${require_config}" >&2
  exit 2
fi

case "${expected_arch}" in
  aarch64) expected_pattern='ELF 64-bit.*ARM aarch64' ;;
  x86-64) expected_pattern='ELF 64-bit.*x86-64' ;;
  *)
    echo "不支持的目标架构: ${expected_arch}" >&2
    exit 2
    ;;
esac

mapfile -t elf_files < <(find "${package_dir}" -type f -print0 | xargs -0 -r file -L | awk -F: '/ELF 64-bit/ {sub(/^ /, "", $1); print $1}')
if (( ${#elf_files[@]} == 0 )); then
  echo "包中没有找到ELF文件: ${package_dir}" >&2
  exit 1
fi

failed=0
for elf in "${elf_files[@]}"; do
  description="$(file -L "${elf}")"
  if ! [[ "${description}" =~ ${expected_pattern} ]]; then
    echo "架构不匹配: ${elf}: ${description}" >&2
    failed=1
  fi

  if [[ "${allow_tests}" != "true" && "${elf}" =~ (^|/)[^/]*_test$ ]]; then
    echo "生产包包含测试二进制: ${elf}" >&2
    failed=1
  fi

  runpath="$(readelf -d "${elf}" 2>/dev/null | sed -n 's/.*Library runpath: \[\(.*\)\].*/\1/p')"
  if [[ "${runpath}" == */data/* || "${runpath}" == *build-* || "${runpath}" == *build/* ]]; then
    echo "RUNPATH包含构建机绝对路径: ${elf}: ${runpath}" >&2
    failed=1
  fi
done

if [[ "${require_config}" == "true" ]]; then
  required_configs=(
    "${package_dir}/conf/module_manager.jsonc"
    "${package_dir}/conf/configPusher/iec61850.jsonc"
  )
  for config_file in "${required_configs[@]}"; do
    if [[ ! -f "${config_file}" ]]; then
      echo "生产包缺少配置模板: ${config_file}" >&2
      failed=1
    fi
  done
fi

if (( failed != 0 )); then
  exit 1
fi
echo "IEC61850交付包静态检查通过: 架构=${expected_arch}, ELF数量=${#elf_files[@]}"
