cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED MSKDSP_TEST_NAME OR MSKDSP_TEST_NAME STREQUAL "")
  message(FATAL_ERROR "缺少测试名称")
endif()

if(NOT DEFINED MSKDSP_TEST_EXECUTABLE OR MSKDSP_TEST_EXECUTABLE STREQUAL "")
  message(FATAL_ERROR "缺少测试可执行文件路径")
endif()

if(NOT DEFINED MSKDSP_TEST_WORKDIR OR MSKDSP_TEST_WORKDIR STREQUAL "")
  message(FATAL_ERROR "缺少测试工作目录")
endif()

function(mskdsp_cleanup_runtime_artifacts base_dir remove_base_dir_if_empty)
  foreach(path IN ITEMS
      "${base_dir}/conf"
      "${base_dir}/log"
      "${base_dir}/socket")
    if(EXISTS "${path}")
      file(REMOVE_RECURSE "${path}")
    endif()
  endforeach()

  if(remove_base_dir_if_empty)
    file(GLOB remaining_entries LIST_DIRECTORIES true "${base_dir}/*")
    if(NOT remaining_entries)
      file(REMOVE_RECURSE "${base_dir}")
    endif()
  endif()
endfunction()

file(MAKE_DIRECTORY "${MSKDSP_TEST_WORKDIR}")
mskdsp_cleanup_runtime_artifacts("${MSKDSP_TEST_WORKDIR}" FALSE)
file(MAKE_DIRECTORY "${MSKDSP_TEST_WORKDIR}")

if(DEFINED MSKDSP_TEST_LIBRARY_PATH AND NOT MSKDSP_TEST_LIBRARY_PATH STREQUAL "")
  if(DEFINED ENV{LD_LIBRARY_PATH} AND NOT "$ENV{LD_LIBRARY_PATH}" STREQUAL "")
    set(ENV{LD_LIBRARY_PATH}
        "${MSKDSP_TEST_LIBRARY_PATH}:$ENV{LD_LIBRARY_PATH}")
  else()
    set(ENV{LD_LIBRARY_PATH} "${MSKDSP_TEST_LIBRARY_PATH}")
  endif()
endif()

execute_process(
  COMMAND "${MSKDSP_TEST_EXECUTABLE}"
  WORKING_DIRECTORY "${MSKDSP_TEST_WORKDIR}"
  RESULT_VARIABLE mskdsp_test_result
)

mskdsp_cleanup_runtime_artifacts("${MSKDSP_TEST_WORKDIR}" TRUE)

if(NOT mskdsp_test_result STREQUAL "0")
  message(FATAL_ERROR "测试执行失败: ${MSKDSP_TEST_NAME}，退出码=${mskdsp_test_result}")
endif()
