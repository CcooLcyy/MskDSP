cmake_minimum_required(VERSION 3.31)

project(MskDSP)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(gRPC CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS asio dll)

set(packPath ${CMAKE_SOURCE_DIR}/package)
set(libPath ${packPath}/lib)
set(configPath ${packPath}/conf)
file(MAKE_DIRECTORY ${packPath} ${libPath} ${configPath})

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${packPath})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${libPath})

add_subdirectory(3rdlibs)
add_subdirectory(protobuf)
add_subdirectory(src)
add_subdirectory(test)