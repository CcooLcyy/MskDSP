if(NOT DEFINED MSKDSP_PACKAGE_DIR)
  message(FATAL_ERROR "未指定打包目录 MSKDSP_PACKAGE_DIR")
endif()

set(package_dir "${MSKDSP_PACKAGE_DIR}")
set(module_dir "${MSKDSP_MODULE_DIR}")
set(lib_dir "${MSKDSP_LIB_DIR}")
set(vcpkg_installed_dir "${MSKDSP_VCPKG_INSTALLED_DIR}")
set(vcpkg_triplet "${MSKDSP_VCPKG_TRIPLET}")

if(NOT vcpkg_installed_dir OR NOT vcpkg_triplet)
  message(WARNING "未设置 vcpkg 安装目录或 triplet，跳过动态库整理")
  return()
endif()

set(vcpkg_lib_dir "${vcpkg_installed_dir}/${vcpkg_triplet}/lib")
if(NOT EXISTS "${vcpkg_lib_dir}")
  message(WARNING "未找到 vcpkg 动态库目录: ${vcpkg_lib_dir}")
  return()
endif()
file(REAL_PATH "${vcpkg_lib_dir}" vcpkg_lib_dir_real)
message(STATUS "vcpkg 动态库目录真实路径: ${vcpkg_lib_dir_real}")

file(MAKE_DIRECTORY "${lib_dir}")

set(executables)
if(EXISTS "${package_dir}/MskDSP")
  list(APPEND executables "${package_dir}/MskDSP")
endif()

file(GLOB module_libs LIST_DIRECTORIES false "${module_dir}/*.so*")
file(GLOB core_libs LIST_DIRECTORIES false
  "${lib_dir}/libdspProto.so*"
  "${lib_dir}/libmoduleManager.so*"
)
set(libraries ${module_libs} ${core_libs})

if(NOT executables AND NOT libraries)
  message(STATUS "未找到可分析的二进制，跳过动态库整理")
  return()
endif()

if(MSKDSP_OBJDUMP)
  if(EXISTS "${MSKDSP_OBJDUMP}")
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${MSKDSP_OBJDUMP}")
  else()
    message(WARNING "未找到 objdump: ${MSKDSP_OBJDUMP}，跳过动态库整理")
    return()
  endif()
endif()
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "linux+elf")

file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES ${executables}
  LIBRARIES ${libraries}
  RESOLVED_DEPENDENCIES_VAR resolved_deps
  UNRESOLVED_DEPENDENCIES_VAR unresolved_deps
  DIRECTORIES "${vcpkg_lib_dir}" "${lib_dir}" "${module_dir}"
  POST_EXCLUDE_REGEXES "^/lib/" "^/usr/lib/" "^/usr/local/lib/" "^${lib_dir}/" "^${module_dir}/"
)
list(LENGTH resolved_deps resolved_count)
message(STATUS "运行依赖解析数量: ${resolved_count}")

if(unresolved_deps)
  message(WARNING "运行依赖解析存在未解析项: ${unresolved_deps}")
endif()

set(vcpkg_deps)
foreach(dep IN LISTS resolved_deps)
  set(dep_real "")
  if(EXISTS "${dep}")
    file(REAL_PATH "${dep}" dep_real)
  endif()
  if(dep MATCHES "^${vcpkg_lib_dir}/")
    list(APPEND vcpkg_deps "${dep}")
    if(dep_real AND NOT dep_real STREQUAL dep)
      list(APPEND vcpkg_deps "${dep_real}")
    endif()
  elseif(dep_real AND dep_real MATCHES "^${vcpkg_lib_dir_real}/")
    list(APPEND vcpkg_deps "${dep_real}")
    if(NOT dep_real STREQUAL dep)
      list(APPEND vcpkg_deps "${dep}")
    endif()
  endif()
endforeach()
list(REMOVE_DUPLICATES vcpkg_deps)
list(LENGTH vcpkg_deps vcpkg_dep_count)
message(STATUS "解析到 vcpkg 动态库数量: ${vcpkg_dep_count}")

if(NOT vcpkg_deps)
  message(WARNING "未解析到 vcpkg 动态库依赖，跳过复制与清理")
  return()
endif()

set(keep_names)
foreach(dep IN LISTS vcpkg_deps)
  get_filename_component(dep_name "${dep}" NAME)
  list(APPEND keep_names "${dep_name}")
endforeach()
list(REMOVE_DUPLICATES keep_names)

set(copied 0)
foreach(dep IN LISTS vcpkg_deps)
  if(NOT EXISTS "${dep}")
    continue()
  endif()
  file(COPY "${dep}" DESTINATION "${lib_dir}")
  math(EXPR copied "${copied} + 1")
endforeach()
message(STATUS "已复制 vcpkg 动态库数量: ${copied}")

file(GLOB existing_libs LIST_DIRECTORIES false "${lib_dir}/*.so*")
set(removed 0)
foreach(lib IN LISTS existing_libs)
  get_filename_component(base "${lib}" NAME)
  if(base MATCHES "^libdspProto\\.so" OR base MATCHES "^libmoduleManager\\.so")
    continue()
  endif()
  list(FIND keep_names "${base}" keep_index)
  if(keep_index EQUAL -1)
    file(REMOVE "${lib}")
    math(EXPR removed "${removed} + 1")
  endif()
endforeach()
if(removed GREATER 0)
  message(STATUS "已清理未使用的动态库数量: ${removed}")
endif()
