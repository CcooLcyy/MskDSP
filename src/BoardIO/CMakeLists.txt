include(cmake/LibInfo.cmake)

configure_file(
    ${CMAKE_SOURCE_DIR}/cmake/LibInfo.h.in
    ${CMAKE_CURRENT_SOURCE_DIR}/include/${LIB_NAME}LibInfo.h
)

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} MODULE_SRCS)
add_library(${LIB_NAME} SHARED ${MODULE_SRCS})
add_dependencies(${LIB_NAME} dspProto)

target_include_directories(${LIB_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${MSKDSP_COMMON_INCLUDE_DIR}
)
target_link_libraries(${LIB_NAME} PUBLIC
    moduleManager
    dspProto
)
set_target_properties(${LIB_NAME} PROPERTIES
    VERSION ${VERSION}
    SOVERSION ${VERSION_MAJOR}
)
