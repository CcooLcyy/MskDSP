add_subdirectory(core)

add_executable(${PROJECT_NAME} main.cc)
target_link_libraries(${PROJECT_NAME} PUBLIC 
    ModuleManager    
)