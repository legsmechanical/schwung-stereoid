#!/usr/bin/env bash
# run.sh — native build + the offline suite. No device needed.
#
# Local green is not green: macOS headers hide what glibc under -std=c11 does
# not declare. Before a release, run it where the release builds:
#   docker run --rm -v "$PWD":/work -w /work debian:bookworm bash -c \
#     'apt-get -qq update >/dev/null && apt-get -qq install -y gcc nodejs >/dev/null; tests/run.sh'
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE"
rm -rf dist/tests
mkdir -p dist/tests
CC="${CC:-cc}"
echo "==> building the suite with $CC"
$CC -O2 -std=c11 -Wall -Wextra -Werror \
    tests/test_stereoid.c src/stereoid_module.c dsp/stereoid.c \
    -o dist/tests/test_stereoid -lm
echo "==> running"
./dist/tests/test_stereoid
if command -v node >/dev/null 2>&1; then
    node tools/pages_check.mjs
else
    echo "pages_check: no node — skipped"
fi
