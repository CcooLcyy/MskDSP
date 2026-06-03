include(cmake/LibInfo.cmake)

configure_file(
    ${CMAKE_SOURCE_DIR}/cmake/LibInfo.h.in
    ${CMAKE_CURRENT_SOURCE_DIR}/include/${LIB_NAME}LibInfo.h
)

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} MODULEMANAGER_SRCS)
add_library(${LIB_NAME} SHARED ${MODULEMANAGER_SRCS})

target_include_directories(${LIB_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(${LIB_NAME} PUBLIC 
    Boost::dll
    Boost::iostreams
    Boost::json
    Boost::log
    ZLIB::ZLIB
    dspProto
    mskdspConfigStore
)
set_target_properties(${LIB_NAME} PROPERTIES
    VERSION ${VERSION}
    SOVERSION ${VERSION_MAJOR}
    LIBRARY_OUTPUT_DIRECTORY ${libPath}
)
