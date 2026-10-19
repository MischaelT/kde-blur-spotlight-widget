# Blur Spotlight

Blur Spotlight is a small KDE Plasma 6 / KWin Wayland utility for placing a real compositor blur mask over part of the screen. It launches directly into a fullscreen inverted spotlight mode: draggable apertures are blurred while the rest of the screen remains clear.

## Compatibility

Blur Spotlight targets KDE Plasma 6 on Wayland only. The only tested version is **Plasma 6.7.5** (KWin 6.7.5), the version installed on the author's machine. The bundled `stableblur` KWin effect is built against KWin private headers, so it is tied to the installed KWin version and may fail to build or load on other Plasma releases.

## Features

- Fullscreen inverted spotlight mode by default; the clear part of the overlay passes pointer input through to other apps and never takes keyboard focus.
- Multiple spotlights (up to 3), each independently draggable and resizable. Extra spotlights last for the current session.
- Circle or rectangle aperture shape and blur inversion, applied to all spotlights.
- Per-spotlight zoom (Off, 1.15x, 1.25x, 1.5x, 2x, 3x) in clear-spotlight mode, sampling the unblurred live background.
- Track object: a spotlight can follow a region of the screen (OpenCV CSRT tracker on frames from KWin's ScreenShot2 D-Bus API).
- Optional floating always-on-top blur rectangle mode.
- Real KWin compositor blur via `KWindowEffects::enableBlurBehind()` plus the bundled `stableblur` KWin effect.
- Live blur strength control with the keyboard.
- Tray icon menu (show/hide, spotlight mode, click-through, quit) and a right-click menu on the overlay.
- Settings persisted with `QSettings`.

## Controls

General:

- `Esc`: close. Other unassigned keys, including Space, do nothing.
- Right click: open the menu. Every menu item has a key, shown next to it in the menu and listed below.
- `S`: switch between fullscreen spotlight mode and blur rectangle mode (menu: "Spotlight blocker mode" when in blur rectangle mode).
- `H`: hide (menu: "Hide").
- `Ctrl+Q`: quit (menu: "Quit").
- `+` / `-` adjust blur strength; `1`-`9` and `0` select levels 1-10.

Spotlight mode:

- There are no on-screen buttons; use the right-click menu or its keys:

  | Menu item | Key |
  | --- | --- |
  | New spotlight | `N` |
  | Invert (blur only the aperture, rest stays clear) | `I` |
  | Shape > Circle / Rectangle | `C` / `R` |
  | Shape > Freeform: drag to draw an outline; the closed outline becomes the aperture of the current spotlight, placed where you drew it, and is scaled into every spotlight's box. `Esc` cancels. No zoom for freeform; saved with the other settings | `F` |
  | Track object / Stop tracking | `T` |
  | Autotrack (while a spotlight follows the mouse or is being dragged (also after you let go), start tracking after 3 s of standstill, shown as one to three dots on the spotlight; the object under the spotlight when the pointer settled is memorized and followed, not whatever is there when the 3 s end; saved setting) | `A` |
  | Autosize (while tracking, the spotlight grows and shrinks with the object, within 20% of the size you set and never beyond the screen; the search for a lost object also tries 80%-120% of its size; saved setting) | `W` |
  | Zoom (cycles through the zoom levels, needs a clear, non-inverted spotlight) | `Z` |
  | Delete spotlight | `Delete` |

- Drag inside the clear aperture to move it. Drag the circle border to resize a circle; drag the rectangle border or corners to resize a rectangle. `Alt` + `+` / `Alt` + `-` resizes the current shape from the keyboard.
- Double-click inside a spotlight to make it stick to the mouse and follow it everywhere on screen. While it follows, only the spotlight itself captures the pointer (plus a wider area while the pointer is moving, so a fast move does not lose it), so other windows stay usable around it; double-click again or press `Esc` to drop it in place. Right-click works while it follows.
- Right-click a spotlight and choose "Track object" to make it follow whatever is under it. It stops on `Esc`, `T`, or when you drag or resize that spotlight; if the target is lost it keeps searching until you stop it. Each spotlight can track its own object (all trackers on a screen share one screen capture loop); only the position follows, the size stays unless Autosize is on. Pressing `Esc` stops all trackers. Needs the app installed with `scripts/install-local.sh` so KWin allows screenshots (see the `X-KDE-DBUS-Restricted-Interfaces` line in the desktop file). Blur inside the aperture is part of the captured picture, so tracking is more reliable when the target stays in the clear area.
- Right-click a spotlight to delete it or choose its zoom level. Deleting the last spotlight closes the overlay.
- The object you chose is remembered and never replaced. If it is lost, the tracker follows the closest look-alike on screen instead, and keeps looking for the chosen object (every few frames and whenever the whole picture changes a lot); it switches back as soon as the chosen one is found again. While following the chosen object it also saves a few later looks of it (other lighting or angle) next to the original, which is never replaced, and matches against all of them. Matching tolerates exposure changes (it compares colors and edges), searches half-size first and then refines at full size, and searches less often the longer the object stays lost.

Blur rectangle mode:

- Double-click the top drag area to enter fullscreen spotlight mode (same as `S`).
- `Ctrl+T`: toggle click-through (menu: "Click-through").

## Dependencies

On KDE neon / Ubuntu Noble based systems:

```bash
sudo apt install \
  build-essential cmake extra-cmake-modules \
  qt6-base-dev libkf6windowsystem-dev libkf6coreaddons-dev libkf6config-dev \
  kwin-dev libepoxy-dev libopencv-dev libopencv-contrib-dev
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

The object matching used by the tracker (`blur-rectangle/src/matcher.cpp`) has unit tests on synthetic frames (moved, resized and re-exposed objects, saved views, switching decisions). The smoke test runs them; or run `build/blur-rectangle/matcher-test` after a build.

## Install

```bash
./scripts/install-local.sh
```

The app installs into `~/.local/bin/blur-rectangle`. The installer also adds a per-user KWin rule that places Blur Spotlight in the Overlay layer, above fullscreen video without taking keyboard focus. To apply the rule to an existing installation, run `bash scripts/configure-overlay.sh`. To undo it, remove the "Blur Spotlight overlay" rule in System Settings > Window Management > Window Rules.

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

Confirm it opens fullscreen with a blurred circular aperture and a clear surrounding screen. Use `I` or the right-click menu to swap which region is blurred, switch between circle (`C`) and rectangle (`R`), add a spotlight (`N`), drag and resize the openings, and quit with `Ctrl+Q`. Press `S` to verify the optional blur rectangle mode.

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
