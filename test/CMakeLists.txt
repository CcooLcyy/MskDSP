enable_testing()

find_package(GTest CONFIG REQUIRED)

add_executable(dataCenterCore_test
  data_center_core_test.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterCore.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterCoreConnection.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterCorePointTable.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterCoreRoutes.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterCorePublish.cc
)

target_include_directories(dataCenterCore_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/DataCenter/include
)

target_link_libraries(dataCenterCore_test PRIVATE
  GTest::gtest_main
  dspProto
)

add_test(NAME dataCenterCore_test COMMAND dataCenterCore_test)

add_executable(dataCenterPointTableStore_test
  data_center_point_table_store_test.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterPointTableStore.cc
)

target_include_directories(dataCenterPointTableStore_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/DataCenter/include
)

target_link_libraries(dataCenterPointTableStore_test PRIVATE
  GTest::gtest_main
  dspProto
)

add_test(NAME dataCenterPointTableStore_test COMMAND dataCenterPointTableStore_test)

add_executable(dataCenterRouteStore_test
  data_center_route_store_test.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterRouteStore.cc
)

target_include_directories(dataCenterRouteStore_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/DataCenter/include
)

target_link_libraries(dataCenterRouteStore_test PRIVATE
  GTest::gtest_main
  dspProto
)

add_test(NAME dataCenterRouteStore_test COMMAND dataCenterRouteStore_test)

add_executable(dataCenterConnectionStore_test
  data_center_connection_store_test.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterConnectionStore.cc
)

target_include_directories(dataCenterConnectionStore_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/DataCenter/include
)

target_link_libraries(dataCenterConnectionStore_test PRIVATE
  GTest::gtest_main
  dspProto
)

add_test(NAME dataCenterConnectionStore_test COMMAND dataCenterConnectionStore_test)

add_executable(iec104PointTable_test
  iec104_point_table_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC104/IEC104PointTable.cc
)

target_include_directories(iec104PointTable_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC104/include
)

target_link_libraries(iec104PointTable_test PRIVATE
  GTest::gtest_main
  dspProto
)

add_test(NAME iec104PointTable_test COMMAND iec104PointTable_test)

add_executable(iec104LinkManager_test
  iec104_link_manager_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC104/IEC104DataCenterClient.cc
  ${CMAKE_SOURCE_DIR}/src/IEC104/IEC104LinkManager.cc
  ${CMAKE_SOURCE_DIR}/src/IEC104/IEC104LinkManagerDataCenter.cc
  ${CMAKE_SOURCE_DIR}/src/IEC104/IEC104PointTable.cc
  ${CMAKE_SOURCE_DIR}/src/IEC104/IEC104TcpLink.cc
  ${CMAKE_SOURCE_DIR}/src/IEC104/IEC104TcpSession.cc
)

target_include_directories(iec104LinkManager_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC104/include
)

target_link_libraries(iec104LinkManager_test PRIVATE
  GTest::gtest_main
  GTest::gmock
  dspProto
  Boost::asio
)

add_test(NAME iec104LinkManager_test COMMAND iec104LinkManager_test)

set(MSKDSP_TEST_TARGETS
  dataCenterCore_test
  dataCenterPointTableStore_test
  dataCenterRouteStore_test
  dataCenterConnectionStore_test
  iec104PointTable_test
  iec104LinkManager_test
)

# Allow overriding via -DGCOVR_EXECUTABLE=/path/to/gcovr (cache variable).
# If an older configure cached an empty value, clear it so installing gcovr
# later doesn't require wiping the build directory.
if(DEFINED GCOVR_EXECUTABLE AND
   (GCOVR_EXECUTABLE STREQUAL "" OR GCOVR_EXECUTABLE MATCHES "-NOTFOUND$"))
  unset(GCOVR_EXECUTABLE CACHE)
endif()

find_program(GCOVR_EXECUTABLE NAMES gcovr DOC "Path to gcovr")
if(NOT GCOVR_EXECUTABLE)
  message(FATAL_ERROR "gcovr not found: install it or set -DGCOVR_EXECUTABLE=/path/to/gcovr")
endif()
if(NOT EXISTS "${GCOVR_EXECUTABLE}")
  message(FATAL_ERROR "GCOVR_EXECUTABLE does not exist: ${GCOVR_EXECUTABLE}")
endif()

if(DEFINED GCOV_EXECUTABLE AND
   (GCOV_EXECUTABLE STREQUAL "" OR GCOV_EXECUTABLE MATCHES "-NOTFOUND$"))
  unset(GCOV_EXECUTABLE CACHE)
endif()
set(MSKDSP_GCOV_NAMES gcov)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  string(REGEX MATCH "^[0-9]+" MSKDSP_GCC_MAJOR "${CMAKE_CXX_COMPILER_VERSION}")
  if(MSKDSP_GCC_MAJOR)
    list(PREPEND MSKDSP_GCOV_NAMES "gcov-${MSKDSP_GCC_MAJOR}")
  endif()
endif()
find_program(GCOV_EXECUTABLE NAMES ${MSKDSP_GCOV_NAMES} DOC "Path to gcov")
if(NOT GCOV_EXECUTABLE)
  message(FATAL_ERROR "gcov not found: install it or set -DGCOV_EXECUTABLE=/path/to/gcov")
endif()
if(NOT EXISTS "${GCOV_EXECUTABLE}")
  message(FATAL_ERROR "GCOV_EXECUTABLE does not exist: ${GCOV_EXECUTABLE}")
endif()

set(MSKDSP_COVERAGE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/coverage")
add_custom_target(coverage ALL
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${MSKDSP_COVERAGE_OUTPUT_DIR}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${MSKDSP_COVERAGE_OUTPUT_DIR}"
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
  COMMAND "${GCOVR_EXECUTABLE}"
    -r "${CMAKE_SOURCE_DIR}"
    --object-directory "${CMAKE_BINARY_DIR}"
    --gcov-executable "${GCOV_EXECUTABLE}"
    --filter "(^|.*/)src/.*[.](cc|cpp|cxx|c|hpp)$"
    --html-details
    --print-summary
    -o "${MSKDSP_COVERAGE_OUTPUT_DIR}/index.html"
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
  VERBATIM
  USES_TERMINAL
)

add_dependencies(coverage ${MSKDSP_TEST_TARGETS})
