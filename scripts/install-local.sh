#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-"$repo_dir/build"}"
prefix="${PREFIX:-"$HOME/.local"}"

"$repo_dir/scripts/build.sh"
cmake --install "$build_dir"
bash "$repo_dir/scripts/configure-overlay.sh"

effect_so="$build_dir/bin/kwin/effects/plugins/stableblur.so"
if [[ ! -f "$effect_so" ]]; then
  effect_so="$(find "$prefix/lib" -path '*/kwin/effects/plugins/stableblur.so' -print -quit)"
fi
effect_json="$prefix/share/kwin-wayland/builtin-effects/stableblur.json"
system_plugin_root="$(qtpaths6 --plugin-dir 2>/dev/null || true)"
if [[ -z "$system_plugin_root" ]]; then
  system_plugin_root="/usr/lib/x86_64-linux-gnu/qt6/plugins"
fi
system_effect_dir="$system_plugin_root/kwin/effects/plugins"

cat <<EOF

Installed the application under:
  $prefix/bin/blur-rectangle

KWin usually loads third-party effects from the system plugin path.
To activate the compositor effect for this session, run:

  sudo install -D -m 755 "$effect_so" "$system_effect_dir/stableblur.so"
  sudo install -D -m 644 "$effect_json" "/usr/share/kwin-wayland/builtin-effects/stableblur.json"
  kwriteconfig6 --file kwinrc --group Plugins --key stableblurEnabled --type bool true
  qdbus6 org.kde.KWin /KWin org.kde.KWin.reconfigure
  qdbus6 org.kde.KWin /Effects org.kde.kwin.Effects.loadEffect stableblur

Then launch:
  $prefix/bin/blur-rectangle
EOF
