#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build/ai}"

ctest --test-dir "$BUILD_DIR" \
  --output-on-failure \
  -R "cu_cp|cucp|rrc|f1ap|ngap|e1ap|mobility|measurement|satellite"
