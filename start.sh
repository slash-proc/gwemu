#!/usr/bin/env bash
# Launch gwemu (patched Zelda + retro-go dual-boot) on Linux.
# Counterpart to start.bat: run from the repo root; expects the native
# binary in build/ and firmware images in backup/qemu-images/.
# Hold Game+Left during the first ~2 seconds of boot for retro-go.
#
# Perf-probe env vars (GNW_MMIO_RATE / GNW_LTDC_TRACE / GNW_TIMER_LATE)
# are opt-in: export them =1 before launching when you need the trace
# numbers (JPEG register read rates, frame completions, pacing-timer
# lateness). Unset/empty/0 all mean off.
set -euo pipefail
cd "$(dirname "$0")"

# Mirror the Windows build's no-console behavior: traces land in
# gwemu.log next to this script (fresh per run), while still showing
# on the terminal.
exec 2> >(tee gwemu.log >&2)

#./build/qemu-system-arm -M gnw-h7b0 \

build/qemu-system-arm -M gnw-h7b0 \
  -global gnw-h7b0-soc.bank1-image=backup/qemu-images/zelda-bank1-patched.bin \
  -global gnw-h7b0-soc.bank2-image=backup/qemu-images/retro-go-bank2.bin \
  -global gnw-h7b0-soc.extflash-image=backup/qemu-images/zelda-extflash-patched-plus-retro-go.bin \
  -drive if=sd,file=backup/qemu-images/sdcard-overlay.qcow2 \
  -audiodev sdl3,id=snd0 -global gnw-h7b0-sai1.audiodev=snd0 \
  -display gwemu "$@"
