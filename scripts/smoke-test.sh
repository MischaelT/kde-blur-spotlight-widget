#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-"$repo_dir/build-smoke"}"

cmake -S "$repo_dir" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"

cmake --build "$build_dir" -j"${JOBS:-$(nproc)}"

QT_QPA_PLATFORM=offscreen "$build_dir/blur-rectangle/blur-rectangle" --self-test
test -f "$build_dir/bin/kwin/effects/plugins/stableblur.so" \
  || test -f "$build_dir/tiny-blur-effect/bin/kwin/effects/plugins/stableblur.so"
grep -q '"Name": "Stable Blur Spotlight"' "$repo_dir/tiny-blur-effect/metadata.json"

echo "Smoke test passed: app starts offscreen and stableblur effect builds."
