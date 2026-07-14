#!/usr/bin/env bash
set -euo pipefail
src=/mnt/d/code/srsRAN_4G_23_11
build=$src/build
if [ ! -d "$src" ]; then
  echo "missing source $src" >&2
  exit 1
fi
mkdir -p "$build"
cd "$build"
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_ZEROMQ=ON \
  -DENABLE_UHD=OFF \
  -DENABLE_BLADERF=OFF \
  -DENABLE_SOAPYSDR=OFF \
  -DENABLE_TESTS=OFF
cmake --build . --target srsue -j2
./srsue/src/srsue --version
