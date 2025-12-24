include(cmake/version.cmake)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in
    ${CMAKE_CURRENT_SOURCE_DIR}/include/version.h
)

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} MODULEMANAGER_SRCS)
add_library(moduleManager SHARED ${MODULEMANAGER_SRCS})

target_include_directories(moduleManager PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(moduleManager PUBLIC 
    Boost::dll
    dspProto
)
set_target_properties(moduleManager PROPERTIES
    VERSION ${MODULE_MANAGER_VERSION}
    SOVERSION ${MODULE_MANAGER_VERSION_MAJOR}
)