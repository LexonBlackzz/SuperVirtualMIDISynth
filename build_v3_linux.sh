#!/usr/bin/env sh
set -eu

cmake -S src/V3 -B build/V3-linux -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DSVMS_BUILD_TESTS=ON
cmake --build build/V3-linux --parallel
ctest --test-dir build/V3-linux --output-on-failure
