# debian:bookworm carries aarch64 gcc 12.2.0, which is what build.sh pins
# (STEREOID_GCC). Ubuntu 22.04 ships 11.4.0: same source, a different binary.
FROM debian:bookworm
RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu libc6-dev-arm64-cross make \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
