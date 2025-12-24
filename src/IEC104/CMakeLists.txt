aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} IEC104_SRCS)

add_library(iec104 SHARED ${IEC104_SRCS})

target_include_directories(iec104 PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(iec104 PUBLIC
    moduleManager
)