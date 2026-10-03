#!/usr/bin/env bash
# build.sh — cross-compile stereoid.so for the Move (aarch64) and package
# dist/stereoid/ + dist/stereoid-module.tar.gz.
#
# Auto-Dockerizes: if CROSS_PREFIX is unset and we're not already in a
# container, build the toolchain image and re-run inside it.
#
# The compiler is PINNED and the pin is read out of the ARTIFACT: gcc writes
# its version into the .so's .comment section, and a build from any other
# version fails here rather than shipping.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE"

MODULE_ID=stereoid
STEREOID_GCC="${STEREOID_GCC:-12.2.0}"

if [ -z "${CROSS_PREFIX:-}" ] && [ ! -f /.dockerenv ]; then
    echo "==> building in Docker (stereoid-builder)"
    docker build -q -t stereoid-builder -f scripts/Dockerfile scripts >/dev/null
    docker run --rm -v "$HERE":/work -w /work \
        -e CROSS_PREFIX=aarch64-linux-gnu- -e STEREOID_GCC="$STEREOID_GCC" \
        stereoid-builder bash scripts/build.sh
    exit 0
fi

CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
CC="${CROSS_PREFIX}gcc"

echo "==> compiling with $CC"
rm -rf "dist/${MODULE_ID}" "dist/${MODULE_ID}-module.tar.gz"
mkdir -p build "dist/${MODULE_ID}"
# -O3, not -Ofast: the laws are tested as exact arithmetic (a settled delay
# reads the frame itself; the mono sum cancels to the bit), and -ffast-math
# is licence to reorder it.
$CC -O3 -shared -fPIC -march=armv8-a -mtune=cortex-a72 \
    -fomit-frame-pointer -fno-stack-protector -DNDEBUG -std=c11 \
    -Wall -Wextra -Werror \
    src/stereoid_module.c dsp/stereoid.c \
    -o "build/${MODULE_ID}.so" -lm

if ! grep -a -q "GCC: ([^)]*) ${STEREOID_GCC}" "build/${MODULE_ID}.so"; then
    echo "ERROR: build/${MODULE_ID}.so was not built by gcc ${STEREOID_GCC}:" >&2
    grep -a -o 'GCC: ([^)]*) [0-9.]*' "build/${MODULE_ID}.so" | sort -u >&2 || true
    rm -rf "dist/${MODULE_ID}"
    exit 1
fi

echo "==> packaging dist/"
cp "build/${MODULE_ID}.so" "dist/${MODULE_ID}/"
cp src/module.json src/help.json LICENSE "dist/${MODULE_ID}/"

tar -czf "dist/${MODULE_ID}-module.tar.gz" -C dist "${MODULE_ID}"
echo "==> done: dist/${MODULE_ID}-module.tar.gz"
