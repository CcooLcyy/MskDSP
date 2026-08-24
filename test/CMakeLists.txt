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
      "-DMSKDSP_TEST_LIBRARY_PATH=${MSKDSP_TEST_LIBRARY_PATH}"
      -P "${MSKDSP_TEST_RUNNER}"
  )
endfunction()

# 测试可执行文件输出到 package；模块共享库位于 package/module，基础运行库位于
# package/lib，Debug依赖库位于 vcpkg 目录。运行隔离测试时显式提供全部搜索路径，
# 避免测试工作目录变化后只能找到部分动态库。这里直接基于顶层包目录构造路径，
# 不依赖 modulePath/libPath 这类历史变量，确保测试子目录单独重配置时路径仍然明确。
set(MSKDSP_TEST_PACKAGE_MODULE_DIR "${MSKDSP_PACKAGE_DIR}/module")
set(MSKDSP_TEST_PACKAGE_LIB_DIR "${MSKDSP_PACKAGE_DIR}/lib")
set(MSKDSP_TEST_LIBRARY_PATH
    "${MSKDSP_TEST_PACKAGE_MODULE_DIR}:${MSKDSP_TEST_PACKAGE_LIB_DIR}:${MSKDSP_VCPKG_LIB_DIR}")
if(EXISTS "${MSKDSP_VCPKG_LIB_DIR}/manual-link")
  string(APPEND MSKDSP_TEST_LIBRARY_PATH ":${MSKDSP_VCPKG_LIB_DIR}/manual-link")
endif()

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

add_library(moduleManagerAvcTraceModule SHARED
  module_manager_dummy_module.cc
)

target_compile_definitions(moduleManagerAvcTraceModule PRIVATE
  MSKDSP_TEST_MODULE_NAME="AVC"
)

set_target_properties(moduleManagerAvcTraceModule PROPERTIES
  OUTPUT_NAME AVC
  VERSION 0.0.1
  SOVERSION 0
  LIBRARY_OUTPUT_DIRECTORY "${MSKDSP_MODULE_MANAGER_TEST_LIB_DIR}"
)

target_link_libraries(moduleManagerAvcTraceModule PRIVATE
  moduleManager
)

add_library(moduleManagerDataCenterTraceModule SHARED
  module_manager_dummy_module.cc
)

target_compile_definitions(moduleManagerDataCenterTraceModule PRIVATE
  MSKDSP_TEST_MODULE_NAME="DataCenter"
)

set_target_properties(moduleManagerDataCenterTraceModule PROPERTIES
  OUTPUT_NAME DataCenter
  VERSION 0.0.1
  SOVERSION 0
  LIBRARY_OUTPUT_DIRECTORY "${MSKDSP_MODULE_MANAGER_TEST_LIB_DIR}"
)

target_link_libraries(moduleManagerDataCenterTraceModule PRIVATE
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
  moduleManagerAvcTraceModule
  moduleManagerDataCenterTraceModule
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

add_executable(configDatabase_test
  config_database_test.cc
)

target_include_directories(configDatabase_test PRIVATE
  ${MSKDSP_COMMON_INCLUDE_DIR}
)

target_link_libraries(configDatabase_test PRIVATE
  GTest::gtest_main
  dspProto
  mskdspConfigStore
)

mskdsp_add_isolated_test(configDatabase_test)

add_executable(calc_group_manager_test
  calc_group_manager_test.cc
)

target_include_directories(calc_group_manager_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/Calc/include
)

target_link_libraries(calc_group_manager_test PRIVATE
  GTest::gtest_main
  GTest::gmock
  dspProto
  Calc
)

mskdsp_add_isolated_test(calc_group_manager_test)

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
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(dataCenterCore_test PRIVATE
  GTest::gtest_main
  dspProto
  moduleManager
)

mskdsp_add_isolated_test(dataCenterCore_test)

add_executable(dataCenterStateStore_test
  data_center_state_store_test.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterStateStore.cc
)

target_include_directories(dataCenterStateStore_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/DataCenter/include
  ${MSKDSP_COMMON_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(dataCenterStateStore_test PRIVATE
  GTest::gtest_main
  dspProto
  moduleManager
  mskdspConfigStore
)

mskdsp_add_isolated_test(dataCenterStateStore_test)

add_executable(dataCenterGrpcService_test
  data_center_grpc_service_test.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterGrpcService.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterCore.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterCoreConnection.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterCoreConnTags.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterCoreRoutes.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterCorePublish.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterStateStore.cc
)

target_include_directories(dataCenterGrpcService_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/DataCenter/include
  ${MSKDSP_COMMON_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(dataCenterGrpcService_test PRIVATE
  GTest::gtest_main
  dspProto
  moduleManager
  mskdspConfigStore
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

add_executable(iec61850SclParser_test
  iec61850_scl_parser_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850SclParser.cc
)

target_include_directories(iec61850SclParser_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
)

target_link_libraries(iec61850SclParser_test PRIVATE
  GTest::gtest_main
  dspProto
  pugixml::pugixml
)

mskdsp_add_isolated_test(iec61850SclParser_test)

add_executable(iec61850ConfigValidation_test
  iec61850_config_validation_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ConfigValidation.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ModelSelection.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ThreadRuntimePolicy.cc
)

target_include_directories(iec61850ConfigValidation_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
  ${MSKDSP_COMMON_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(iec61850ConfigValidation_test PRIVATE
  GTest::gtest_main
  dspProto
  moduleManager
)

mskdsp_add_isolated_test(iec61850ConfigValidation_test)

add_executable(iec61850ConfigStore_test
  iec61850_config_store_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ConfigStore.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ConfigValidation.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ModelSelection.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ThreadRuntimePolicy.cc
)

target_include_directories(iec61850ConfigStore_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
  ${MSKDSP_COMMON_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(iec61850ConfigStore_test PRIVATE
  GTest::gtest_main
  dspProto
  moduleManager
  mskdspConfigStore
)

mskdsp_add_isolated_test(iec61850ConfigStore_test)

add_executable(iec61850Manager_test
  iec61850_manager_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850Manager.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsControl.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsSession.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsService.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsBer.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ProtocolStack.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsPipeline.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850DataCenterClient.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ConfigStore.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ConfigValidation.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ModelSelection.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850RealtimePlan.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850RealtimeSignalBus.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850RealtimeSignalProcessor.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ProtectionEngine.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ProtectionActionDispatcher.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ThreadRuntimePolicy.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850GooseRuntime.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850SvRuntime.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850SvMathEngine.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850SvMath.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850SclParser.cc
)

target_include_directories(iec61850Manager_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
  ${MSKDSP_COMMON_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(iec61850Manager_test PRIVATE
  GTest::gtest_main
  GTest::gmock
  dspProto
  moduleManager
  mskdspConfigStore
  pugixml::pugixml
)

mskdsp_add_isolated_test(iec61850Manager_test)

add_executable(iec61850MmsPipeline_test
  iec61850_mms_pipeline_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsPipeline.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850DataCenterClient.cc
)

target_include_directories(iec61850MmsPipeline_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
  ${MSKDSP_COMMON_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(iec61850MmsPipeline_test PRIVATE
  GTest::gtest_main
  GTest::gmock
  dspProto
  moduleManager
)

mskdsp_add_isolated_test(iec61850MmsPipeline_test)

add_executable(iec61850MmsSession_test
  iec61850_mms_session_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsSession.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsChannelPolicy.cc
)

target_include_directories(iec61850MmsSession_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
  ${MSKDSP_COMMON_INCLUDE_DIR}
)

target_link_libraries(iec61850MmsSession_test PRIVATE
  GTest::gtest_main
  dspProto
)

mskdsp_add_isolated_test(iec61850MmsSession_test)

add_executable(iec61850MmsIsoSession_test
  iec61850_mms_iso_session_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsIsoSession.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsBer.cc
)

target_include_directories(iec61850MmsIsoSession_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
)

target_link_libraries(iec61850MmsIsoSession_test PRIVATE
  GTest::gtest_main
  dspProto
)

mskdsp_add_isolated_test(iec61850MmsIsoSession_test)

add_executable(iec61850MmsInitiate_test
  iec61850_mms_initiate_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsPdu.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsBer.cc
)

target_include_directories(iec61850MmsInitiate_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
)

target_link_libraries(iec61850MmsInitiate_test PRIVATE
  GTest::gtest_main
  dspProto
)

mskdsp_add_isolated_test(iec61850MmsInitiate_test)

add_executable(iec61850MmsService_test
  iec61850_mms_service_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsService.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsOptional.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsBer.cc
)

target_include_directories(iec61850MmsService_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
)

target_link_libraries(iec61850MmsService_test PRIVATE
  GTest::gtest_main
  dspProto
)

mskdsp_add_isolated_test(iec61850MmsService_test)

add_executable(iec61850MmsFile_test
  iec61850_mms_file_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsFile.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsExchange.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsIsoSession.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsService.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsOptional.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsBer.cc
)

target_include_directories(iec61850MmsFile_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(iec61850MmsFile_test PRIVATE
  GTest::gtest_main
  dspProto
  moduleManager
)

mskdsp_add_isolated_test(iec61850MmsFile_test)

add_executable(iec61850MmsOptionalTransaction_test
  iec61850_mms_optional_transaction_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsJournal.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsDynamicDataSet.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsFile.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsExchange.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsIsoSession.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsService.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsOptional.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsBer.cc
)

target_include_directories(iec61850MmsOptionalTransaction_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(iec61850MmsOptionalTransaction_test PRIVATE
  GTest::gtest_main
  dspProto
  moduleManager
)

mskdsp_add_isolated_test(iec61850MmsOptionalTransaction_test)

add_executable(iec61850MmsServer_test
  iec61850_mms_server_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsServer.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsJournal.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsService.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsOptional.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsBer.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsPdu.cc
)

target_include_directories(iec61850MmsServer_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
)

target_link_libraries(iec61850MmsServer_test PRIVATE
  GTest::gtest_main
  dspProto
)

mskdsp_add_isolated_test(iec61850MmsServer_test)

add_executable(iec61850MmsSettingGroup_test
  iec61850_mms_setting_group_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsSettingGroup.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsService.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsBer.cc
)

target_include_directories(iec61850MmsSettingGroup_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(iec61850MmsSettingGroup_test PRIVATE
  GTest::gtest_main
  dspProto
  moduleManager
)

mskdsp_add_isolated_test(iec61850MmsSettingGroup_test)

add_executable(iec61850MmsControl_test
  iec61850_mms_control_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsControl.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsSession.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsService.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsBer.cc
)

target_include_directories(iec61850MmsControl_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
  ${MSKDSP_COMMON_INCLUDE_DIR}
)

target_link_libraries(iec61850MmsControl_test PRIVATE
  GTest::gtest_main
  dspProto
)

mskdsp_add_isolated_test(iec61850MmsControl_test)

add_executable(iec61850MmsReport_test
  iec61850_mms_report_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsReport.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsBer.cc
)

target_include_directories(iec61850MmsReport_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
  ${MSKDSP_COMMON_INCLUDE_DIR}
)

target_link_libraries(iec61850MmsReport_test PRIVATE
  GTest::gtest_main
  dspProto
)

mskdsp_add_isolated_test(iec61850MmsReport_test)

add_executable(iec61850MmsRcb_test
  iec61850_mms_rcb_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsRcb.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsSession.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsService.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsBer.cc
)

target_include_directories(iec61850MmsRcb_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
  ${MSKDSP_COMMON_INCLUDE_DIR}
)

target_link_libraries(iec61850MmsRcb_test PRIVATE
  GTest::gtest_main
  dspProto
)

mskdsp_add_isolated_test(iec61850MmsRcb_test)

add_executable(iec61850GooseState_test
  iec61850_goose_state_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850GooseState.cc
)

target_include_directories(iec61850GooseState_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
)

target_link_libraries(iec61850GooseState_test PRIVATE
  GTest::gtest_main
  dspProto
)

mskdsp_add_isolated_test(iec61850GooseState_test)

add_executable(iec61850SvState_test
  iec61850_sv_state_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850SvState.cc
)

target_include_directories(iec61850SvState_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
)

target_link_libraries(iec61850SvState_test PRIVATE
  GTest::gtest_main
)

mskdsp_add_isolated_test(iec61850SvState_test)

add_executable(iec61850SvMath_test
  iec61850_sv_math_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850SvMath.cc
)

target_include_directories(iec61850SvMath_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
)

target_link_libraries(iec61850SvMath_test PRIVATE
  GTest::gtest_main
)

mskdsp_add_isolated_test(iec61850SvMath_test)

add_executable(iec61850SvMathEngine_test
  iec61850_sv_math_engine_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850SvMathEngine.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850SvMath.cc
)

target_include_directories(iec61850SvMathEngine_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
)

target_link_libraries(iec61850SvMathEngine_test PRIVATE
  GTest::gtest_main
  dspProto
)

mskdsp_add_isolated_test(iec61850SvMathEngine_test)

add_executable(iec61850SvRuntime_test
  iec61850_sv_runtime_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850SvRuntime.cc
)

target_include_directories(iec61850SvRuntime_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
)

target_link_libraries(iec61850SvRuntime_test PRIVATE
  GTest::gtest_main
  dspProto
)

mskdsp_add_isolated_test(iec61850SvRuntime_test)

add_executable(iec61850RealtimePlan_test
  iec61850_realtime_plan_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850RealtimePlan.cc
)

target_include_directories(iec61850RealtimePlan_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
)

target_link_libraries(iec61850RealtimePlan_test PRIVATE
  GTest::gtest_main
  dspProto
)

mskdsp_add_isolated_test(iec61850RealtimePlan_test)

add_executable(iec61850GooseRuntime_test
  iec61850_goose_runtime_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850GooseRuntime.cc
)

target_include_directories(iec61850GooseRuntime_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
)

target_link_libraries(iec61850GooseRuntime_test PRIVATE
  GTest::gtest_main
  dspProto
)

mskdsp_add_isolated_test(iec61850GooseRuntime_test)

add_executable(iec61850RawEthernet_test
  iec61850_raw_ethernet_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850RawEthernet.cc
)

target_include_directories(iec61850RawEthernet_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
)

target_link_libraries(iec61850RawEthernet_test PRIVATE
  GTest::gtest_main
  dspProto
)

mskdsp_add_isolated_test(iec61850RawEthernet_test)

add_executable(iec61850MmsTransport_test
  iec61850_mms_transport_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsTransport.cc
)

target_include_directories(iec61850MmsTransport_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(iec61850MmsTransport_test PRIVATE
  GTest::gtest_main
  moduleManager
)

mskdsp_add_isolated_test(iec61850MmsTransport_test)

add_executable(iec61850MmsExchange_test
  iec61850_mms_exchange_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsExchange.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsIsoSession.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsService.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsBer.cc
)

target_include_directories(iec61850MmsExchange_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(iec61850MmsExchange_test PRIVATE
  GTest::gtest_main
  dspProto
  moduleManager
)

mskdsp_add_isolated_test(iec61850MmsExchange_test)

add_executable(iec61850MmsBer_test
  iec61850_mms_ber_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsBer.cc
)

target_include_directories(iec61850MmsBer_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
)

target_link_libraries(iec61850MmsBer_test PRIVATE
  GTest::gtest_main
  dspProto
)

mskdsp_add_isolated_test(iec61850MmsBer_test)

add_executable(iec61850RawProtocolStack_test
  iec61850_raw_protocol_stack_test.cc
  iec61850_mms_worker_test.cc
  iec61850_mms_tcp_integration_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850RawProtocolStack.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850RawEthernet.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850GoosePublisher.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850SvPublisher.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsWorker.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsFile.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsControl.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsChannelPolicy.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsExchange.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsTransport.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsIsoSession.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsPdu.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsService.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsOptional.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsSession.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsRcb.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsReport.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsSettingGroup.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsBer.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ThreadRuntimePolicy.cc
)

target_include_directories(iec61850RawProtocolStack_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
  ${MSKDSP_COMMON_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(iec61850RawProtocolStack_test PRIVATE
  GTest::gtest_main
  dspProto
  moduleManager
)

mskdsp_add_isolated_test(iec61850RawProtocolStack_test)

# 验证生产MMS Worker与独立模拟IED之间的跨进程建链、目录、RCB和GI闭环。
if(TARGET iec61850_mms_sim AND TARGET iec61850_mms_worker_smoke)
  add_executable(iec61850MmsProcess_test
    iec61850_mms_process_test.cc
  )

  add_dependencies(iec61850MmsProcess_test
    iec61850_mms_sim
    iec61850_mms_worker_smoke
  )

  add_test(NAME iec61850MmsProcess_test
    COMMAND iec61850MmsProcess_test
      $<TARGET_FILE:iec61850_mms_sim>
      $<TARGET_FILE:iec61850_mms_worker_smoke>
  )
  set_tests_properties(iec61850MmsProcess_test PROPERTIES
    TIMEOUT 90
  )
endif()

add_executable(iec61850RealtimeSignalBus_test
  iec61850_realtime_signal_bus_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850RealtimeSignalBus.cc
)

target_include_directories(iec61850RealtimeSignalBus_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
)

target_link_libraries(iec61850RealtimeSignalBus_test PRIVATE
  GTest::gtest_main
)

mskdsp_add_isolated_test(iec61850RealtimeSignalBus_test)

add_executable(iec61850RealtimeSignalProcessor_test
  iec61850_realtime_signal_processor_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850RealtimeSignalProcessor.cc
)

target_include_directories(iec61850RealtimeSignalProcessor_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
)

target_link_libraries(iec61850RealtimeSignalProcessor_test PRIVATE
  GTest::gtest_main
  dspProto
)

mskdsp_add_isolated_test(iec61850RealtimeSignalProcessor_test)

add_executable(iec61850ProtectionEngine_test
  iec61850_protection_engine_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ProtectionEngine.cc
  iec61850_protection_action_dispatcher_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ProtectionActionDispatcher.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ThreadRuntimePolicy.cc
)

target_include_directories(iec61850ProtectionEngine_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(iec61850ProtectionEngine_test PRIVATE
  GTest::gtest_main
  dspProto
  moduleManager
)

mskdsp_add_isolated_test(iec61850ProtectionEngine_test)

add_executable(iec61850ThreadRuntimePolicy_test
  iec61850_thread_runtime_policy_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ThreadRuntimePolicy.cc
)

target_include_directories(iec61850ThreadRuntimePolicy_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(iec61850ThreadRuntimePolicy_test PRIVATE
  GTest::gtest_main
  moduleManager
)

mskdsp_add_isolated_test(iec61850ThreadRuntimePolicy_test)

add_executable(iec61850GrpcService_test
  iec61850_grpc_service_test.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850CommandExecutorService.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850GrpcService.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850Manager.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ProtocolStack.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsControl.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsSession.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsService.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsBer.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850MmsPipeline.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850DataCenterClient.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ConfigStore.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ConfigValidation.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ModelSelection.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850RealtimePlan.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850RealtimeSignalBus.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850RealtimeSignalProcessor.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ProtectionEngine.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ProtectionActionDispatcher.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850ThreadRuntimePolicy.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850GooseRuntime.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850SvRuntime.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850SvMathEngine.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850SvMath.cc
  ${CMAKE_SOURCE_DIR}/src/IEC61850/IEC61850SclParser.cc
)

target_include_directories(iec61850GrpcService_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/IEC61850/include
  ${MSKDSP_COMMON_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(iec61850GrpcService_test PRIVATE
  GTest::gtest_main
  dspProto
  moduleManager
  mskdspConfigStore
  pugixml::pugixml
)

mskdsp_add_isolated_test(iec61850GrpcService_test)

add_executable(iec61850Manifest_test
  iec61850_manifest_test.cc
)

target_include_directories(iec61850Manifest_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(iec61850Manifest_test PRIVATE
  GTest::gtest_main
  dspProto
  IEC61850
)

# IEC61850模块库输出到package/module，manifest测试位于package根目录。
set_property(TARGET iec61850Manifest_test APPEND PROPERTY
             INSTALL_RPATH "$ORIGIN/module")

mskdsp_add_isolated_test(iec61850Manifest_test)

add_executable(configPusherApplyIec61850_test
  config_pusher_apply_iec61850_test.cc
  ${CMAKE_SOURCE_DIR}/src/ConfigPusher/ConfigPusherApplyIec61850.cc
)

target_include_directories(configPusherApplyIec61850_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/ConfigPusher/include
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
  ${MSKDSP_COMMON_INCLUDE_DIR}
)

target_link_libraries(configPusherApplyIec61850_test PRIVATE
  GTest::gtest_main
  GTest::gmock
  dspProto
  moduleManager
)

mskdsp_add_isolated_test(configPusherApplyIec61850_test)

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

add_executable(modbusRtuManifest_test
  modbus_rtu_manifest_test.cc
)

target_link_libraries(modbusRtuManifest_test PRIVATE
  GTest::gtest_main
  dspProto
  ModbusRTU
)

mskdsp_add_isolated_test(modbusRtuManifest_test)

add_executable(modbusRtuCommandService_test
  modbus_rtu_command_service_test.cc
)

target_include_directories(modbusRtuCommandService_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/ModbusRTU/include
)

target_link_libraries(modbusRtuCommandService_test PRIVATE
  GTest::gtest_main
  dspProto
  ModbusRTU
)

mskdsp_add_isolated_test(modbusRtuCommandService_test)

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
add_dependencies(dlt645LinkManager_test dspProto)

target_include_directories(dlt645LinkManager_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/DLT645/include
  ${MSKDSP_COMMON_INCLUDE_DIR}
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

add_executable(configPusherApplyAvc_test
  config_pusher_apply_avc_test.cc
)

target_include_directories(configPusherApplyAvc_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/ConfigPusher/include
)

target_link_libraries(configPusherApplyAvc_test PRIVATE
  GTest::gtest_main
  GTest::gmock
  dspProto
  ConfigPusher
)

mskdsp_add_isolated_test(configPusherApplyAvc_test)

add_executable(configPusherApplyCalc_test
  config_pusher_apply_calc_test.cc
)

target_include_directories(configPusherApplyCalc_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/ConfigPusher/include
)

target_link_libraries(configPusherApplyCalc_test PRIVATE
  GTest::gtest_main
  GTest::gmock
  dspProto
  ConfigPusher
)

mskdsp_add_isolated_test(configPusherApplyCalc_test)

add_executable(configPusherApplyDlt645_test
  config_pusher_apply_dlt645_test.cc
)
add_dependencies(configPusherApplyDlt645_test dspProto)

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

add_executable(configPusherApplyIec104_test
  config_pusher_apply_iec104_test.cc
)

target_include_directories(configPusherApplyIec104_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/ConfigPusher/include
)

target_link_libraries(configPusherApplyIec104_test PRIVATE
  GTest::gtest_main
  GTest::gmock
  dspProto
  ConfigPusher
)

mskdsp_add_isolated_test(configPusherApplyIec104_test)

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
  ${MSKDSP_COMMON_INCLUDE_DIR}
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
  ${MSKDSP_COMMON_INCLUDE_DIR}
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
  ${MSKDSP_COMMON_INCLUDE_DIR}
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
  ${CMAKE_SOURCE_DIR}/src/AGVC/AGC/AGCDefaultPoints.cc
  ${CMAKE_SOURCE_DIR}/src/AGVC/AGC/AGCGroupValidation.cc
  ${CMAKE_SOURCE_DIR}/src/AGVC/AGC/AGCControl.cc
  ${CMAKE_SOURCE_DIR}/src/AGVC/AGC/AGCControlProfileStore.cc
)

target_include_directories(agcGroupManager_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/AGVC/AGC/include
  ${MSKDSP_COMMON_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(agcGroupManager_test PRIVATE
  GTest::gtest_main
  GTest::gmock
  agvcCommon
  dspProto
  moduleManager
  mskdspConfigStore
)

mskdsp_add_isolated_test(agcGroupManager_test)

add_executable(agcGroupStore_test
  agc_group_store_test.cc
  ${CMAKE_SOURCE_DIR}/src/AGVC/AGC/AGCGroupStore.cc
  ${CMAKE_SOURCE_DIR}/src/AGVC/AGC/AGCDefaultPoints.cc
  ${CMAKE_SOURCE_DIR}/src/AGVC/AGC/AGCGroupValidation.cc
)

target_include_directories(agcGroupStore_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/AGVC/AGC/include
  ${MSKDSP_COMMON_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(agcGroupStore_test PRIVATE
  GTest::gtest_main
  dspProto
  moduleManager
  mskdspConfigStore
)

mskdsp_add_isolated_test(agcGroupStore_test)

add_executable(agcControlProfileStore_test
  agc_control_profile_store_test.cc
  ${CMAKE_SOURCE_DIR}/src/AGVC/AGC/AGCControlProfileStore.cc
)

target_include_directories(agcControlProfileStore_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/AGVC/AGC/include
  ${MSKDSP_COMMON_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(agcControlProfileStore_test PRIVATE
  GTest::gtest_main
  dspProto
  moduleManager
  mskdspConfigStore
)

mskdsp_add_isolated_test(agcControlProfileStore_test)

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

add_executable(avcGroupManager_test
  avc_group_manager_test.cc
  ${CMAKE_SOURCE_DIR}/src/AGVC/AVC/AVCGroupManager.cc
  ${CMAKE_SOURCE_DIR}/src/AGVC/AVC/AVCGroupStore.cc
  ${CMAKE_SOURCE_DIR}/src/AGVC/AVC/AVCDefaultPoints.cc
  ${CMAKE_SOURCE_DIR}/src/AGVC/AVC/AVCGroupValidation.cc
  ${CMAKE_SOURCE_DIR}/src/AGVC/AVC/AVCControl.cc
)

target_include_directories(avcGroupManager_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/AGVC/AVC/include
  ${MSKDSP_COMMON_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(avcGroupManager_test PRIVATE
  GTest::gtest_main
  GTest::gmock
  agvcCommon
  dspProto
  moduleManager
)

mskdsp_add_isolated_test(avcGroupManager_test)

add_executable(avcGroupStore_test
  avc_group_store_test.cc
  ${CMAKE_SOURCE_DIR}/src/AGVC/AVC/AVCGroupStore.cc
  ${CMAKE_SOURCE_DIR}/src/AGVC/AVC/AVCDefaultPoints.cc
  ${CMAKE_SOURCE_DIR}/src/AGVC/AVC/AVCGroupValidation.cc
)

target_include_directories(avcGroupStore_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/AGVC/AVC/include
  ${MSKDSP_COMMON_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/src/core/ModuleManager/include
)

target_link_libraries(avcGroupStore_test PRIVATE
  GTest::gtest_main
  dspProto
  moduleManager
  mskdspConfigStore
)

mskdsp_add_isolated_test(avcGroupStore_test)

add_executable(avcControl_test
  avc_control_test.cc
  ${CMAKE_SOURCE_DIR}/src/AGVC/AVC/AVCControl.cc
)

target_include_directories(avcControl_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/AGVC/AVC/include
)

target_link_libraries(avcControl_test PRIVATE
  GTest::gtest_main
  agvcCommon
)

mskdsp_add_isolated_test(avcControl_test)

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
  configDatabase_test
  dataCenterCore_test
  dataCenterStateStore_test
  dataCenterGrpcService_test
  iec61850SclParser_test
  iec61850ConfigValidation_test
  iec61850ThreadRuntimePolicy_test
  iec61850ConfigStore_test
  iec61850Manager_test
  iec61850MmsPipeline_test
  iec61850MmsSession_test
  iec61850MmsIsoSession_test
  iec61850MmsInitiate_test
  iec61850MmsService_test
  iec61850MmsFile_test
  iec61850MmsOptionalTransaction_test
  iec61850MmsSettingGroup_test
  iec61850MmsServer_test
  iec61850GooseState_test
  iec61850SvState_test
  iec61850SvRuntime_test
  iec61850RealtimePlan_test
  iec61850ProtectionEngine_test
  iec61850GooseRuntime_test
  iec61850RawEthernet_test
  iec61850RawProtocolStack_test
  iec61850GrpcService_test
  iec61850Manifest_test
  configPusherApplyIec61850_test
  iec104PointTable_test
  modbusRtuPointTable_test
  modbusRtuManifest_test
  modbusRtuCommandService_test
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
  agcControlProfileStore_test
  agcControl_test
  avcGroupManager_test
  avcGroupStore_test
  avcControl_test
  configPusherDataCenter_test
  configPusherConfigLoader_test
  configPusherModuleManager_test
  configPusherApplyConfig_test
  configPusherApplyAgc_test
  configPusherApplyAvc_test
  configPusherApplyCalc_test
)

if(MSKDSP_ENABLE_COVERAGE)
  # 仅覆盖率构建需要 gcovr/gcov，普通测试配置不再强制查找这些工具。
  if(DEFINED GCOVR_EXECUTABLE AND
     (GCOVR_EXECUTABLE STREQUAL "" OR GCOVR_EXECUTABLE MATCHES "-NOTFOUND$"))
    unset(GCOVR_EXECUTABLE CACHE)
  endif()

  find_program(GCOVR_EXECUTABLE NAMES gcovr DOC "Path to gcovr")
  if(NOT GCOVR_EXECUTABLE)
    message(FATAL_ERROR "未找到 gcovr，请安装或通过 -DGCOVR_EXECUTABLE=/path/to/gcovr 指定")
  endif()
  if(NOT EXISTS "${GCOVR_EXECUTABLE}")
    message(FATAL_ERROR "GCOVR_EXECUTABLE 指向的文件不存在: ${GCOVR_EXECUTABLE}")
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
      message(FATAL_ERROR "未找到 gcov，请安装或通过 -DGCOV_EXECUTABLE=/path/to/gcov 指定")
    endif()
    if(NOT EXISTS "${GCOV_EXECUTABLE}")
      message(FATAL_ERROR "GCOV_EXECUTABLE 指向的文件不存在: ${GCOV_EXECUTABLE}")
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

  # 手工列表用于保留覆盖率目标的稳定顺序；实际依赖再从本目录收集全部
  # *_test 目标，避免新增测试忘记加入列表后 ctest 找不到可执行文件。
  set(MSKDSP_TEST_BUILD_TARGETS ${MSKDSP_TEST_TARGETS})
  get_property(MSKDSP_TEST_DIRECTORY_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(MSKDSP_TEST_DIRECTORY_TARGET IN LISTS MSKDSP_TEST_DIRECTORY_TARGETS)
    if(MSKDSP_TEST_DIRECTORY_TARGET MATCHES "_test$")
      list(APPEND MSKDSP_TEST_BUILD_TARGETS "${MSKDSP_TEST_DIRECTORY_TARGET}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES MSKDSP_TEST_BUILD_TARGETS)
  add_dependencies(coverage ${MSKDSP_TEST_BUILD_TARGETS})
endif()
