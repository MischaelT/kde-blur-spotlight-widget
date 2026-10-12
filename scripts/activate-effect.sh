#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
plugin_dir="$(qtpaths6 --plugin-dir)/kwin/effects/plugins"
source_plugin="$repo_dir/build/bin/kwin/effects/plugins/stableblur.so"
test -f "$source_plugin"
# Replace atomically, without modifying the file mapped by the running KWin.
sudo install -D -m 755 "$source_plugin" "$plugin_dir/stableblur.so.new"
sudo mv -f "$plugin_dir/stableblur.so.new" "$plugin_dir/stableblur.so"
sudo install -D -m 644 "$repo_dir/tiny-blur-effect/metadata.json" /usr/share/kwin-wayland/builtin-effects/stableblur.json
qdbus6 org.kde.KWin /Effects org.kde.kwin.Effects.unloadEffect stableblur
qdbus6 org.kde.KWin /Effects org.kde.kwin.Effects.loadEffect stableblur
revision="$(qdbus6 org.kde.KWin /org/local/StableBlur org.local.StableBlur.revision 2>/dev/null || true)"
if [[ "$revision" != 'clear-zoom-popup-3' ]]; then
    printf '%s\n' \
        'The updated effect is installed, but this session is not running the expected version.' \
        'KWin may retain the old plugin library after unloading the effect.' \
        'Save your work, log out of Plasma, and log back in. Then launch Blur Spotlight.' \
        'If zoom remains unavailable after login, report this output for further diagnosis.'
    exit 1
fi
printf '%s\n' 'Stable Blur loaded; the zoom API is available.'
