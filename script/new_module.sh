#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
用法:
  bash script/new_module.sh <ModuleName> [--lib-name <LibName>] [--no-proto] [--force]

说明:
  - <ModuleName> 用于生成目录/命名空间/类名/协议文件名（例如 DataCenter、IEC104、MyModule）
  - 默认 <LibName> 从 <ModuleName> 推导：
      * 若 <ModuleName> 形如 DataCenter（第2个字符为小写），默认 LibName=dataCenter
      * 若 <ModuleName> 形如 IEC104（全大写/数字），默认 LibName=IEC104
  - 生成的模块会被追加到 src/CMakeLists.txt 的 add_subdirectory 列表中
  - 同时生成模块文档骨架：src/<ModuleName>/doc/README.md

示例:
  bash script/new_module.sh MyModule
  bash script/new_module.sh DataCenter --lib-name dataCenter
  bash script/new_module.sh IEC104
EOF
}

die() {
  echo "错误: $*" >&2
  exit 1
}

require_arg() {
  local opt="$1"
  local val="${2:-}"
  [[ -n "$val" ]] || die "$opt 需要参数"
}

lower_first_char_if_needed() {
  local name="$1"
  if [[ ${#name} -ge 2 && "${name:0:1}" =~ [A-Z] && "${name:1:1}" =~ [a-z] ]]; then
    echo "$(tr 'A-Z' 'a-z' <<<"${name:0:1}")${name:1}"
    return 0
  fi
  echo "$name"
}

member_prefix() {
  local name="$1"
  if [[ "$name" =~ [a-z] ]]; then
    echo "$(tr 'A-Z' 'a-z' <<<"${name:0:1}")${name:1}"
    return 0
  fi
  tr 'A-Z' 'a-z' <<<"$name"
}

MODULE_NAME=""
LIB_NAME=""
NO_PROTO=0
FORCE=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --lib-name)
      require_arg "$1" "${2:-}"
      LIB_NAME="$2"
      shift 2
      ;;
    --no-proto)
      NO_PROTO=1
      shift
      ;;
    -f|--force)
      FORCE=1
      shift
      ;;
    -*)
      die "未知参数: $1"
      ;;
    *)
      if [[ -n "$MODULE_NAME" ]]; then
        die "重复的模块名参数: $1"
      fi
      MODULE_NAME="$1"
      shift
      ;;
  esac
done

[[ -n "$MODULE_NAME" ]] || { usage; exit 1; }
[[ "$MODULE_NAME" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || die "模块名不合法: $MODULE_NAME"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

SRC_DIR="${ROOT_DIR}/src"
PROTO_DIR="${ROOT_DIR}/protobuf"

[[ -f "${SRC_DIR}/CMakeLists.txt" ]] || die "未找到 ${SRC_DIR}/CMakeLists.txt，请在仓库内运行"
[[ -d "$PROTO_DIR" ]] || die "未找到 ${PROTO_DIR}"
if [[ -f "${PROTO_DIR}/.git" ]]; then
  echo "提示: protobuf 是 git 子模块，脚手架生成/修改 .proto 会影响子模块工作区。" >&2
fi

if [[ -z "$LIB_NAME" ]]; then
  LIB_NAME="$(lower_first_char_if_needed "$MODULE_NAME")"
fi

[[ "$LIB_NAME" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || die "库名不合法: $LIB_NAME"

VAR_PREFIX="$(member_prefix "$MODULE_NAME")"

MODULE_DIR="${SRC_DIR}/${MODULE_NAME}"
MODULE_INCLUDE_DIR="${MODULE_DIR}/include"
MODULE_CMAKE_DIR="${MODULE_DIR}/cmake"
MODULE_DOC_DIR="${MODULE_DIR}/doc"
PROTO_FILE="${PROTO_DIR}/${MODULE_NAME}.proto"

if [[ -e "$MODULE_DIR" && "$FORCE" -ne 1 ]]; then
  die "目录已存在: ${MODULE_DIR}（如需覆盖请使用 --force）"
fi
if [[ -e "$PROTO_FILE" && "$NO_PROTO" -ne 1 && "$FORCE" -ne 1 ]]; then
  die "协议文件已存在: ${PROTO_FILE}（如需覆盖请使用 --force 或 --no-proto）"
fi

mkdir -p "$MODULE_INCLUDE_DIR" "$MODULE_CMAKE_DIR" "$MODULE_DOC_DIR"

cat > "${MODULE_CMAKE_DIR}/LibInfo.cmake" <<EOF
set(LIB_NAME ${LIB_NAME})
set(VERSION_MAJOR 0)
set(VERSION_MINOR 0)
set(VERSION_PATCH 1)
set(VERSION "\${VERSION_MAJOR}.\${VERSION_MINOR}.\${VERSION_PATCH}")
EOF

cat > "${MODULE_DOC_DIR}/README.md" <<EOF
# ${MODULE_NAME} 模块

## 简介
TODO：一句话说明模块职责/边界。

## 能力清单
- TODO

## 接口与协议
- Protobuf：\`protobuf/${MODULE_NAME}.proto\`
- gRPC Service：\`${MODULE_NAME}Proto::${MODULE_NAME}Service\`

## 运行与地址
- 对外 gRPC：随机选择 \`0.0.0.0:<port>\`（7001–7999）
- 内部 gRPC：\`unix socket\`：\`./socket/${LIB_NAME}.sock\`
- 运行时可通过管理器 \`GetRunningModuleInfo\` 查询实际地址

## 配置与数据
- TODO：运行时配置项、文件位置、持久化数据目录等

## 构建产物
- 共享库：\`package/module/lib${LIB_NAME}.so.<version>\`（版本见 \`src/${MODULE_NAME}/cmake/LibInfo.cmake\`）
EOF

cat > "${MODULE_DIR}/CMakeLists.txt" <<EOF
include(cmake/LibInfo.cmake)

configure_file(
    \${CMAKE_SOURCE_DIR}/cmake/LibInfo.h.in
    \${CMAKE_CURRENT_SOURCE_DIR}/include/\${LIB_NAME}LibInfo.h
)

aux_source_directory(\${CMAKE_CURRENT_SOURCE_DIR} MODULE_SRCS)
add_library(\${LIB_NAME} SHARED \${MODULE_SRCS})

target_include_directories(\${LIB_NAME} PUBLIC \${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(\${LIB_NAME} PUBLIC
    moduleManager
)
set_target_properties(\${LIB_NAME} PROPERTIES
    VERSION \${VERSION}
    SOVERSION \${VERSION_MAJOR}
)
EOF

cat > "${MODULE_INCLUDE_DIR}/${MODULE_NAME}.h" <<EOF
#pragma once

#include <memory>
#include <stop_token>

#include "ModuleInterface.h"

namespace ${MODULE_NAME} {
class ${MODULE_NAME}GrpcServiceImpl;
class ${MODULE_NAME} : public ModuleInterface::ModuleInterface {
public:
  explicit ${MODULE_NAME}();
  ~${MODULE_NAME}() override;

  void start(std::stop_token stopToken) override;

private:
  std::shared_ptr<${MODULE_NAME}GrpcServiceImpl> ${VAR_PREFIX}Service_;
};
}  // namespace ${MODULE_NAME}
EOF

cat > "${MODULE_INCLUDE_DIR}/${MODULE_NAME}GrpcService.h" <<EOF
#pragma once

#include <grpcpp/impl/service_type.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include "${MODULE_NAME}.grpc.pb.h"
#include "${MODULE_NAME}.h"
#include "${MODULE_NAME}.pb.h"

namespace ${MODULE_NAME} {
class ${MODULE_NAME}GrpcServiceImpl : public ${MODULE_NAME}Proto::${MODULE_NAME}Service::Service {
public:
  void get${MODULE_NAME}(${MODULE_NAME}* module);
  grpc::Status Ping(grpc::ServerContext* context, const ${MODULE_NAME}Proto::Empty*, ${MODULE_NAME}Proto::Empty*) override;

private:
  ${MODULE_NAME}* module_;
};
}  // namespace ${MODULE_NAME}
EOF

cat > "${MODULE_DIR}/${MODULE_NAME}GrpcService.cc" <<EOF
#include "${MODULE_NAME}GrpcService.h"

#include <grpcpp/support/status.h>

namespace ${MODULE_NAME} {
void ${MODULE_NAME}GrpcServiceImpl::get${MODULE_NAME}(${MODULE_NAME}* module) {
  module_ = module;
}
grpc::Status ${MODULE_NAME}GrpcServiceImpl::Ping(grpc::ServerContext* context, const ${MODULE_NAME}Proto::Empty*, ${MODULE_NAME}Proto::Empty*) {
  return grpc::Status::OK;
}
}  // namespace ${MODULE_NAME}
EOF

cat > "${MODULE_DIR}/${MODULE_NAME}.cc" <<EOF
#include "${MODULE_NAME}.h"

#include <boost/dll.hpp>
#include <chrono>
#include <memory>
#include <stop_token>
#include <thread>

#include "${MODULE_NAME}GrpcService.h"
#include "${LIB_NAME}LibInfo.h"

namespace ${MODULE_NAME} {
${MODULE_NAME}::${MODULE_NAME}() :
  ModuleInterface(),
  ${VAR_PREFIX}Service_(std::make_shared<${MODULE_NAME}GrpcServiceImpl>()) {
  initLibInfo(${LIB_NAME}LibInfo);
}
${MODULE_NAME}::~${MODULE_NAME}() {}
void ${MODULE_NAME}::start(std::stop_token stopToken) {
  ${VAR_PREFIX}Service_->get${MODULE_NAME}(this);
  grpcServerBuilder(${VAR_PREFIX}Service_);
  while (!stopToken.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}
}  // namespace ${MODULE_NAME}

extern "C" BOOST_SYMBOL_EXPORT ModuleInterface::ModuleInterface* create() {
  return new ${MODULE_NAME}::${MODULE_NAME}();
}
EOF

if [[ "$NO_PROTO" -ne 1 ]]; then
  cat > "$PROTO_FILE" <<EOF
syntax = "proto3";

package ${MODULE_NAME}Proto;

message Empty {}

service ${MODULE_NAME}Service {
  rpc Ping(Empty) returns (Empty);
}
EOF
fi

SRC_CMAKELISTS="${SRC_DIR}/CMakeLists.txt"
if ! grep -qF "add_subdirectory(${MODULE_NAME})" "$SRC_CMAKELISTS"; then
  tmp="$(mktemp)"
  awk -v mod="${MODULE_NAME}" '
    BEGIN { inserted = 0 }
    /^add_executable\(/ && inserted == 0 {
      print "add_subdirectory(" mod ")"
      inserted = 1
    }
    { print }
    END {
      if (inserted == 0) {
        print "add_subdirectory(" mod ")"
      }
    }
  ' "$SRC_CMAKELISTS" > "$tmp"
  mv "$tmp" "$SRC_CMAKELISTS"
fi

echo "已生成模块骨架:"
echo "  - ModuleName: ${MODULE_NAME}"
echo "  - LIB_NAME: ${LIB_NAME}"
echo "  - ${MODULE_DIR}"
[[ "$NO_PROTO" -ne 1 ]] && echo "  - ${PROTO_FILE}"
echo
echo "下一步建议:"
echo "  1) cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=\$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
echo "  2) cmake --build build --target ${LIB_NAME} --parallel"
echo "  3) cd package && ./MskDSP"
