#!/usr/bin/env bash
# Launch gwemu (patched Zelda + retro-go dual-boot) on macOS via the
# .app bundle -- counterpart to start.sh (Linux) / start.bat (Windows).
# Run from the repo root; expects firmware images in backup/qemu-images/.
# Hold Game+Left during the first ~2 seconds of boot for retro-go.
#
# GWEMU_APP overrides which bundle to use, e.g.:
#   GWEMU_APP=/Applications/gwemu.app ./start-mac.sh
#   GWEMU_APP=/Volumes/GWemu/gwemu.app ./start-mac.sh   (mounted DMG)
# Default is the locally packaged bundle; falls back to the bare build
# binary if no bundle exists yet.
set -euo pipefail
cd "$(dirname "$0")"

APP="${GWEMU_APP:-dist-macos/gwemu.app}"
if [ -x "$APP/Contents/MacOS/gwemu" ]; then
  BIN="$APP/Contents/MacOS/gwemu"
elif [ -x build/gwemu ]; then
  BIN=build/gwemu
else
  echo "no gwemu.app bundle ($APP) and no build/gwemu -- build or set GWEMU_APP" >&2
  exit 1
fi

exec "$BIN" -M gnw-h7b0 \
  -global gnw-h7b0-soc.bank1-image=backup/qemu-images/zelda-bank1-patched.bin \
  -global gnw-h7b0-soc.bank2-image=backup/qemu-images/retro-go-bank2.bin \
  -global gnw-h7b0-soc.extflash-image=backup/qemu-images/zelda-extflash-patched-plus-retro-go.bin \
  -audiodev sdl3,id=snd0 -global gnw-h7b0-sai1.audiodev=snd0 \
  -display gwemu "$@"
