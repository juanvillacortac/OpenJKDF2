#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")"

export OPENJKDF2_RELEASE_COMMIT=$(git log -1 --format="%H")
export OPENJKDF2_RELEASE_COMMIT_SHORT=$(git rev-parse --short=8 HEAD)

rm -rf build_linux64_gles
mkdir -p build_linux64_gles && pushd build_linux64_gles

cmake .. \
    -DPLAT_LINUX_X86_64_GLES=ON \
    -DCMAKE_BUILD_TYPE=Release

make -j "$(nproc)" ZLIB_Linux_x86_64
make -j "$(nproc)" LIBPNG SDL SDL_mixer
make -j "$(nproc)" openjkdf2

popd

echo "Built: build_linux64_gles/openjkdf2 (GLES desktop experiment)"
