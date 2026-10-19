# 使用与平台相同的固定 BuildKit 客户端，不需要宿主机 Docker socket。
FROM docker.m.daocloud.io/moby/buildkit@sha256:65eca3c01cd1180c8f64565c4e3c5983af5145818b0e6fa863385d099eb73855 AS buildkit
FROM mskdsp-platform-ci-tools:bookworm-v2
COPY --from=buildkit /usr/bin/buildctl /usr/local/bin/buildctl
RUN buildctl --version && python3 --version && bash --version >/dev/null
