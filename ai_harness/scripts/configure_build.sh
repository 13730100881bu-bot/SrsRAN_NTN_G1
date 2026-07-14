#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build/ai}"

cmake -S . -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=On
