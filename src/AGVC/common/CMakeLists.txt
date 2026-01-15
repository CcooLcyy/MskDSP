add_library(agvcCommon STATIC
  AgvcDataCenterClient.cc
  AgvcStrategy.cc
)

set_target_properties(agvcCommon PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(agvcCommon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(agvcCommon PUBLIC
  dspProto
)
