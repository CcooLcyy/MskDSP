cmake_minimum_required(VERSION 3.28)

project(MskDSP)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)

find_package(gRPC CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS asio dll iostreams log log_setup)
find_package(ZLIB REQUIRED)

set(packPath ${CMAKE_SOURCE_DIR}/package)
set(libPath ${packPath}/lib)
set(modulePath ${packPath}/module)
set(configPath ${packPath}/conf)
file(MAKE_DIRECTORY ${packPath} ${libPath} ${modulePath} ${configPath})

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${packPath})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${modulePath})

set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
set(CMAKE_BUILD_RPATH "$ORIGIN;$ORIGIN/lib;$ORIGIN/../lib")
set(CMAKE_INSTALL_RPATH "$ORIGIN;$ORIGIN/lib;$ORIGIN/../lib")

add_subdirectory(3rdlibs)

set(MSKDSP_BUILD_TESTS_DEFAULT OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(MSKDSP_BUILD_TESTS_DEFAULT ON)
endif()
option(MSKDSP_BUILD_TESTS "Build tests (enables coverage & auto report)" ${MSKDSP_BUILD_TESTS_DEFAULT})
option(MSKDSP_STRIP_DEBUG "Strip binaries and keep separate debug info under package/debug." OFF)

if(MSKDSP_BUILD_TESTS)
  include(${CMAKE_SOURCE_DIR}/cmake/Coverage.cmake)
  mskdsp_enable_coverage_flags()
endif()

add_subdirectory(protobuf)
add_subdirectory(src)

set(MSKDSP_MODULE_TARGETS
  ConfigPusher
  COMMock
  DataCenter
  IEC104
  DLT645
  ModbusRTU
  AGC
  AVC
)
add_custom_target(mskdsp_stage_runtime_deps ALL
  COMMAND ${CMAKE_COMMAND}
          -DMSKDSP_PACKAGE_DIR=${packPath}
          -DMSKDSP_MODULE_DIR=${modulePath}
          -DMSKDSP_LIB_DIR=${libPath}
          -DMSKDSP_VCPKG_INSTALLED_DIR=${VCPKG_INSTALLED_DIR}
          -DMSKDSP_VCPKG_TRIPLET=${VCPKG_TARGET_TRIPLET}
          -DMSKDSP_OBJDUMP=${CMAKE_OBJDUMP}
          -P ${CMAKE_SOURCE_DIR}/cmake/StageRuntimeDeps.cmake
  COMMENT "整理 vcpkg 动态库到 package/lib"
  DEPENDS MskDSP dspProto moduleManager ${MSKDSP_MODULE_TARGETS}
  VERBATIM
)

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
