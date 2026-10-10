#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "用法: $0 [镜像tar包路径|版本号]" >&2
  exit 1
}

die() {
  echo "Error: $*" >&2
  exit 1
}

extract_version_from_tar() {
  local tar_path="$1"
  local repo_tag=""
  if ! repo_tag="$(
    python3 - "${tar_path}" <<'PY'
import json
import sys
import tarfile

tar_path = sys.argv[1]
with tarfile.open(tar_path, "r:*") as tf:
    try:
        mf = tf.extractfile("manifest.json")
    except KeyError:
        mf = None
    if mf is None:
        sys.exit(1)
    manifest = json.load(mf)

if not isinstance(manifest, list) or not manifest:
    sys.exit(1)

repo_tags = manifest[0].get("RepoTags") or []
if not repo_tags or not repo_tags[0]:
    sys.exit(1)

print(repo_tags[0])
PY
  )"; then
    die "无法从 tar 包中提取镜像标签: ${tar_path}"
  fi

  if [[ "${repo_tag}" != *:* ]]; then
    die "tar 包中的镜像标签格式不正确: ${repo_tag}"
  fi

  printf '%s\n' "${repo_tag##*:}"
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PACKAGE_DIR="${PROJECT_ROOT}/package"

INPUT_ARG=""
if [[ $# -eq 0 ]]; then
  VERSION_FILE="${PROJECT_ROOT}/VERSION"
  if [[ ! -f "${VERSION_FILE}" ]]; then
    die "未找到版本文件: ${VERSION_FILE}"
  fi
  INPUT_ARG="$(
    awk '
      {
        sub(/\r$/, "")
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", $0)
        if ($0 != "") {
          print $0
          exit
        }
      }
    ' "${VERSION_FILE}"
  )"
  if [[ -z "${INPUT_ARG}" ]]; then
    die "版本文件内容为空: ${VERSION_FILE}"
  fi
  echo "未传入参数，默认使用版本文件 ${VERSION_FILE} 中的版本: ${INPUT_ARG}"
elif [[ $# -eq 1 ]]; then
  INPUT_ARG="$1"
else
  usage
fi

rm -rf "${PACKAGE_DIR}/module"
rm -rf "${PACKAGE_DIR}/lib"
echo "清理旧的 package/module 和 package/lib 目录"
rm -rf "${PACKAGE_DIR}/log"
rm -rf "${PACKAGE_DIR}/socket"
echo "清理旧的 package/log 和 package/socket 目录"
rm -rf "${PACKAGE_DIR}/debug"
echo "清理旧的 package/debug 目录"

INPUT_TAR=""
VERSION=""
if [[ -f "${INPUT_ARG}" ]]; then
  INPUT_TAR="${INPUT_ARG}"
  VERSION="$(extract_version_from_tar "${INPUT_TAR}")"
else
  if [[ "${INPUT_ARG}" == *"/"* || "${INPUT_ARG}" == *.tar ]]; then
    die "Input tar not found: ${INPUT_ARG}"
  fi
  VERSION="${INPUT_ARG}"
  MAKE_IMAGE_SCRIPT="${SCRIPT_DIR}/make_image.sh"
  if [[ ! -f "${MAKE_IMAGE_SCRIPT}" ]]; then
    die "make_image.sh not found: ${MAKE_IMAGE_SCRIPT}"
  fi
  bash "${MAKE_IMAGE_SCRIPT}" "${VERSION}"
  INPUT_TAR="${PROJECT_ROOT}/images/mskdsp-${VERSION}.tar"
  if [[ ! -f "${INPUT_TAR}" ]]; then
    die "Generated tar not found: ${INPUT_TAR}"
  fi
fi

OUTPUT_PATH="${PROJECT_ROOT}/images/mskdsp-${VERSION}"

mkdir -p "$(dirname "${OUTPUT_PATH}")"

cat > "${OUTPUT_PATH}" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

SCRIPT_NAME="$(basename "$0")"
CONTAINER_NAME="mskdsp"
HOST_DIR="/data/mskdsp"
CONTAINER_DIR="/opt/mskdsp"
IMAGE_TAG=""
IMAGE_ID=""
IMAGE_MANIFEST_ID=""
LOCAL_IMAGE_ID=""
PAYLOAD_TAR=""
PAYLOAD_TMP_DIR=""
NEED_UPDATE=0
CLEANUP_REPO=0
IMAGE_REPO=""

usage() {
  cat <<USAGE
Usage: ${SCRIPT_NAME} start [-- <docker run extra args>]
       ${SCRIPT_NAME} stop
USAGE
}

die() {
  echo "Error: $*" >&2
  exit 1
}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "Missing command: $1"
}

cleanup_payload() {
  if [[ -n "${PAYLOAD_TAR}" && -f "${PAYLOAD_TAR}" ]]; then
    rm -f "${PAYLOAD_TAR}"
  fi
  if [[ -n "${PAYLOAD_TMP_DIR}" ]]; then
    rmdir "${PAYLOAD_TMP_DIR}" 2>/dev/null || true
  fi
  PAYLOAD_TAR=""
  PAYLOAD_TMP_DIR=""
}

payload_start_line() {
  awk '/^__ARCHIVE_BELOW__$/ {print NR+1; exit 0;}' "$0"
}

extract_payload() {
  local start_line
  start_line="$(payload_start_line)"
  if [[ -z "${start_line}" ]]; then
    die "Payload marker not found."
  fi
  PAYLOAD_TMP_DIR="$(mktemp -d)"
  PAYLOAD_TAR="${PAYLOAD_TMP_DIR}/image.tar"
  tail -n +"${start_line}" "$0" > "${PAYLOAD_TAR}"
}

parse_manifest() {
  local tar_path="$1"
  local parsed
  if ! parsed="$(python3 - "${tar_path}" <<'PY'
import hashlib
import json
import re
import sys
import tarfile

def read_file(tf, name):
    members = [member for member in tf.getmembers() if member.name == name]
    if len(members) != 1:
        sys.exit(f"归档元数据缺失或包含重复路径: {name}")
    member = members[0]
    if not member.isfile():
        sys.exit(f"归档内容不是普通文件: {name}")
    if member.size > 1024 * 1024:
        sys.exit(f"归档元数据超过 1 MiB 限制: {name}")
    return tf.extractfile(member).read()


tar_path = sys.argv[1]
try:
    with tarfile.open(tar_path, "r:*") as tf:
        manifest = json.loads(read_file(tf, "manifest.json"))
        if not isinstance(manifest, list) or len(manifest) != 1:
            sys.exit("安装归档必须只包含一个镜像，不能自动选择多镜像或多平台")
        entry = manifest[0]
        config_path = entry.get("Config") or ""
        match = re.fullmatch(r"(?:blobs/sha256/)?([0-9a-fA-F]{64})(?:\.json)?", config_path)
        if not match:
            sys.exit(f"镜像配置路径格式不合法: {config_path}")
        config_data = read_file(tf, config_path)
        config_hash = hashlib.sha256(config_data).hexdigest()
        if config_hash != match.group(1).lower():
            sys.exit(f"镜像配置内容摘要与归档路径不一致: {config_path}")
        config = json.loads(config_data)
        config_id = "sha256:" + config_hash
        repo_tags = entry.get("RepoTags") or []
        repo_tag = repo_tags[0] if repo_tags else ""
        if not isinstance(repo_tag, str) or ":" not in repo_tag or re.search(r"\s", repo_tag):
            sys.exit("归档缺少合法的镜像标签")

        manifest_id = ""
        if "index.json" in tf.getnames():
            index = json.loads(read_file(tf, "index.json"))
            descriptors = index.get("manifests") or []
            if len(descriptors) != 1:
                sys.exit("OCI 索引必须只包含一个镜像，不能自动选择多镜像或多平台")
            descriptor = descriptors[0]
            manifest_id = descriptor.get("digest", "")
            if not re.fullmatch(r"sha256:[0-9a-f]{64}", manifest_id):
                sys.exit("OCI manifest 摘要格式不合法")
            manifest_data = read_file(tf, "blobs/sha256/" + manifest_id[7:])
            if "sha256:" + hashlib.sha256(manifest_data).hexdigest() != manifest_id:
                sys.exit("OCI manifest 内容摘要与索引不一致")
            image_manifest = json.loads(manifest_data)
            if image_manifest.get("config", {}).get("digest") != config_id:
                sys.exit("OCI manifest 未指向安装归档中的镜像配置")
            platform = descriptor.get("platform") or {}
            for field in ("os", "architecture"):
                if field in platform and platform[field] != config.get(field):
                    sys.exit(f"OCI 索引的平台信息与镜像配置不一致: {field}")
            if "variant" in platform and "variant" in config and platform["variant"] != config["variant"]:
                sys.exit("OCI 索引的平台变体与镜像配置不一致")
        print(config_id)
        print(repo_tag)
        print(manifest_id)
except (KeyError, ValueError, TypeError, AttributeError, tarfile.TarError, OSError) as error:
    sys.exit(f"解析镜像归档失败: {error}")
PY
)"
  then
    die "安装包镜像身份校验失败"
  fi

  local repo_tag
  IMAGE_ID="$(printf '%s\n' "${parsed}" | sed -n '1p')"
  repo_tag="$(printf '%s\n' "${parsed}" | sed -n '2p')"
  IMAGE_MANIFEST_ID="$(printf '%s\n' "${parsed}" | sed -n '3p')"
  if [[ -z "${IMAGE_ID}" || -z "${repo_tag}" ]]; then
    die "镜像归档缺少配置摘要或镜像标签"
  fi

  IMAGE_TAG="${repo_tag}"
  IMAGE_REPO="${IMAGE_TAG%:*}"
  echo "安装包镜像配置摘要: ${IMAGE_ID}，关联 manifest 摘要: ${IMAGE_MANIFEST_ID:-无}"
}

saved_image_config_id() {
  local parser
  parser="$(cat <<'PY'
import hashlib
import json
import re
import sys
import tarfile

try:
    hashes = {}
    manifest = None
    with tarfile.open(fileobj=sys.stdin.buffer, mode="r|*") as tf:
        for member in tf:
            if not member.isfile():
                continue
            if member.name == "manifest.json":
                if manifest is not None:
                    sys.exit("本机镜像导出包含重复的镜像清单")
                if member.size > 1024 * 1024:
                    sys.exit("本机镜像清单超过 1 MiB 限制")
                manifest = json.load(tf.extractfile(member))
            elif re.fullmatch(r"(?:blobs/sha256/)?[0-9a-fA-F]{64}(?:\.json)?", member.name):
                if member.name in hashes:
                    sys.exit("本机镜像导出包含重复的镜像配置路径")
                digest = hashlib.sha256()
                source = tf.extractfile(member)
                for chunk in iter(lambda: source.read(1024 * 1024), b""):
                    digest.update(chunk)
                hashes[member.name] = digest.hexdigest()
    for chunk in iter(lambda: sys.stdin.buffer.read(1024 * 1024), b""):
        pass
    if not isinstance(manifest, list) or len(manifest) != 1:
        sys.exit("本机镜像导出必须只包含一个镜像")
    config_path = manifest[0].get("Config", "")
    match = re.fullmatch(r"(?:blobs/sha256/)?([0-9a-fA-F]{64})(?:\.json)?", config_path)
    if not match or hashes.get(config_path) != match.group(1).lower():
        sys.exit("本机镜像导出的配置摘要校验失败")
    print("sha256:" + hashes[config_path])
except (KeyError, ValueError, TypeError, AttributeError, tarfile.TarError, OSError) as error:
    sys.exit(f"解析本机镜像导出失败: {error}")
PY
)"
  docker image save "$1" | python3 -c "${parser}"
}

image_matches_payload() {
  local local_id="$1"
  local config_id
  [[ -n "${local_id}" ]] || return 1
  if [[ "${local_id}" == "${IMAGE_ID}" || ( -n "${IMAGE_MANIFEST_ID}" && "${local_id}" == "${IMAGE_MANIFEST_ID}" ) ]]; then
    echo "本机镜像身份已与安装包关联: ${local_id}"
    return 0
  fi
  echo "本机镜像身份 ${local_id} 无法直接关联，正在流式核对镜像配置摘要"
  if ! config_id="$(saved_image_config_id "${local_id}")"; then
    echo "警告: 无法验证本机镜像配置摘要" >&2
    return 1
  fi
  echo "本机镜像配置摘要: ${config_id}，安装包配置摘要: ${IMAGE_ID}"
  [[ "${config_id}" == "${IMAGE_ID}" ]]
}

load_image_if_needed() {
  extract_payload
  parse_manifest "${PAYLOAD_TAR}"

  local existing_id
  existing_id="$(docker image inspect --format '{{.Id}}' "${IMAGE_TAG}" 2>/dev/null || true)"
  if image_matches_payload "${existing_id}"; then
    echo "镜像配置相同，跳过加载: ${IMAGE_TAG}"
    LOCAL_IMAGE_ID="${existing_id}"
    cleanup_payload
    NEED_UPDATE=0
    return 0
  fi

  echo "镜像配置尚未匹配，正在加载安装包镜像: ${IMAGE_TAG}"
  if ! docker load -i "${PAYLOAD_TAR}" >/dev/null; then
    die "加载安装包镜像失败，未替换既有业务容器"
  fi
  CLEANUP_REPO=1
  local new_id
  new_id="$(docker image inspect --format '{{.Id}}' "${IMAGE_TAG}" 2>/dev/null || true)"
  if ! image_matches_payload "${new_id}"; then
    die "加载后的镜像身份仍与安装包不一致，未替换既有业务容器"
  fi
  LOCAL_IMAGE_ID="${new_id}"
  if [[ -n "${existing_id}" && -n "${new_id}" && "${existing_id}" != "${new_id}" ]]; then
    docker rmi "${existing_id}" >/dev/null 2>&1 || true
  fi

  NEED_UPDATE=1
  cleanup_payload
}

cleanup_repo_tags() {
  if [[ "${CLEANUP_REPO}" -ne 1 || -z "${IMAGE_REPO}" ]]; then
    return 0
  fi
  while IFS= read -r tag; do
    [[ -n "${tag}" ]] || continue
    if [[ "${tag}" != "${IMAGE_TAG}" ]]; then
      docker rmi "${tag}" >/dev/null 2>&1 || true
    fi
  done < <(docker images --format '{{.Repository}}:{{.Tag}}' "${IMAGE_REPO}" 2>/dev/null | sort -u)
}

ensure_module_dir() {
  local module_dir="${HOST_DIR}/module"
  local need_copy_module=0

  if [[ "${NEED_UPDATE}" -eq 1 ]]; then
    if [[ -d "${module_dir}" ]]; then
      find "${module_dir}" -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null || true
    fi
    need_copy_module=1
  else
    if [[ -z "$(ls -A "${module_dir}" 2>/dev/null || true)" ]]; then
      need_copy_module=1
    fi
  fi

  if [[ "${need_copy_module}" -eq 0 ]]; then
    return 0
  fi

  mkdir -p "${module_dir}"
  if [[ "${NEED_UPDATE}" -eq 1 ]]; then
    echo "镜像已更新，重新同步模块目录到宿主机: ${module_dir}"
  else
    echo "模块目录为空，初始化模块到宿主机: ${module_dir}"
  fi
  local init_name="mskdsp-init-$$"
  docker create --name "${init_name}" "${LOCAL_IMAGE_ID}" >/dev/null
  if ! docker cp "${init_name}:${CONTAINER_DIR}/module/." "${module_dir}/"; then
    docker rm -f "${init_name}" >/dev/null 2>&1 || true
    die "模块目录初始化失败: ${module_dir}"
  fi
  docker rm "${init_name}" >/dev/null
}

ensure_default_conf() {
  local conf_dir="${HOST_DIR}/conf"
  local conf_is_empty=0
  local init_name="mskdsp-conf-init-$$"
  local staging_dir=""
  local staged_config=""

  if [[ ! -d "${conf_dir}" || -z "$(ls -A "${conf_dir}" 2>/dev/null || true)" ]]; then
    conf_is_empty=1
  fi
  mkdir -p "${conf_dir}"

  if ! docker create --name "${init_name}" "${LOCAL_IMAGE_ID}" >/dev/null; then
    die "创建配置同步临时容器失败: ${init_name}"
  fi

  if [[ "${conf_is_empty}" -eq 1 ]]; then
    echo "配置目录为空，初始化默认配置到宿主机: ${conf_dir}"
    if ! docker cp "${init_name}:${CONTAINER_DIR}/conf/." "${conf_dir}/"; then
      docker rm -f "${init_name}" >/dev/null 2>&1 || true
      die "初始化配置目录失败: ${conf_dir}"
    fi
  else
    echo "配置目录已有现场配置，将仅同步模块启动策略并保留其他配置: ${conf_dir}"
  fi

  if ! staging_dir="$(mktemp -d "${conf_dir}/.module-manager-sync.XXXXXX")"; then
    docker rm -f "${init_name}" >/dev/null 2>&1 || true
    die "创建模块启动策略临时目录失败，原配置保持不变: ${conf_dir}"
  fi
  staged_config="${staging_dir}/module_manager.jsonc"

  if ! docker cp "${init_name}:${CONTAINER_DIR}/conf/module_manager.jsonc" "${staged_config}"; then
    rm -f "${staged_config}" 2>/dev/null || true
    rmdir "${staging_dir}" 2>/dev/null || true
    docker rm -f "${init_name}" >/dev/null 2>&1 || true
    die "从镜像同步模块启动策略失败，原配置保持不变: ${conf_dir}/module_manager.jsonc"
  fi
  if [[ ! -s "${staged_config}" ]]; then
    rm -f "${staged_config}" 2>/dev/null || true
    rmdir "${staging_dir}" 2>/dev/null || true
    docker rm -f "${init_name}" >/dev/null 2>&1 || true
    die "镜像中的模块启动策略为空，原配置保持不变: ${CONTAINER_DIR}/conf/module_manager.jsonc"
  fi
  if ! mv -f "${staged_config}" "${conf_dir}/module_manager.jsonc"; then
    rm -f "${staged_config}" 2>/dev/null || true
    rmdir "${staging_dir}" 2>/dev/null || true
    docker rm -f "${init_name}" >/dev/null 2>&1 || true
    die "替换模块启动策略失败，原配置保持不变: ${conf_dir}/module_manager.jsonc"
  fi
  rmdir "${staging_dir}" 2>/dev/null || true
  if ! docker rm "${init_name}" >/dev/null 2>&1; then
    echo "警告: 清理配置同步临时容器失败，可稍后手动删除: ${init_name}" >&2
  fi
  echo "已从镜像同步模块启动策略: ${conf_dir}/module_manager.jsonc"
}

ensure_log_dir() {
  local log_dir="${HOST_DIR}/log"
  if [[ -d "${log_dir}" ]]; then
    return 0
  fi
  mkdir -p "${log_dir}"
  echo "已创建日志目录: ${log_dir}"
}

start_container() {
  require_cmd docker
  require_cmd python3
  trap cleanup_payload EXIT

  load_image_if_needed
  cleanup_repo_tags
  ensure_module_dir
  ensure_default_conf
  ensure_log_dir

  docker rm -f "${CONTAINER_NAME}" >/dev/null 2>&1 || true
  if [[ "${1:-}" == "--" ]]; then
    shift
  fi
  echo "运行容器固定使用 Asia/Shanghai（北京时间，UTC+8），并关闭 Docker 日志收集"
  if ! docker run -d \
    --name "${CONTAINER_NAME}" \
    --privileged \
    --restart unless-stopped \
    --log-driver none \
    --network host \
    -v "${HOST_DIR}/conf:${CONTAINER_DIR}/conf" \
    -v "${HOST_DIR}/module:${CONTAINER_DIR}/module" \
    -v "${HOST_DIR}/log:${CONTAINER_DIR}/log" \
    "$@" \
    "${LOCAL_IMAGE_ID}"; then
    die "运行 mskdsp 容器失败"
  fi
  local running
  running="$(docker inspect --format '{{.State.Running}}' "${CONTAINER_NAME}" 2>/dev/null || true)"
  if [[ "${running}" != "true" ]]; then
    die "mskdsp 容器未处于运行状态，请检查下位机服务日志"
  fi
  echo "mskdsp 容器已处于运行状态，镜像身份校验通过"
}

stop_container() {
  require_cmd docker
  docker rm -f "${CONTAINER_NAME}" >/dev/null 2>&1 || true
}

main() {
  local cmd="${1:-}"
  if [[ -z "${cmd}" ]]; then
    usage
    exit 1
  fi
  shift || true
  case "${cmd}" in
    start)
      start_container "$@"
      ;;
    stop)
      stop_container
      ;;
    *)
      usage
      exit 1
      ;;
  esac
}

main "$@"
exit 0
__ARCHIVE_BELOW__
EOF

cat "${INPUT_TAR}" >> "${OUTPUT_PATH}"
chmod +x "${OUTPUT_PATH}"

echo "Generated ${OUTPUT_PATH}"
