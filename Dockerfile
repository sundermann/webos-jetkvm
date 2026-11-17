FROM node:22.21.1-bookworm-slim AS node
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates cmake curl gawk git make ninja-build pkg-config python3 \
    && rm -rf /var/lib/apt/lists/*

ARG TARGETARCH
RUN case "$TARGETARCH" in \
      arm64) sdk_arch=aarch64; sdk_sha=3a9330f3789f8008ece7c8a48f58c3a71d0307a9a1f81896db938b785524c3ce ;; \
      amd64) sdk_arch=x86_64; sdk_sha=2230a6351e5cce6fcb9d4fdcb4697335f223995401ef969c81744aaeb14c5344 ;; \
      *) exit 1 ;; \
    esac \
    && curl -fL "https://github.com/openlgtv/buildroot-nc4/releases/download/2026.08-webos/arm-webos-linux-gnueabi_sdk-buildroot-$sdk_arch.tar.gz" -o /tmp/sdk.tar.gz \
    && echo "$sdk_sha  /tmp/sdk.tar.gz" | sha256sum -c - \
    && mkdir -p /opt/webos-sdk \
    && tar -xzf /tmp/sdk.tar.gz --strip-components=1 -C /opt/webos-sdk \
    && rm /tmp/sdk.tar.gz \
    && /opt/webos-sdk/relocate-sdk.sh

ENV PATH="/opt/webos-sdk/bin:${PATH}" \
    CMAKE_TOOLCHAIN_FILE=/opt/webos-sdk/share/buildroot/toolchainfile.cmake

COPY --from=node /usr/local/bin/node /usr/local/bin/node
COPY --from=node /usr/local/lib/node_modules/npm /usr/local/lib/node_modules/npm
RUN ln -s ../lib/node_modules/npm/bin/npm-cli.js /usr/local/bin/npm

WORKDIR /work
CMD ["sh", "-c", "npm ci --ignore-scripts && npm run build && npm run build:native && npm run package && npm run manifest"]
