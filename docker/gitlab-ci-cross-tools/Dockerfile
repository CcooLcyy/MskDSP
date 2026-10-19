# 基于既有 x64 工具镜像添加 Ubuntu 官方 GCC14 ARM64 交叉工具链。
FROM mskdsp-ci-tools:ubuntu24.04-x64

ARG DEBIAN_FRONTEND=noninteractive
ARG CROSS_GCC_VERSION=14.2.0-4ubuntu2~24.04.1cross1

RUN test "$(dpkg --print-architecture)" = amd64 \
    && apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30 update \
    && apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30 install -y --no-install-recommends \
        "gcc-14-aarch64-linux-gnu=${CROSS_GCC_VERSION}" \
        "g++-14-aarch64-linux-gnu=${CROSS_GCC_VERSION}" \
        binutils-aarch64-linux-gnu \
    && rm -rf /var/lib/apt/lists/*

# 构建时验证两套编译器与链接器；只执行 x64 程序，不在编译镜像中执行 ARM64 程序。
RUN gcc-14 --version \
    && g++-14 --version \
    && aarch64-linux-gnu-gcc-14 --version \
    && aarch64-linux-gnu-g++-14 --version \
    && test "$(gcc-14 -dumpmachine)" = x86_64-linux-gnu \
    && test "$(g++-14 -dumpmachine)" = x86_64-linux-gnu \
    && test "$(aarch64-linux-gnu-gcc-14 -dumpmachine)" = aarch64-linux-gnu \
    && test "$(aarch64-linux-gnu-g++-14 -dumpmachine)" = aarch64-linux-gnu \
    && test "$(aarch64-linux-gnu-gcc-14 -dumpfullversion)" = 14.2.0 \
    && aarch64-linux-gnu-objcopy --version \
    && aarch64-linux-gnu-strip --version \
    && printf '#include <iostream>\nint main(){std::cout << "ok";return 0;}\n' > /tmp/cross-smoke.cc \
    && g++-14 /tmp/cross-smoke.cc -o /tmp/native-smoke \
    && aarch64-linux-gnu-g++-14 /tmp/cross-smoke.cc -o /tmp/arm64-smoke \
    && test "$(/tmp/native-smoke)" = ok \
    && readelf -h /tmp/native-smoke | grep -q 'Advanced Micro Devices X86-64' \
    && aarch64-linux-gnu-readelf -h /tmp/arm64-smoke | grep -q 'AArch64' \
    && rm -f /tmp/cross-smoke.cc /tmp/native-smoke /tmp/arm64-smoke
