FROM --platform=linux/arm64 localhost/arm64v8/ubuntu:noble

WORKDIR /opt/mskdsp

COPY package/MskDSP ./MskDSP
COPY package/module ./module
COPY package/lib ./lib
COPY package/conf ./conf
COPY script/network/ordinary_network.py ./script/network/ordinary_network.py

# 普通外网出口使用参数化运行脚本及宿主机 resolved 客户端；不在镜像中启动服务。
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends python3 iproute2 systemd-resolved && \
    rm -rf /var/lib/apt/lists/*

# IEC104 CP56Time2a 使用本地日历时间；发布容器统一固定为北京时间（UTC+8），
# 不随设备宿主机时区变化；若基础镜像缺少 tzdata，则在构建阶段补齐。
ENV TZ=Asia/Shanghai
RUN if [ ! -e /usr/share/zoneinfo/Asia/Shanghai ]; then \
      apt-get update && \
      DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends tzdata && \
      rm -rf /var/lib/apt/lists/*; \
    fi && \
    ln -snf /usr/share/zoneinfo/Asia/Shanghai /etc/localtime && \
    echo 'Asia/Shanghai' > /etc/timezone

ENV LD_LIBRARY_PATH=/opt/mskdsp/lib

ENTRYPOINT ["./MskDSP"]
