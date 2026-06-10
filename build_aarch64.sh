#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")"

export OPENJKDF2_RELEASE_COMMIT=$(git log -1 --format="%H")
export OPENJKDF2_RELEASE_COMMIT_SHORT=$(git rev-parse --short=8 HEAD)

rm -rf build_aarch64
mkdir -p build_aarch64 && pushd build_aarch64

cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=../cmake_modules/toolchain_linux_aarch64.cmake \
    -DPLAT_LINUX_AARCH64=ON \
    -DCMAKE_BUILD_TYPE=Release

# Dependencias primero (evita carreras en cross-compile)
make -j "$(nproc)" ZLIB_Linux_aarch64
make -j "$(nproc)" LIBPNG SDL SDL_mixer OPENAL
make -j "$(nproc)" openjkdf2

popd

echo "Built: build_aarch64/openjkdf2"
