cmake_minimum_required(VERSION 3.31)

project(MskDSP)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(3rdlibs)
add_subdirectory(src)
add_subdirectory(test)