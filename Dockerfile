FROM --platform=linux/arm64 localhost/arm64v8/ubuntu:noble

WORKDIR /opt/mskdsp

COPY package/MskDSP ./MskDSP
COPY package/module ./module
COPY package/lib ./lib
COPY package/conf ./conf

ENV LD_LIBRARY_PATH=/opt/mskdsp/lib

ENTRYPOINT ["./MskDSP"]
