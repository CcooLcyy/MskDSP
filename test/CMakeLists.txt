enable_testing()

find_package(GTest CONFIG REQUIRED)

set(MSKDSP_TEST_RUNNER "${CMAKE_CURRENT_SOURCE_DIR}/RunIsolatedTest.cmake")

function(mskdsp_add_isolated_test test_name)
  set(options)
  set(oneValueArgs TARGET WORKING_DIRECTORY)
  cmake_parse_arguments(MSKDSP_TEST "${options}" "${oneValueArgs}" "" ${ARGN})

  if(NOT MSKDSP_TEST_TARGET)
    set(MSKDSP_TEST_TARGET "${test_name}")
  endif()

  if(NOT MSKDSP_TEST_WORKING_DIRECTORY)
    set(MSKDSP_TEST_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/test_env/${test_name}")
  endif()

  add_test(NAME ${test_name}
    COMMAND ${CMAKE_COMMAND}
      "-DMSKDSP_TEST_NAME=${test_name}"
      "-DMSKDSP_TEST_EXECUTABLE=$<TARGET_FILE:${MSKDSP_TEST_TARGET}>"
      "-DMSKDSP_TEST_WORKDIR=${MSKDSP_TEST_WORKING_DIRECTORY}"
      -P "${MSKDSP_TEST_RUNNER}"
  )
endfunction()

set(MSKDSP_MODULE_MANAGER_TEST_ENV_DIR "${CMAKE_BINARY_DIR}/test_env/moduleManager")
set(MSKDSP_MODULE_MANAGER_TEST_LIB_DIR "${MSKDSP_MODULE_MANAGER_TEST_ENV_DIR}/module")
file(MAKE_DIRECTORY "${MSKDSP_MODULE_MANAGER_TEST_LIB_DIR}")

add_library(moduleManagerDummyModule SHARED
  module_manager_dummy_module.cc
)

set_target_properties(moduleManagerDummyModule PROPERTIES
  OUTPUT_NAME Dummy
  VERSION 0.0.1
  SOVERSION 0
  LIBRARY_OUTPUT_DIRECTORY "${MSKDSP_MODULE_MANAGER_TEST_LIB_DIR}"
)

target_link_libraries(moduleManagerDummyModule PRIVATE
  moduleManager
)

add_executable(moduleManager_test
  module_manager_test.cc
)

target_link_libraries(moduleManager_test PRIVATE
  GTest::gtest_main
  moduleManager
)

add_dependencies(moduleManager_test
  moduleManagerDummyModule
)

mskdsp_add_isolated_test(moduleManager_test
  WORKING_DIRECTORY "${MSKDSP_MODULE_MANAGER_TEST_ENV_DIR}"
)

set(MSKDSP_MODULE_INTERFACE_TEST_ENV_DIR "${CMAKE_BINARY_DIR}/test_env/moduleInterface")

add_executable(moduleInterface_test
  module_interface_test.cc
)

target_link_libraries(moduleInterface_test PRIVATE
  GTest::gtest_main
  moduleManager
)

mskdsp_add_isolated_test(moduleInterface_test
  WORKING_DIRECTORY "${MSKDSP_MODULE_INTERFACE_TEST_ENV_DIR}"
)

set(MSKDSP_LOGGER_TEST_ENV_DIR "${CMAKE_BINARY_DIR}/test_env/logger")

add_executable(logger_test
  logger_test.cc
)

target_link_libraries(logger_test PRIVATE
  GTest::gtest_main
  moduleManager
)

mskdsp_add_isolated_test(logger_test
  WORKING_DIRECTORY "${MSKDSP_LOGGER_TEST_ENV_DIR}"
)

add_executable(mqtt_manager_test
  mqtt_manager_test.cc
  ${CMAKE_SOURCE_DIR}/src/MQTTManager/MQTTTopicMatcher.cc
)

target_include_directories(mqtt_manager_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/MQTTManager/include
)

target_link_libraries(mqtt_manager_test PRIVATE
  GTest::gtest_main
  dspProto
  Boost::json
  MQTTManager
)

mskdsp_add_isolated_test(mqtt_manager_test)

add_executable(dataCenterCore_test
  data_center_core_test.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterCore.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterCoreConnection.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterCoreConnTags.cc
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

mskdsp_add_isolated_test(dataCenterCore_test)

add_executable(dataCenterPointTableStore_test
  data_center_point_table_store_test.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterConnTagsStore.cc
)

target_include_directories(dataCenterPointTableStore_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/DataCenter/include
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(dataCenterPointTableStore_test PRIVATE
  GTest::gtest_main
  dspProto
  moduleManager
)

mskdsp_add_isolated_test(dataCenterPointTableStore_test)

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

mskdsp_add_isolated_test(dataCenterRouteStore_test)

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

mskdsp_add_isolated_test(dataCenterConnectionStore_test)

add_executable(dataCenterGrpcService_test
  data_center_grpc_service_test.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterGrpcService.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterCore.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterCoreConnection.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterCoreConnTags.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterCoreRoutes.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterCorePublish.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterConnTagsStore.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterRouteStore.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterConnectionStore.cc
)

target_include_directories(dataCenterGrpcService_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/DataCenter/include
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(dataCenterGrpcService_test PRIVATE
  GTest::gtest_main
  dspProto
  moduleManager
)

mskdsp_add_isolated_test(dataCenterGrpcService_test)

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

mskdsp_add_isolated_test(iec104PointTable_test)

add_executable(modbusRtuPointTable_test
  modbus_rtu_point_table_test.cc
  ${CMAKE_SOURCE_DIR}/src/ModbusRTU/ModbusRTUPointTable.cc
)

target_include_directories(modbusRtuPointTable_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/ModbusRTU/include
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(modbusRtuPointTable_test PRIVATE
  GTest::gtest_main
  dspProto
  moduleManager
)

mskdsp_add_isolated_test(modbusRtuPointTable_test)

add_executable(dlt645PointTable_test
  dlt645_point_table_test.cc
  ${CMAKE_SOURCE_DIR}/src/DLT645/DLT645PointTable.cc
)

target_include_directories(dlt645PointTable_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/DLT645/include
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(dlt645PointTable_test PRIVATE
  GTest::gtest_main
  dspProto
  moduleManager
)

mskdsp_add_isolated_test(dlt645PointTable_test)

add_executable(dlt645LinkManager_test
  dlt645_link_manager_test.cc
  ${CMAKE_SOURCE_DIR}/src/DLT645/DLT645LinkManager.cc
  ${CMAKE_SOURCE_DIR}/src/DLT645/DLT645DataCenterClient.cc
  ${CMAKE_SOURCE_DIR}/src/DLT645/DLT645LinkStore.cc
  ${CMAKE_SOURCE_DIR}/src/DLT645/DLT645MqttClient.cc
  ${CMAKE_SOURCE_DIR}/src/DLT645/DLT645MqttStore.cc
  ${CMAKE_SOURCE_DIR}/src/DLT645/DLT645PointTable.cc
  ${CMAKE_SOURCE_DIR}/src/DLT645/DLT645PointTableStore.cc
)

target_include_directories(dlt645LinkManager_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/DLT645/include
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(dlt645LinkManager_test PRIVATE
  GTest::gtest_main
  GTest::gmock
  dspProto
  Boost::json
  moduleManager
)

mskdsp_add_isolated_test(dlt645LinkManager_test)

add_executable(dlt645MqttClient_test
  dlt645_mqtt_client_test.cc
  ${CMAKE_SOURCE_DIR}/src/DLT645/DLT645MqttClient.cc
)

target_include_directories(dlt645MqttClient_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/DLT645/include
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(dlt645MqttClient_test PRIVATE
  GTest::gtest_main
  GTest::gmock
  dspProto
  moduleManager
)

mskdsp_add_isolated_test(dlt645MqttClient_test)

add_executable(dlt645DataCenterClient_test
  dlt645_data_center_client_test.cc
  ${CMAKE_SOURCE_DIR}/src/DLT645/DLT645DataCenterClient.cc
)

target_include_directories(dlt645DataCenterClient_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/DLT645/include
)

target_link_libraries(dlt645DataCenterClient_test PRIVATE
  GTest::gtest_main
  GTest::gmock
  dspProto
)

mskdsp_add_isolated_test(dlt645DataCenterClient_test)

add_executable(dlt645GrpcService_test
  dlt645_grpc_service_test.cc
)

target_include_directories(dlt645GrpcService_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/DLT645/include
)

target_link_libraries(dlt645GrpcService_test PRIVATE
  GTest::gtest_main
  GTest::gmock
  dspProto
  DLT645
)

mskdsp_add_isolated_test(dlt645GrpcService_test)

add_executable(iec104GrpcService_test
  iec104_grpc_service_test.cc
)

target_include_directories(iec104GrpcService_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC104/include
)

target_link_libraries(iec104GrpcService_test PRIVATE
  GTest::gtest_main
  GTest::gmock
  dspProto
  IEC104
)

mskdsp_add_isolated_test(iec104GrpcService_test)

add_executable(configPusherApplyConfig_test
  config_pusher_apply_config_test.cc
)

target_include_directories(configPusherApplyConfig_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/ConfigPusher/include
)

target_link_libraries(configPusherApplyConfig_test PRIVATE
  GTest::gtest_main
  GTest::gmock
  dspProto
  ConfigPusher
)

mskdsp_add_isolated_test(configPusherApplyConfig_test)

add_executable(configPusherApplyAgc_test
  config_pusher_apply_agc_test.cc
)

target_include_directories(configPusherApplyAgc_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/ConfigPusher/include
)

target_link_libraries(configPusherApplyAgc_test PRIVATE
  GTest::gtest_main
  GTest::gmock
  dspProto
  ConfigPusher
)

mskdsp_add_isolated_test(configPusherApplyAgc_test)

add_executable(configPusherApplyDlt645_test
  config_pusher_apply_dlt645_test.cc
)

target_include_directories(configPusherApplyDlt645_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/ConfigPusher/include
)

target_link_libraries(configPusherApplyDlt645_test PRIVATE
  GTest::gtest_main
  GTest::gmock
  dspProto
  ConfigPusher
)

mskdsp_add_isolated_test(configPusherApplyDlt645_test)

add_executable(iec104LinkManager_test
  iec104_link_manager_test.cc
  iec104_link_manager_helper_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC104/IEC104DataCenterClient.cc
  ${CMAKE_SOURCE_DIR}/src/IEC104/IEC104LinkManager.cc
  ${CMAKE_SOURCE_DIR}/src/IEC104/IEC104LinkManagerDataCenter.cc
  ${CMAKE_SOURCE_DIR}/src/IEC104/IEC104LinkStore.cc
  ${CMAKE_SOURCE_DIR}/src/IEC104/IEC104PointTable.cc
  ${CMAKE_SOURCE_DIR}/src/IEC104/IEC104PointTableStore.cc
  ${CMAKE_SOURCE_DIR}/src/IEC104/IEC104TcpLink.cc
  ${CMAKE_SOURCE_DIR}/src/IEC104/IEC104TcpSession.cc
)

target_include_directories(iec104LinkManager_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC104/include
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(iec104LinkManager_test PRIVATE
  GTest::gtest_main
  GTest::gmock
  dspProto
  Boost::asio
  moduleManager
)

mskdsp_add_isolated_test(iec104LinkManager_test)

add_executable(iec104Persistence_test
  iec104_persistence_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC104/IEC104DataCenterClient.cc
  ${CMAKE_SOURCE_DIR}/src/IEC104/IEC104LinkManager.cc
  ${CMAKE_SOURCE_DIR}/src/IEC104/IEC104LinkManagerDataCenter.cc
  ${CMAKE_SOURCE_DIR}/src/IEC104/IEC104LinkStore.cc
  ${CMAKE_SOURCE_DIR}/src/IEC104/IEC104PointTable.cc
  ${CMAKE_SOURCE_DIR}/src/IEC104/IEC104PointTableStore.cc
  ${CMAKE_SOURCE_DIR}/src/IEC104/IEC104TcpLink.cc
  ${CMAKE_SOURCE_DIR}/src/IEC104/IEC104TcpSession.cc
)

target_include_directories(iec104Persistence_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC104/include
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(iec104Persistence_test PRIVATE
  GTest::gtest_main
  GTest::gmock
  dspProto
  Boost::asio
  moduleManager
)

mskdsp_add_isolated_test(iec104Persistence_test)

add_executable(modbusRtuLinkManager_test
  modbus_rtu_data_center_client_test.cc
  modbus_rtu_link_manager_test.cc
  modbus_rtu_serial_bus_test.cc
  ${CMAKE_SOURCE_DIR}/src/ModbusRTU/ModbusRTUDataCenterClient.cc
  ${CMAKE_SOURCE_DIR}/src/ModbusRTU/ModbusRTULinkManager.cc
  ${CMAKE_SOURCE_DIR}/src/ModbusRTU/ModbusRTULinkStore.cc
  ${CMAKE_SOURCE_DIR}/src/ModbusRTU/ModbusRTUMqttBus.cc
  ${CMAKE_SOURCE_DIR}/src/ModbusRTU/ModbusRTUMqttClient.cc
  ${CMAKE_SOURCE_DIR}/src/ModbusRTU/ModbusRTUMqttStore.cc
  ${CMAKE_SOURCE_DIR}/src/ModbusRTU/ModbusRTUPointTable.cc
  ${CMAKE_SOURCE_DIR}/src/ModbusRTU/ModbusRTUPointTableStore.cc
  ${CMAKE_SOURCE_DIR}/src/ModbusRTU/ModbusRTUSerialBus.cc
)

target_include_directories(modbusRtuLinkManager_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/ModbusRTU/include
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(modbusRtuLinkManager_test PRIVATE
  GTest::gtest_main
  GTest::gmock
  dspProto
  Boost::asio
  Boost::json
  moduleManager
)

mskdsp_add_isolated_test(modbusRtuLinkManager_test)

add_executable(configPusherApplyModbusRtu_test
  config_pusher_apply_modbus_rtu_test.cc
)

target_include_directories(configPusherApplyModbusRtu_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/ConfigPusher/include
)

target_link_libraries(configPusherApplyModbusRtu_test PRIVATE
  GTest::gtest_main
  GTest::gmock
  dspProto
  ConfigPusher
)

mskdsp_add_isolated_test(configPusherApplyModbusRtu_test)

add_executable(iec104TcpSession_test
  iec104_tcp_session_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC104/IEC104TcpSession.cc
)

target_include_directories(iec104TcpSession_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC104/include
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(iec104TcpSession_test PRIVATE
  GTest::gtest_main
  Boost::asio
  dspProto
  moduleManager
)

mskdsp_add_isolated_test(iec104TcpSession_test)

add_executable(agvcStrategy_test
  agvc_strategy_test.cc
)

target_link_libraries(agvcStrategy_test PRIVATE
  GTest::gtest_main
  agvcCommon
)

mskdsp_add_isolated_test(agvcStrategy_test)

add_executable(agcGroupManager_test
  agc_group_manager_test.cc
  ${CMAKE_SOURCE_DIR}/src/AGVC/AGC/AGCGroupManager.cc
  ${CMAKE_SOURCE_DIR}/src/AGVC/AGC/AGCGroupStore.cc
  ${CMAKE_SOURCE_DIR}/src/AGVC/AGC/AGCGroupValidation.cc
  ${CMAKE_SOURCE_DIR}/src/AGVC/AGC/AGCControl.cc
)

target_include_directories(agcGroupManager_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/AGVC/AGC/include
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(agcGroupManager_test PRIVATE
  GTest::gtest_main
  GTest::gmock
  agvcCommon
  dspProto
  moduleManager
)

mskdsp_add_isolated_test(agcGroupManager_test)

add_executable(agcGroupStore_test
  agc_group_store_test.cc
  ${CMAKE_SOURCE_DIR}/src/AGVC/AGC/AGCGroupStore.cc
  ${CMAKE_SOURCE_DIR}/src/AGVC/AGC/AGCGroupValidation.cc
)

target_include_directories(agcGroupStore_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/AGVC/AGC/include
)

target_link_libraries(agcGroupStore_test PRIVATE
  GTest::gtest_main
  dspProto
)

mskdsp_add_isolated_test(agcGroupStore_test)

add_executable(agcControl_test
  agc_control_test.cc
  ${CMAKE_SOURCE_DIR}/src/AGVC/AGC/AGCControl.cc
)

target_include_directories(agcControl_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/AGVC/AGC/include
)

target_link_libraries(agcControl_test PRIVATE
  GTest::gtest_main
  agvcCommon
)

mskdsp_add_isolated_test(agcControl_test)

add_executable(configPusherDataCenter_test
  config_pusher_data_center_test.cc
  ${CMAKE_SOURCE_DIR}/src/ConfigPusher/ConfigPusherDataCenter.cc
)

target_include_directories(configPusherDataCenter_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/ConfigPusher/include
)

target_link_libraries(configPusherDataCenter_test PRIVATE
  GTest::gtest_main
  GTest::gmock
  dspProto
  moduleManager
)

mskdsp_add_isolated_test(configPusherDataCenter_test)

add_executable(configPusherConfigLoader_test
  config_pusher_config_loader_test.cc
  ${CMAKE_SOURCE_DIR}/src/ConfigPusher/ConfigPusherConfigLoader.cc
)

target_include_directories(configPusherConfigLoader_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/ConfigPusher/include
)

target_link_libraries(configPusherConfigLoader_test PRIVATE
  GTest::gtest_main
  dspProto
  moduleManager
)

mskdsp_add_isolated_test(configPusherConfigLoader_test)

add_executable(configPusherModuleManager_test
  config_pusher_module_manager_test.cc
  ${CMAKE_SOURCE_DIR}/src/ConfigPusher/ConfigPusherModuleManager.cc
)

target_include_directories(configPusherModuleManager_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/ConfigPusher/include
)

target_link_libraries(configPusherModuleManager_test PRIVATE
  GTest::gtest_main
  dspProto
  moduleManager
)

target_compile_definitions(configPusherModuleManager_test PRIVATE
  MSKDSP_TEST_DISABLE_PROTO_LOG=1
)

mskdsp_add_isolated_test(configPusherModuleManager_test)

set(MSKDSP_TEST_TARGETS
  moduleManager_test
  moduleInterface_test
  logger_test
  mqtt_manager_test
  dataCenterCore_test
  dataCenterPointTableStore_test
  dataCenterRouteStore_test
  dataCenterConnectionStore_test
  dataCenterGrpcService_test
  iec104PointTable_test
  modbusRtuPointTable_test
  dlt645LinkManager_test
  dlt645MqttClient_test
  dlt645DataCenterClient_test
  dlt645GrpcService_test
  iec104GrpcService_test
  iec104LinkManager_test
  iec104Persistence_test
  modbusRtuLinkManager_test
  iec104TcpSession_test
  agvcStrategy_test
  agcGroupManager_test
  agcGroupStore_test
  agcControl_test
  configPusherDataCenter_test
  configPusherConfigLoader_test
  configPusherModuleManager_test
  configPusherApplyConfig_test
  configPusherApplyAgc_test
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
set(MSKDSP_GCOV_EXECUTABLE "")
set(MSKDSP_GCOV_IGNORE_ERRORS "no_working_dir_found")
set(MSKDSP_GCOV_NAMES gcov)
set(MSKDSP_REQUIRE_GCOV TRUE)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  string(REGEX MATCH "^[0-9]+" MSKDSP_GCC_MAJOR "${CMAKE_CXX_COMPILER_VERSION}")
  if(MSKDSP_GCC_MAJOR)
    list(PREPEND MSKDSP_GCOV_NAMES "gcov-${MSKDSP_GCC_MAJOR}")
  endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(MSKDSP_LLVM_COV_NAMES llvm-cov llvm-cov-18 llvm-cov-17 llvm-cov-16 llvm-cov-15)
  find_program(LLVM_COV_EXECUTABLE NAMES ${MSKDSP_LLVM_COV_NAMES} DOC "Path to llvm-cov")
  if(LLVM_COV_EXECUTABLE)
    set(MSKDSP_GCOV_EXECUTABLE "${LLVM_COV_EXECUTABLE} gcov")
    set(MSKDSP_REQUIRE_GCOV FALSE)
  else()
    message(WARNING "未找到 llvm-cov，将使用 gcov 并忽略覆盖率工具错误")
    set(MSKDSP_GCOV_IGNORE_ERRORS "all")
  endif()
endif()
if(MSKDSP_REQUIRE_GCOV)
  find_program(GCOV_EXECUTABLE NAMES ${MSKDSP_GCOV_NAMES} DOC "Path to gcov")
  if(NOT GCOV_EXECUTABLE)
    message(FATAL_ERROR "gcov not found: install it or set -DGCOV_EXECUTABLE=/path/to/gcov")
  endif()
  if(NOT EXISTS "${GCOV_EXECUTABLE}")
    message(FATAL_ERROR "GCOV_EXECUTABLE does not exist: ${GCOV_EXECUTABLE}")
  endif()
  set(MSKDSP_GCOV_EXECUTABLE "${GCOV_EXECUTABLE}")
endif()

set(MSKDSP_COVERAGE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/coverage")
add_custom_target(coverage
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${MSKDSP_COVERAGE_OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MSKDSP_COVERAGE_OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -DMSKDSP_COVERAGE_ROOT="${CMAKE_BINARY_DIR}" -P "${CMAKE_SOURCE_DIR}/cmake/CleanCoverageData.cmake"
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    COMMAND "${GCOVR_EXECUTABLE}"
      -r "${CMAKE_SOURCE_DIR}"
      --object-directory "${CMAKE_BINARY_DIR}"
      --gcov-executable "${MSKDSP_GCOV_EXECUTABLE}"
      --gcov-ignore-errors=${MSKDSP_GCOV_IGNORE_ERRORS}
      --merge-mode-functions=merge-use-line-min
      --filter "(^|.*/)src/.*[.](cc|cpp|cxx|c|hpp)$"
      --html-details
      --print-summary
      -o "${MSKDSP_COVERAGE_OUTPUT_DIR}/index.html"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    VERBATIM
    USES_TERMINAL
  )

add_dependencies(coverage ${MSKDSP_TEST_TARGETS})
