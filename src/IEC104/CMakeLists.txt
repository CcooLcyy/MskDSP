include(cmake/LibInfo.cmake)

configure_file(
    ${CMAKE_SOURCE_DIR}/cmake/LibInfo.h.in
    ${CMAKE_CURRENT_SOURCE_DIR}/include/${LIB_NAME}LibInfo.h
)

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} IEC104_SRCS)
add_library(${LIB_NAME} SHARED ${IEC104_SRCS})

target_include_directories(${LIB_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${MSKDSP_COMMON_INCLUDE_DIR}
)
target_link_libraries(${LIB_NAME} PUBLIC 
    moduleManager
    Boost::asio
)
set_target_properties(${LIB_NAME} PROPERTIES
    VERSION ${VERSION}
    SOVERSION ${VERSION_MAJOR}
)
