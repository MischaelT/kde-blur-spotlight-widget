#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-"$repo_dir/build"}"
prefix="${PREFIX:-"$HOME/.local"}"
jobs="${JOBS:-$(nproc)}"

cmake -S "$repo_dir" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}" \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  "$@"

cmake --build "$build_dir" -j"$jobs"
