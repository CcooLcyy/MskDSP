add_subdirectory(core)
add_subdirectory(DataCenter)
add_subdirectory(IEC104)

add_subdirectory(DLT645)
add_executable(${PROJECT_NAME} main.cc)
target_link_libraries(${PROJECT_NAME} PUBLIC 
    moduleManager
)
