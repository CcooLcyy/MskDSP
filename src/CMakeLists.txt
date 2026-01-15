add_subdirectory(core)
add_subdirectory(DataCenter)
add_subdirectory(IEC104)
add_subdirectory(DLT645)
add_subdirectory(ModbusRTU)
add_subdirectory(AGVC)
add_subdirectory(ConfigPusher)
add_executable(${PROJECT_NAME} main.cc)
target_link_libraries(${PROJECT_NAME} PUBLIC 
    moduleManager
)
