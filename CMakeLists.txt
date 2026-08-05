cmake_minimum_required(VERSION 3.28)

project(MskDSP)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)

find_package(gRPC CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS asio dll iostreams json log log_setup)
find_package(ZLIB REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

set(MSKDSP_BUILD_TESTS_DEFAULT OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(MSKDSP_BUILD_TESTS_DEFAULT ON)
endif()
option(MSKDSP_BUILD_TESTS "构建测试用例" ${MSKDSP_BUILD_TESTS_DEFAULT})
option(MSKDSP_ENABLE_COVERAGE "启用覆盖率编译选项与 coverage 目标" OFF)
option(MSKDSP_STRIP_DEBUG "Strip binaries and keep separate debug info under package/debug." OFF)
option(MSKDSP_ENABLE_SANITIZERS "开启 ASAN/UBSAN 运行时诊断" OFF)
option(MSKDSP_ENABLE_TSAN "开启 TSAN 运行时诊断" OFF)
option(MSKDSP_BUILD_TOOLS "构建集成测试辅助工具" OFF)

if(MSKDSP_ENABLE_COVERAGE AND NOT MSKDSP_BUILD_TESTS)
  message(FATAL_ERROR "启用 MSKDSP_ENABLE_COVERAGE 时必须同时开启 MSKDSP_BUILD_TESTS")
endif()

if(MSKDSP_ENABLE_SANITIZERS AND MSKDSP_ENABLE_TSAN)
  message(FATAL_ERROR "ASAN/UBSAN 与 TSAN 不能同时开启")
endif()

if(MSKDSP_ENABLE_SANITIZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "开启 ASAN/UBSAN 需要 GCC/Clang，当前编译器为 ${CMAKE_CXX_COMPILER_ID}")
  endif()
  add_compile_options(-fno-omit-frame-pointer -fsanitize=address,undefined)
  add_link_options(-fsanitize=address,undefined)
  message(STATUS "已开启 ASAN/UBSAN 编译选项")
endif()

if(MSKDSP_ENABLE_TSAN)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "开启 TSAN 需要 GCC/Clang，当前编译器为 ${CMAKE_CXX_COMPILER_ID}")
  endif()
  add_compile_options(-fno-omit-frame-pointer -fsanitize=thread)
  add_link_options(-fsanitize=thread)
  message(STATUS "已开启 TSAN 编译选项")
endif()

set(MSKDSP_PACKAGE_DIR "${CMAKE_SOURCE_DIR}/package" CACHE PATH "MskDSP package output directory")
set(_mskdsp_pkg_probe "${MSKDSP_PACKAGE_DIR}/lib/libdspProto.so")
if(EXISTS "${_mskdsp_pkg_probe}")
  find_program(MSKDSP_FILE_TOOL file)
  if(MSKDSP_FILE_TOOL)
    execute_process(
      COMMAND "${MSKDSP_FILE_TOOL}" -b "${_mskdsp_pkg_probe}"
      OUTPUT_VARIABLE _mskdsp_pkg_file_info
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
    string(TOLOWER "${_mskdsp_pkg_file_info}" _mskdsp_pkg_file_lower)
    set(_mskdsp_pkg_mismatch OFF)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$" AND _mskdsp_pkg_file_lower MATCHES "aarch64|arm64")
      set(_mskdsp_pkg_mismatch ON)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$" AND _mskdsp_pkg_file_lower MATCHES "x86-64")
      set(_mskdsp_pkg_mismatch ON)
    endif()
    if(_mskdsp_pkg_mismatch)
      set(_mskdsp_pkg_bins "")
      if(EXISTS "${MSKDSP_PACKAGE_DIR}/MskDSP")
        list(APPEND _mskdsp_pkg_bins "${MSKDSP_PACKAGE_DIR}/MskDSP")
      endif()
      file(GLOB _mskdsp_pkg_tests "${MSKDSP_PACKAGE_DIR}/*_test")
      list(APPEND _mskdsp_pkg_bins ${_mskdsp_pkg_tests})
      if(EXISTS "${MSKDSP_PACKAGE_DIR}/lib")
        file(GLOB _mskdsp_pkg_libs "${MSKDSP_PACKAGE_DIR}/lib/*")
        list(APPEND _mskdsp_pkg_bins ${_mskdsp_pkg_libs})
      endif()
      if(EXISTS "${MSKDSP_PACKAGE_DIR}/module")
        file(GLOB _mskdsp_pkg_modules "${MSKDSP_PACKAGE_DIR}/module/*")
        list(APPEND _mskdsp_pkg_bins ${_mskdsp_pkg_modules})
      endif()
      if(_mskdsp_pkg_bins)
        file(REMOVE ${_mskdsp_pkg_bins})
      endif()
      message(WARNING
              "检测到 package/lib 架构与当前编译器不一致，已清理 package 编译产物(保留 conf/log)，继续输出到: ${MSKDSP_PACKAGE_DIR}")
    endif()
  endif()
endif()

set(packPath ${MSKDSP_PACKAGE_DIR})
set(libPath ${packPath}/lib)
set(modulePath ${packPath}/module)
set(configPath ${packPath}/conf)
file(MAKE_DIRECTORY ${packPath} ${libPath} ${modulePath} ${configPath})

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${packPath})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${modulePath})

set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
set(CMAKE_BUILD_RPATH "$ORIGIN;$ORIGIN/lib;$ORIGIN/../lib")
set(CMAKE_INSTALL_RPATH "$ORIGIN;$ORIGIN/lib;$ORIGIN/../lib")
set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH OFF)

add_subdirectory(3rdlibs)

if(MSKDSP_ENABLE_COVERAGE)
  include(${CMAKE_SOURCE_DIR}/cmake/Coverage.cmake)
  mskdsp_enable_coverage_flags()
endif()

set(MSKDSP_COMMON_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/src/common/include")

add_subdirectory(protobuf)
add_subdirectory(src)
if(MSKDSP_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

set(MSKDSP_MODULE_TARGETS
  ConfigPusher
  DataCenter
  IEC61850
  IEC104
  DLT645
  ModbusRTU
  MQTTManager
  AGC
  AVC
  Calc
)

set(MSKDSP_VCPKG_INSTALL_DIR "")
if(DEFINED VCPKG_INSTALLED_DIR AND NOT VCPKG_INSTALLED_DIR STREQUAL "")
  set(MSKDSP_VCPKG_INSTALL_DIR "${VCPKG_INSTALLED_DIR}")
else()
  set(MSKDSP_VCPKG_INSTALL_DIR "${CMAKE_BINARY_DIR}/vcpkg_installed")
endif()
if(NOT EXISTS "${MSKDSP_VCPKG_INSTALL_DIR}")
  message(FATAL_ERROR "未找到 vcpkg_installed 目录: ${MSKDSP_VCPKG_INSTALL_DIR}")
endif()
if(NOT VCPKG_TARGET_TRIPLET)
  message(FATAL_ERROR "未设置 VCPKG_TARGET_TRIPLET，无法收集 vcpkg 运行时依赖")
endif()
set(MSKDSP_VCPKG_TRIPLET_DIR "${MSKDSP_VCPKG_INSTALL_DIR}/${VCPKG_TARGET_TRIPLET}")
if(NOT EXISTS "${MSKDSP_VCPKG_TRIPLET_DIR}")
  message(FATAL_ERROR "未找到 vcpkg 目标目录: ${MSKDSP_VCPKG_TRIPLET_DIR}")
endif()
set(MSKDSP_VCPKG_RUNTIME_DIRS "")
set(MSKDSP_VCPKG_LIB_SUBDIR "lib")
if(CMAKE_BUILD_TYPE STREQUAL "Debug"
   AND EXISTS "${MSKDSP_VCPKG_TRIPLET_DIR}/debug/lib")
  set(MSKDSP_VCPKG_LIB_SUBDIR "debug/lib")
endif()
set(MSKDSP_VCPKG_LIB_DIR "${MSKDSP_VCPKG_TRIPLET_DIR}/${MSKDSP_VCPKG_LIB_SUBDIR}")
if(EXISTS "${MSKDSP_VCPKG_LIB_DIR}")
  list(APPEND MSKDSP_VCPKG_RUNTIME_DIRS "${MSKDSP_VCPKG_LIB_DIR}")
endif()
set(MSKDSP_VCPKG_BIN_SUBDIR "bin")
if(MSKDSP_VCPKG_LIB_SUBDIR STREQUAL "debug/lib"
   AND EXISTS "${MSKDSP_VCPKG_TRIPLET_DIR}/debug/bin")
  set(MSKDSP_VCPKG_BIN_SUBDIR "debug/bin")
endif()
set(MSKDSP_VCPKG_BIN_DIR "${MSKDSP_VCPKG_TRIPLET_DIR}/${MSKDSP_VCPKG_BIN_SUBDIR}")
if(EXISTS "${MSKDSP_VCPKG_BIN_DIR}")
  list(APPEND MSKDSP_VCPKG_RUNTIME_DIRS "${MSKDSP_VCPKG_BIN_DIR}")
endif()
if(MSKDSP_VCPKG_RUNTIME_DIRS STREQUAL "")
  message(FATAL_ERROR "未找到 vcpkg 运行时库目录: ${MSKDSP_VCPKG_TRIPLET_DIR}")
endif()

string(REGEX REPLACE "([][+.*^$(){}|\\\\])" "\\\\\\1"
       MSKDSP_VCPKG_TRIPLET_DIR_REGEX "${MSKDSP_VCPKG_TRIPLET_DIR}")
message(STATUS "安装阶段仅收集 vcpkg 运行时依赖: ${MSKDSP_VCPKG_TRIPLET_DIR}")

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "${MSKDSP_PACKAGE_DIR}" CACHE PATH "MskDSP 安装输出目录" FORCE)
endif()

set(MSKDSP_CONFIG_TEMPLATE_DIR "${CMAKE_SOURCE_DIR}/package/conf" CACHE PATH
    "MskDSP 运行配置模板目录")

install(TARGETS ${PROJECT_NAME}
  RUNTIME DESTINATION .
)

install(TARGETS moduleManager dspProto
  LIBRARY DESTINATION lib
)

install(TARGETS ${MSKDSP_MODULE_TARGETS}
  LIBRARY DESTINATION module
)

# 默认输出目录本身就是仓库内的 package/conf；其他交付目录必须显式带上
# ModuleManager 和 ConfigPusher 的模板，避免 ARM64 包只有二进制而无法启动。
if(EXISTS "${MSKDSP_CONFIG_TEMPLATE_DIR}"
   AND NOT CMAKE_INSTALL_PREFIX STREQUAL "${CMAKE_SOURCE_DIR}/package")
  install(DIRECTORY "${MSKDSP_CONFIG_TEMPLATE_DIR}/"
          DESTINATION conf
          FILES_MATCHING
          PATTERN "*.jsonc")
endif()

set(MSKDSP_INSTALL_EXEC_PATHS "$<TARGET_FILE:${PROJECT_NAME}>")
set(MSKDSP_INSTALL_LIB_PATHS "")
list(APPEND MSKDSP_INSTALL_LIB_PATHS "$<TARGET_FILE:moduleManager>")
list(APPEND MSKDSP_INSTALL_LIB_PATHS "$<TARGET_FILE:dspProto>")
foreach(MSKDSP_INSTALL_TARGET IN LISTS MSKDSP_MODULE_TARGETS)
  list(APPEND MSKDSP_INSTALL_LIB_PATHS "$<TARGET_FILE:${MSKDSP_INSTALL_TARGET}>")
endforeach()

set(MSKDSP_INSTALL_SCRIPT
    "${CMAKE_CURRENT_BINARY_DIR}/mskdsp_install_runtime_deps.cmake")
set(MSKDSP_INSTALL_SCRIPT_CONTENT [=[
message(STATUS "开始收集 vcpkg 运行时依赖到: ${CMAKE_INSTALL_PREFIX}/lib")
set(_mskdsp_execs "@MSKDSP_INSTALL_EXEC_PATHS@")
set(_mskdsp_libs "@MSKDSP_INSTALL_LIB_PATHS@")
set(_mskdsp_search_dirs "@MSKDSP_VCPKG_RUNTIME_DIRS@")
set(_mskdsp_allow_regex "^@MSKDSP_VCPKG_TRIPLET_DIR_REGEX@/")
# 安装脚本独立运行时不会继承交叉编译配置，必须显式指定目标ELF解析器。
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "linux+elf")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "objdump")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "@CMAKE_OBJDUMP@")
file(MAKE_DIRECTORY "${CMAKE_INSTALL_PREFIX}/lib")
file(GET_RUNTIME_DEPENDENCIES
  RESOLVED_DEPENDENCIES_VAR _mskdsp_deps
  UNRESOLVED_DEPENDENCIES_VAR _mskdsp_unresolved
  EXECUTABLES ${_mskdsp_execs}
  LIBRARIES ${_mskdsp_libs}
  DIRECTORIES ${_mskdsp_search_dirs}
  POST_INCLUDE_REGEXES "${_mskdsp_allow_regex}"
  POST_EXCLUDE_REGEXES ".*"
)
if(_mskdsp_unresolved)
  message(STATUS "以下依赖未解析，将被忽略: ${_mskdsp_unresolved}")
endif()
list(REMOVE_DUPLICATES _mskdsp_deps)
foreach(_dep IN LISTS _mskdsp_deps)
  message(STATUS "复制依赖库: ${_dep}")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                  "${_dep}" "${CMAKE_INSTALL_PREFIX}/lib")
endforeach()
message(STATUS "开始收集 C++ 运行时库到: ${CMAKE_INSTALL_PREFIX}/lib")
set(_mskdsp_runtime_libs "libstdc++.so.6" "libgcc_s.so.1" "libatomic.so.1")
set(_mskdsp_runtime_paths "")
foreach(_lib IN LISTS _mskdsp_runtime_libs)
  execute_process(
    COMMAND "@CMAKE_CXX_COMPILER@" "-print-file-name=${_lib}"
    OUTPUT_VARIABLE _mskdsp_runtime_path
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(_mskdsp_runtime_path STREQUAL "" OR _mskdsp_runtime_path STREQUAL "${_lib}")
    message(STATUS "未找到运行时库: ${_lib}")
  elseif(EXISTS "${_mskdsp_runtime_path}")
    list(APPEND _mskdsp_runtime_paths "${_mskdsp_runtime_path}")
  else()
    message(STATUS "运行时库路径不存在: ${_mskdsp_runtime_path}")
  endif()
endforeach()
list(REMOVE_DUPLICATES _mskdsp_runtime_paths)
foreach(_dep IN LISTS _mskdsp_runtime_paths)
  message(STATUS "复制运行时库: ${_dep}")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                  "${_dep}" "${CMAKE_INSTALL_PREFIX}/lib")
endforeach()
]=])
string(CONFIGURE "${MSKDSP_INSTALL_SCRIPT_CONTENT}"
               MSKDSP_INSTALL_SCRIPT_CONFIGURED_CONTENT @ONLY)
# CONTENT模式会在生成安装脚本时展开$<TARGET_FILE:...>，避免安装阶段把生成器表达式当成字面量。
file(GENERATE OUTPUT "${MSKDSP_INSTALL_SCRIPT}"
     CONTENT "${MSKDSP_INSTALL_SCRIPT_CONFIGURED_CONTENT}")
install(SCRIPT "${MSKDSP_INSTALL_SCRIPT}")

if(MSKDSP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()

if(MSKDSP_STRIP_DEBUG)
  set(MSKDSP_DEBUG_DIR "${packPath}/debug")
  set(MSKDSP_STRIP_SCRIPT "${CMAKE_SOURCE_DIR}/script/strip_debug.sh")
  set(MSKDSP_STRIP_TOOL "${CMAKE_STRIP}")
  set(MSKDSP_OBJCOPY_TOOL "${CMAKE_OBJCOPY}")

  if(NOT MSKDSP_STRIP_TOOL OR NOT MSKDSP_OBJCOPY_TOOL)
    get_filename_component(MSKDSP_CC_NAME "${CMAKE_C_COMPILER}" NAME)
    if(MSKDSP_CC_NAME MATCHES "^(.*)(gcc|g\\+\\+|cc|clang|clang\\+\\+)$")
      set(MSKDSP_TOOLCHAIN_PREFIX "${CMAKE_MATCH_1}")
    endif()
    if(NOT MSKDSP_STRIP_TOOL)
      if(MSKDSP_TOOLCHAIN_PREFIX)
        find_program(MSKDSP_STRIP_TOOL NAMES "${MSKDSP_TOOLCHAIN_PREFIX}strip" strip)
      else()
        find_program(MSKDSP_STRIP_TOOL NAMES strip)
      endif()
    endif()
    if(NOT MSKDSP_OBJCOPY_TOOL)
      if(MSKDSP_TOOLCHAIN_PREFIX)
        find_program(MSKDSP_OBJCOPY_TOOL NAMES "${MSKDSP_TOOLCHAIN_PREFIX}objcopy" objcopy)
      else()
        find_program(MSKDSP_OBJCOPY_TOOL NAMES objcopy)
      endif()
    endif()
  endif()

  if(NOT MSKDSP_STRIP_TOOL OR NOT MSKDSP_OBJCOPY_TOOL)
    message(FATAL_ERROR
            "MSKDSP_STRIP_DEBUG requires strip/objcopy tools. Set CMAKE_STRIP/CMAKE_OBJCOPY or ensure binutils is in PATH.")
  endif()

  function(mskdsp_collect_targets out_var dir)
    get_property(local_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    set(all_targets ${local_targets})
    get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(subdir IN LISTS subdirs)
      mskdsp_collect_targets(sub_targets "${subdir}")
      list(APPEND all_targets ${sub_targets})
    endforeach()
    set(${out_var} "${all_targets}" PARENT_SCOPE)
  endfunction()

  mskdsp_collect_targets(MSKDSP_ALL_TARGETS "${CMAKE_CURRENT_BINARY_DIR}")
  set(MSKDSP_STRIP_TARGETS "")
  foreach(MSKDSP_TARGET IN LISTS MSKDSP_ALL_TARGETS)
    if(NOT TARGET "${MSKDSP_TARGET}")
      continue()
    endif()
    get_target_property(MSKDSP_TARGET_IMPORTED "${MSKDSP_TARGET}" IMPORTED)
    if(MSKDSP_TARGET_IMPORTED)
      continue()
    endif()
    get_target_property(MSKDSP_TARGET_TYPE "${MSKDSP_TARGET}" TYPE)
    if(NOT MSKDSP_TARGET_TYPE)
      continue()
    endif()
    if(MSKDSP_TARGET MATCHES ".*_test$")
      continue()
    endif()
    if(MSKDSP_TARGET_TYPE STREQUAL "EXECUTABLE"
       OR MSKDSP_TARGET_TYPE STREQUAL "SHARED_LIBRARY"
       OR MSKDSP_TARGET_TYPE STREQUAL "MODULE_LIBRARY")
      list(APPEND MSKDSP_STRIP_TARGETS "${MSKDSP_TARGET}")
    endif()
  endforeach()

  message(STATUS "MSKDSP strip-debug enabled; output: ${MSKDSP_DEBUG_DIR}")

  add_custom_target(mskdsp_strip_debug ALL
    COMMAND bash "${MSKDSP_STRIP_SCRIPT}" "${packPath}" "${MSKDSP_DEBUG_DIR}"
            "${MSKDSP_STRIP_TOOL}" "${MSKDSP_OBJCOPY_TOOL}"
    COMMENT "Stripping binaries and exporting debug symbols"
    VERBATIM
  )

  if(MSKDSP_STRIP_TARGETS)
    add_dependencies(mskdsp_strip_debug ${MSKDSP_STRIP_TARGETS})
  endif()
endif()
