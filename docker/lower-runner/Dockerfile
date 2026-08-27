FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG RUNNER_VERSION=2.321.0
ARG RUNNER_DOWNLOAD_BASE_URL=https://github.com

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        gnupg \
    && install --mode 0755 --directory /etc/apt/keyrings \
    && curl --fail --location https://download.docker.com/linux/ubuntu/gpg \
      --output /etc/apt/keyrings/docker.asc \
    && chmod a+r /etc/apt/keyrings/docker.asc \
    && printf '%s\n' \
      "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/ubuntu noble stable" \
      > /etc/apt/sources.list.d/docker.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        binutils-aarch64-linux-gnu \
        build-essential \
        ccache \
        cmake \
        docker-buildx-plugin \
        docker-ce-cli \
        g++-14 \
        g++-aarch64-linux-gnu \
        gcc-14 \
        gcc-aarch64-linux-gnu \
        gh \
        git \
        gosu \
        jq \
        ninja-build \
        openssh-client \
        pkg-config \
        python3 \
        python3-pip \
        sudo \
        tar \
        tini \
        unzip \
        zip \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 140 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 140 \
    && update-alternatives --set gcc /usr/bin/gcc-14 \
    && update-alternatives --set g++ /usr/bin/g++-14 \
    && rm -rf /var/lib/apt/lists/*

RUN test "$(dpkg --print-architecture)" = "amd64" \
    && useradd --create-home --shell /bin/bash runner \
    && echo 'runner ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/runner \
    && chmod 0440 /etc/sudoers.d/runner \
    && mkdir --parents /opt/actions-runner \
    && curl --fail --location --retry 5 --retry-all-errors \
      "${RUNNER_DOWNLOAD_BASE_URL%/}/actions/runner/releases/download/v${RUNNER_VERSION}/actions-runner-linux-x64-${RUNNER_VERSION}.tar.gz" \
      --output /tmp/actions-runner.tar.gz \
    && tar --extract --gzip --file /tmp/actions-runner.tar.gz --directory /opt/actions-runner \
    && rm /tmp/actions-runner.tar.gz \
    && chown --recursive runner:runner /opt/actions-runner

COPY entrypoint.sh /usr/local/bin/lower-runner-entrypoint
RUN chmod 0755 /usr/local/bin/lower-runner-entrypoint

ENV RUNNER_HOME=/opt/actions-runner
ENV RUNNER_ALLOW_RUNASROOT=0

WORKDIR /opt/actions-runner
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/lower-runner-entrypoint"]
