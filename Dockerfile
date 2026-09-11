FROM --platform=linux/arm64 localhost/arm64v8/ubuntu:noble

WORKDIR /opt/mskdsp

COPY package/MskDSP ./MskDSP
COPY package/module ./module
COPY package/lib ./lib
COPY package/conf ./conf

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
