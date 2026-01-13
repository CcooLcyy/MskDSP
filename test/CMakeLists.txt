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
