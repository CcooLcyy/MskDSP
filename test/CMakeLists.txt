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
