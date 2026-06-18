#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")"

export OPENJKDF2_RELEASE_COMMIT=$(git log -1 --format="%H")
export OPENJKDF2_RELEASE_COMMIT_SHORT=$(git rev-parse --short=8 HEAD)
export OPENJKDF2_PORTMASTER_BUILD=1

rm -rf build_linux64
mkdir -p build_linux64 && pushd build_linux64

cmake .. \
    -DPLAT_LINUX_64=ON \
    -DCMAKE_BUILD_TYPE=Release

make -j "$(nproc)" ZLIB_Linux_x86_64
make -j "$(nproc)" LIBPNG SDL SDL_mixer GLEW
(make -j "$(nproc)" PROTOBUF || make -j1 PROTOBUF)
make -j "$(nproc)" openjkdf2

popd

echo "Built: build_linux64/openjkdf2"
