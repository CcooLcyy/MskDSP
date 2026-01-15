cmake_minimum_required(VERSION 3.28)

project(MskDSP)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(gRPC CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS asio dll log log_setup)

set(packPath ${CMAKE_SOURCE_DIR}/package)
set(libPath ${packPath}/lib)
set(configPath ${packPath}/conf)
file(MAKE_DIRECTORY ${packPath} ${libPath} ${configPath})

option(MSKDSP_USE_CCACHE "Use ccache to speed up builds (if available)" ON)
if(MSKDSP_USE_CCACHE)
  find_program(CCACHE_PROGRAM ccache)
  if(CCACHE_PROGRAM)
    # Some environments (e.g. sandboxed builds) may not be able to write to the
    # default ccache directory (usually under $HOME/.cache). When CCACHE_DIR is
    # not explicitly set, use a build-local directory to avoid permission issues.
    if(DEFINED ENV{CCACHE_DIR} AND NOT "$ENV{CCACHE_DIR}" STREQUAL "")
      set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
      set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
    else()
      set(MSKDSP_CCACHE_DIR "${CMAKE_BINARY_DIR}/.ccache")
      file(MAKE_DIRECTORY "${MSKDSP_CCACHE_DIR}")
      set(CMAKE_C_COMPILER_LAUNCHER
          "${CMAKE_COMMAND};-E;env;CCACHE_DIR=${MSKDSP_CCACHE_DIR};${CCACHE_PROGRAM}")
      set(CMAKE_CXX_COMPILER_LAUNCHER
          "${CMAKE_COMMAND};-E;env;CCACHE_DIR=${MSKDSP_CCACHE_DIR};${CCACHE_PROGRAM}")
    endif()
  endif()
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${packPath})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${libPath})

add_subdirectory(3rdlibs)

set(MSKDSP_BUILD_TESTS_DEFAULT OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(MSKDSP_BUILD_TESTS_DEFAULT ON)
endif()
option(MSKDSP_BUILD_TESTS "Build tests (enables coverage & auto report)" ${MSKDSP_BUILD_TESTS_DEFAULT})

if(MSKDSP_BUILD_TESTS)
  include(${CMAKE_SOURCE_DIR}/cmake/Coverage.cmake)
  mskdsp_enable_coverage_flags()
endif()

add_subdirectory(protobuf)
add_subdirectory(src)

if(MSKDSP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()
