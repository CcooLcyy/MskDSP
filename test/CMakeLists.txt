enable_testing()

find_package(GTest CONFIG REQUIRED)

add_executable(dataCenterCore_test
  data_center_core_test.cc
  ${CMAKE_SOURCE_DIR}/src/DataCenter/DataCenterCore.cc
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
