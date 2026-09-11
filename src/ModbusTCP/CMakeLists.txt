include(cmake/LibInfo.cmake)

configure_file(
    ${CMAKE_SOURCE_DIR}/cmake/LibInfo.h.in
    ${CMAKE_CURRENT_SOURCE_DIR}/include/${LIB_NAME}LibInfo.h
)

add_library(${LIB_NAME} SHARED
    ModbusTCP.cc
    ModbusTCPGrpcService.cc
    ModbusTCPLinkManager.cc
    ModbusTCPLinkStore.cc
    ModbusTCPPointTableStore.cc
    ModbusTCPTcpBus.cc
    ${CMAKE_SOURCE_DIR}/src/ModbusRTU/ModbusRTUDataCenterClient.cc
    ${CMAKE_SOURCE_DIR}/src/ModbusRTU/ModbusRTUPointTable.cc
)

target_include_directories(${LIB_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/ModbusRTU/include
    ${MSKDSP_COMMON_INCLUDE_DIR}
)
target_link_libraries(${LIB_NAME} PUBLIC
    moduleManager
    dspProto
    Boost::asio
    Boost::json
)
set_target_properties(${LIB_NAME} PROPERTIES
    VERSION ${VERSION}
    SOVERSION ${VERSION_MAJOR}
)
