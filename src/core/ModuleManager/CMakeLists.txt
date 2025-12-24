include(cmake/LibInfo.cmake)
set(MODULE_MANAGER_LIB_NAME moduleManager)

configure_file(
    ${CMAKE_SOURCE_DIR}/cmake/LibInfo.h.in
    ${CMAKE_CURRENT_SOURCE_DIR}/include/${MODULE_MANAGER_LIB_NAME}LibInfo.h
)

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} MODULEMANAGER_SRCS)
add_library(${MODULE_MANAGER_LIB_NAME} SHARED ${MODULEMANAGER_SRCS})

target_include_directories(${MODULE_MANAGER_LIB_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(${MODULE_MANAGER_LIB_NAME} PUBLIC 
    Boost::dll
    dspProto
)
set_target_properties(${MODULE_MANAGER_LIB_NAME} PROPERTIES
    VERSION ${MODULE_MANAGER_VERSION}
    SOVERSION ${MODULE_MANAGER_VERSION_MAJOR}
)