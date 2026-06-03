add_library(mskdspConfigStore STATIC
  ConfigDatabase.cc
)

set_target_properties(mskdspConfigStore PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(mskdspConfigStore PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(mskdspConfigStore PUBLIC
  gRPC::grpc++
  unofficial::sqlite3::sqlite3
)
