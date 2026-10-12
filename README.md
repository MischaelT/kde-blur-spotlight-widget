# Blur Rectangle

Blur Rectangle is a small KDE Plasma 6 / KWin Wayland utility for placing a real compositor blur mask over part of the screen. It launches directly into a fullscreen inverted spotlight mode: draggable apertures are blurred while the rest of the screen remains clear.

## Compatibility

Blur Rectangle targets KDE Plasma 6 on Wayland only. The only tested version is **Plasma 6.7.5** (KWin 6.7.5), the version installed on the author's machine. The bundled `stableblur` KWin effect is built against KWin private headers, so it is tied to the installed KWin version and may fail to build or load on other Plasma releases.

## Features

- Fullscreen inverted spotlight mode by default; the clear part of the overlay passes pointer input through to other apps and never takes keyboard focus.
- Multiple spotlights (up to 16), each independently draggable and resizable. Extra spotlights last for the current session.
- Circle or rectangle aperture shape and blur inversion, applied to all spotlights.
- Per-spotlight zoom (Off, 1.15x, 1.25x, 1.5x, 2x, 3x) in clear-spotlight mode, sampling the unblurred live background.
- Optional floating always-on-top blur rectangle mode.
- Real KWin compositor blur via `KWindowEffects::enableBlurBehind()` plus the bundled `stableblur` KWin effect.
- Live blur strength control with the keyboard.
- Tray menu and right-click menu.
- Settings persisted with `QSettings`.

## Controls

General:

- `Esc`: close. Other unassigned keys, including Space, do nothing.
- `S`: switch between fullscreen spotlight mode and blur rectangle mode.
- Top-right buttons: minimize, switch mode, close.
- Right click: open menu.
- `+` / `-` adjust blur strength; `1`-`9` and `0` select levels 1-10.

Spotlight mode:

- Bottom-left buttons: add a spotlight (+), choose circle or rectangle shape, and invert the blur (also `I`) so only the aperture is blurred and the rest stays clear.
- Drag inside the clear aperture to move it. Drag the circle border to resize a circle; drag the rectangle border or corners to resize a rectangle. `Alt` + `+` / `Alt` + `-` resizes the current shape from the keyboard.
- Right-click a spotlight to delete it or choose its zoom level. Deleting the last spotlight closes the overlay.

Blur rectangle mode:

- Double-click the top drag area to enter fullscreen spotlight mode (same as `S`).
- `Ctrl+T`: toggle click-through.

## Dependencies

On KDE neon / Ubuntu Noble based systems:

```bash
sudo apt install \
  build-essential cmake extra-cmake-modules \
  qt6-base-dev libkf6windowsystem-dev libkf6coreaddons-dev libkf6config-dev \
  kwin-dev libepoxy-dev
```

Package names vary by distro. The KWin effect uses KWin private headers; if CMake cannot find them, pass:

```bash
cmake -S . -B build -DKWIN_PRIVATE_INCLUDE_DIR=/path/to/include/kwin
```

## Build

```bash
./scripts/build.sh
```

Or manually:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build -j"$(nproc)"
```

## Smoke Test

```bash
./scripts/smoke-test.sh
```

This verifies that the app starts with Qt's offscreen platform and that the KWin effect builds. Real compositor blur must still be verified inside a KDE Plasma Wayland session.

## Install

```bash
./scripts/install-local.sh
```

The app installs into `~/.local/bin/blur-rectangle`. The installer also adds a per-user KWin rule that places Blur Rectangle in the Overlay layer, above fullscreen video without taking keyboard focus. To apply the rule to an existing installation, run `bash scripts/configure-overlay.sh`. To undo it, remove the "Blur Rectangle overlay" rule in System Settings > Window Management > Window Rules.

KWin on Plasma 6.7 typically loads binary effects from the system plugin directory, so the script prints the two `sudo install` commands needed to copy `stableblur.so` and `stableblur.json` into KWin's system paths. Alternatively, after building, run `bash scripts/activate-effect.sh` to install and reload the effect (uses `sudo`). Zoom requires the current version of the effect; if KWin keeps the old plugin loaded, log out and back in.

If you install manually, use your Qt plugin root:

```bash
QT_PLUGIN_DIR="$(qtpaths6 --plugin-dir)"
sudo install -D -m 755 build/bin/kwin/effects/plugins/stableblur.so \
  "$QT_PLUGIN_DIR/kwin/effects/plugins/stableblur.so"
sudo install -D -m 644 ~/.local/share/kwin-wayland/builtin-effects/stableblur.json \
  /usr/share/kwin-wayland/builtin-effects/stableblur.json
```

## Live Verification

After installing the effect into the system KWin plugin paths:

```bash
kwriteconfig6 --file kwinrc --group Plugins --key stableblurEnabled --type bool true
qdbus6 org.kde.KWin /KWin org.kde.KWin.reconfigure
qdbus6 org.kde.KWin /Effects org.kde.kwin.Effects.loadEffect stableblur
qdbus6 org.kde.KWin /Effects org.kde.kwin.Effects.isEffectLoaded stableblur
```

The final command should print `true`.

Launch:

```bash
~/.local/bin/blur-rectangle
```

Confirm it opens fullscreen with a blurred circular aperture and a clear surrounding screen. Use the invert button or `I` to swap which region is blurred, switch between the circle and rectangle buttons, drag and resize the openings, and click the top-right close button. Press `S` to verify the optional blur rectangle mode.

## Uninstall

```bash
rm -f ~/.local/bin/blur-rectangle
rm -f ~/.local/share/applications/org.local.blurrectangle.desktop
rm -f ~/.local/share/kwin-wayland/builtin-effects/stableblur.json
find ~/.local/lib -path '*/kwin/effects/plugins/stableblur.so' -delete

QT_PLUGIN_DIR="$(qtpaths6 --plugin-dir)"
sudo rm -f "$QT_PLUGIN_DIR/kwin/effects/plugins/stableblur.so"
sudo rm -f /usr/share/kwin-wayland/builtin-effects/stableblur.json

kwriteconfig6 --file kwinrc --group Plugins --key stableblurEnabled --type bool false
qdbus6 org.kde.KWin /KWin org.kde.KWin.reconfigure
```
