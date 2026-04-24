FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

ARG DEBIAN_FRONTEND=noninteractive

RUN if [ -f /etc/apt/sources.list.d/ubuntu.sources ]; then \
      sed -i 's|http://archive.ubuntu.com/ubuntu/|http://mirrors.tuna.tsinghua.edu.cn/ubuntu/|g; s|http://security.ubuntu.com/ubuntu/|http://mirrors.tuna.tsinghua.edu.cn/ubuntu/|g; s|https://mirrors.tuna.tsinghua.edu.cn/ubuntu/|http://mirrors.tuna.tsinghua.edu.cn/ubuntu/|g' /etc/apt/sources.list.d/ubuntu.sources; \
    fi \
    && if [ -f /etc/apt/sources.list ]; then \
      sed -i 's|http://archive.ubuntu.com/ubuntu/|http://mirrors.tuna.tsinghua.edu.cn/ubuntu/|g; s|http://security.ubuntu.com/ubuntu/|http://mirrors.tuna.tsinghua.edu.cn/ubuntu/|g; s|https://mirrors.tuna.tsinghua.edu.cn/ubuntu/|http://mirrors.tuna.tsinghua.edu.cn/ubuntu/|g' /etc/apt/sources.list; \
    fi \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        binutils-aarch64-linux-gnu \
        build-essential \
        ca-certificates \
        ccache \
        clangd \
        cmake \
        curl \
        file \
        g++-14 \
        g++-aarch64-linux-gnu \
        gcovr \
        gcc-14 \
        gcc-aarch64-linux-gnu \
        gdb \
        git \
        ninja-build \
        patchelf \
        perl \
        pkg-config \
        python3 \
        python3-pip \
        rsync \
        unzip \
        zip \
        linux-libc-dev \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 140 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 140 \
    && update-alternatives --install /usr/bin/cc cc /usr/bin/gcc-14 140 \
    && update-alternatives --install /usr/bin/c++ c++ /usr/bin/g++-14 140 \
    && update-alternatives --set gcc /usr/bin/gcc-14 \
    && update-alternatives --set g++ /usr/bin/g++-14 \
    && update-alternatives --set cc /usr/bin/gcc-14 \
    && update-alternatives --set c++ /usr/bin/g++-14 \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /root/.cache/vcpkg/downloads /root/.cache/vcpkg/archives /root/.cache/ccache

COPY script/devcontainer/init_vcpkg.sh /usr/local/bin/init_vcpkg.sh
RUN chmod +x /usr/local/bin/init_vcpkg.sh
RUN printf '%s\n' 'unset CC' 'unset CXX' > /etc/profile.d/mskdsp-clear-cc.sh \
    && chmod +x /etc/profile.d/mskdsp-clear-cc.sh \
    && printf '\n# 避免基础镜像继承的 CC/CXX 污染交叉编译。\nunset CC\nunset CXX\n' >> /etc/bash.bashrc

ENV MSKDSP_REPO_ROOT=/data/code/mskdsp
ENV VCPKG_ROOT=/data/3rdlibs/vcpkg
ENV VCPKG_DOWNLOADS=/root/.cache/vcpkg/downloads
ENV VCPKG_DEFAULT_BINARY_CACHE=/root/.cache/vcpkg/archives
ENV CCACHE_DIR=/root/.cache/ccache
ENV VCPKG_DISABLE_METRICS=1

WORKDIR /data/code/mskdsp

ENTRYPOINT ["/usr/local/bin/init_vcpkg.sh"]
CMD ["sleep", "infinity"]
