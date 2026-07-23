#!/usr/bin/env bash
# Launch gwemu (patched Zelda + retro-go dual-boot) on Linux.
# Counterpart to start.bat: run from the repo root; expects the native
# binary in build/ and firmware images in backup/qemu-images/.
# Hold Game+Left during the first ~2 seconds of boot for retro-go.
#
# Perf-probe env vars are ON here (harmless, log-only) so the resulting
# gwemu.log directly yields the numbers used in the JPEG-MMIO throughput
# investigation (see docs/CHANGELOG):
#   JPGRD lines  = JPEG register reads/s (per-offset breakdown)
#   LTC cap vbr  = guest frame completions (virtual-clock timestamps)
#   VBL/DMA late = pacing-timer dispatch lateness
set -euo pipefail
cd "$(dirname "$0")"

export GNW_MMIO_RATE=1
export GNW_LTDC_TRACE=1
export GNW_TIMER_LATE=1

# Mirror the Windows build's no-console behavior: traces land in
# gwemu.log next to this script (fresh per run), while still showing
# on the terminal.
exec 2> >(tee gwemu.log >&2)

./build/qemu-system-arm -M gnw-h7b0 \
  -global gnw-h7b0-soc.bank1-image=backup/qemu-images/zelda-bank1-patched.bin \
  -global gnw-h7b0-soc.bank2-image=backup/qemu-images/retro-go-bank2.bin \
  -global gnw-h7b0-soc.extflash-image=backup/qemu-images/zelda-extflash-patched-plus-retro-go.bin \
  -drive if=sd,file=backup/qemu-images/sdcard-overlay.qcow2 \
  -audiodev sdl3,id=snd0 -global gnw-h7b0-sai1.audiodev=snd0 \
  -display gwemu "$@"
