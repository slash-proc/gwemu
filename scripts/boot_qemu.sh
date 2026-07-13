#!/usr/bin/env bash
# boot_qemu.sh -- standardized QEMU launch for a stock G&W game, using the
# standardized bank1/bank2/extflash images from
# backup/qemu-images/<game>-{bank1,bank2,extflash}.bin (build them first
# with scripts/make_boot_images.py <game> if they don't exist yet).
#
# Deliberately does NOT pass -d guest_errors,unimp or any other verbose
# logging flag -- that has repeatedly filled /tmp and destabilized the
# host. If you need guest-error logging for a specific debugging session,
# add it explicitly and bound the output (e.g. `| head`), don't leave an
# unbounded log file writing for a free-running boot.
#
# Usage: ./scripts/boot_qemu.sh <game> [--patched]
#   --patched: boot <game>-bank1-patched.bin + <game>-extflash-patched.bin
#   -- the EXACT gnwmanager-CFW image pair from the repo-root
#   zelda-patched.7z archive, verified byte-for-byte identical to the
#   physical device's live internal flash (2026-07-12 parts 10/11). This
#   is the pair to use for all lockstep QEMU-vs-hardware tracing: the
#   pivot to the fully-patched CFW (not just a 2-byte standby skip) was
#   an intentional project decision so both targets run the identical
#   image. Defaults to the unmodified stock image pair otherwise, per
#   CLAUDE.md's "always boot unmodified" rule for stock-accuracy work.
set -euo pipefail

GAME="${1:?usage: $0 <game> [--patched]}"
PATCHED="${2:-}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGES_DIR="$REPO_ROOT/backup/qemu-images"
QEMU="$REPO_ROOT/build/qemu-system-arm"

if [ "$PATCHED" = "--patched" ]; then
    BANK1="$IMAGES_DIR/$GAME-bank1-patched.bin"
    EXTFLASH="$IMAGES_DIR/$GAME-extflash-patched.bin"
else
    BANK1="$IMAGES_DIR/$GAME-bank1.bin"
    EXTFLASH="$IMAGES_DIR/$GAME-extflash.bin"
fi
BANK2="$IMAGES_DIR/$GAME-bank2.bin"

for f in "$BANK1" "$BANK2" "$EXTFLASH"; do
    if [ ! -f "$f" ]; then
        echo "missing $f -- run scripts/make_boot_images.py $GAME first" >&2
        exit 1
    fi
done

exec "$QEMU" -M gnw-h7b0 \
    -device loader,file="$BANK1",addr=0x08000000 \
    -device loader,file="$BANK2",addr=0x08100000 \
    -device loader,file="$EXTFLASH",addr=0x90000000 \
    -audiodev pa,id=snd0 \
    -global gnw-h7b0-sai1.audiodev=snd0 \
    -display sdl -s
