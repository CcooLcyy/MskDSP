# 在 x64 主机运行 GCC14，生成 ARM64 二进制；不依赖目标架构编译器的 QEMU 执行。
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc-14)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++-14)
set(CMAKE_ASM_COMPILER aarch64-linux-gnu-gcc-14)
set(CMAKE_AR aarch64-linux-gnu-ar)
set(CMAKE_RANLIB aarch64-linux-gnu-ranlib)
set(CMAKE_NM aarch64-linux-gnu-nm)
set(CMAKE_LINKER aarch64-linux-gnu-ld)
set(CMAKE_STRIP aarch64-linux-gnu-strip)
set(CMAKE_OBJCOPY aarch64-linux-gnu-objcopy)
set(CMAKE_OBJDUMP aarch64-linux-gnu-objdump)

# 沿用固定 baseline 的 Linux 默认编译参数及 vcpkg 端口附加参数。
if(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/toolchains/linux.cmake")
  include("$ENV{VCPKG_ROOT}/scripts/toolchains/linux.cmake")
endif()

# 工具在主机执行，头文件和库仅从目标 sysroot/vcpkg 目标目录查找。
list(APPEND CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
