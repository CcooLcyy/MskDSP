#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 4 ]]; then
  echo "Usage: $0 <package_dir> <debug_dir> <strip_tool> <objcopy_tool>" >&2
  exit 2
fi

package_dir="$1"
debug_dir="$2"
strip_tool="$3"
objcopy_tool="$4"

if [[ ! -d "$package_dir" ]]; then
  echo "strip_debug: package dir not found: $package_dir" >&2
  exit 1
fi

mkdir -p "$debug_dir"
package_dir="$(cd "$package_dir" && pwd)"
debug_dir="$(cd "$debug_dir" && pwd)"

if ! command -v "$strip_tool" >/dev/null 2>&1; then
  echo "strip_debug: strip tool not found: $strip_tool" >&2
  exit 1
fi

if ! command -v "$objcopy_tool" >/dev/null 2>&1; then
  echo "strip_debug: objcopy tool not found: $objcopy_tool" >&2
  exit 1
fi

if ! command -v file >/dev/null 2>&1; then
  echo "strip_debug: file(1) not found in PATH" >&2
  exit 1
fi

echo "strip_debug: package=${package_dir} debug=${debug_dir}"

count=0
skipped=0

while IFS= read -r -d '' path; do
  base="$(basename "$path")"
  if [[ "$base" == *_test* ]] || [[ "$path" == *.debug ]]; then
    skipped=$((skipped + 1))
    continue
  fi
  if ! file -b "$path" | grep -q 'ELF'; then
    continue
  fi

  rel="${path#$package_dir/}"
  debug_path="${debug_dir}/${rel}.debug"
  mkdir -p "$(dirname "$debug_path")"

  echo "strip_debug: ${rel} -> ${debug_path}"
  if ! "$objcopy_tool" --only-keep-debug "$path" "$debug_path"; then
    echo "strip_debug: skip ${rel} (objcopy failed)" >&2
    rm -f "$debug_path"
    skipped=$((skipped + 1))
    continue
  fi
  "$strip_tool" --strip-unneeded "$path"
  if ! "$objcopy_tool" --add-gnu-debuglink="$debug_path" "$path"; then
    echo "strip_debug: 添加 debuglink 失败，尝试移除旧 debuglink 后重试: ${rel}" >&2
    if "$objcopy_tool" --remove-section .gnu_debuglink "$path"; then
      "$objcopy_tool" --add-gnu-debuglink="$debug_path" "$path"
    else
      echo "strip_debug: 移除旧 debuglink 失败，跳过 ${rel}" >&2
      skipped=$((skipped + 1))
      continue
    fi
  fi
  count=$((count + 1))
done < <(find "$package_dir" -type f ! -path "$debug_dir/*" -print0)

echo "strip_debug: processed ${count} file(s), skipped ${skipped}"
