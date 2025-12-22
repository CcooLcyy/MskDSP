aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} MODULEMANAGER_SRC)

add_library(ModuleManager SHARED ${MODULEMANAGER_SRC})

target_include_directories(ModuleManager PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ModuleManager PUBLIC 
    Boost::dll
)