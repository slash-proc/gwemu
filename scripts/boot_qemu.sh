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
# Usage: ./scripts/boot_qemu.sh <game> [--patched] [--ephemeral]
#        ./scripts/boot_qemu.sh --diag [path/to/diag.bin]
#        ./scripts/boot_qemu.sh --gui
#   --gui: boot with no bank1-image/bank2-image/extflash-image at all --
#   gnw_h7b0_init_ram_or_file() already falls back to blank RAM when a
#   property is unset, so nothing crashes on the QEMU-device side, but a
#   fully blank vector table (SP=0, PC=0) makes the ARMv7-M CPU itself hit
#   a genuine, correctly-detected lockup ("can't escalate 3 to HardFault")
#   the instant it starts running -- QEMU treats that as fatal and aborts
#   the whole process, GUI included, not just the guest. So this mode
#   starts the CPU halted (-S) instead of running -- the GUI itself
#   doesn't depend on the guest CPU executing, only on Flash being
#   configured before you'd ever want it to. Configure Flash from inside
#   the GUI (Flash tab, Apply), which restarts the process via
#   xemu_relaunch_with_flash_images() with real images -- normal boot
#   from there.
#   --diag: boot the sibling `../stm32h7b0-diag` project's benchmark/test
#   firmware instead of a game image -- a single raw binary loaded at
#   0x08000000 (Cortex-M reset vector), no bank2/extflash involved. Defaults
#   to ../stm32h7b0-diag/fw/build/diag.bin (build it first with `make -C
#   ../stm32h7b0-diag/fw` if missing); override the diag repo location with
#   DIAG_ROOT, or pass an explicit .bin path as the second argument. Runs
#   free (not gdbstub-halted) with a real display, unlike that project's own
#   scripts/run_qemu.sh (which halts at reset for host/harness.py to attach)
#   -- this mode is for watching it run, not for the harness.
#   --patched: boot <game>-bank1-patched.bin + <game>-extflash-patched.bin
#   -- the EXACT gnwmanager-CFW image pair from the repo-root
#   zelda-patched.7z archive, verified byte-for-byte identical to the
#   physical device's live internal flash (2026-07-12 parts 10/11). This
#   is the pair to use for all lockstep QEMU-vs-hardware tracing: the
#   pivot to the fully-patched CFW (not just a 2-byte standby skip) was
#   an intentional project decision so both targets run the identical
#   image. Defaults to the unmodified stock image pair otherwise, per
#   CLAUDE.md's "always boot unmodified" rule for stock-accuracy work.
#   bank1/bank2/extflash are mmap'd directly from their .bin files
#   (gnw-h7b0-soc's bank1-image/bank2-image/extflash-image properties)
#   by default -- guest writes (flashing, erasing) land in the files
#   themselves as they happen, live, so state survives a QEMU restart.
#   This matches how real hardware behaves (flashing a real device
#   changes the device); an in-memory-only, discard-on-exit mode was the
#   wrong default for that reason.
#   --ephemeral: opt out of the above -- images are just a one-time boot
#   seed loaded into anonymous RAM, writes never touch the source .bin
#   files. Useful for repeatable testing against a known-good image
#   without re-copying it after every run.
set -euo pipefail

GAME="${1:?usage: $0 <game> [--patched] [--ephemeral]  |  $0 --diag [path/to/diag.bin]  |  $0 --gui}"
shift
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU="$REPO_ROOT/build/qemu-system-arm"

if [ "$GAME" = "--gui" ]; then
    exec "$QEMU" -M gnw-h7b0 -display gwemu -S
fi

if [ "$GAME" = "--diag" ]; then
    DIAG_ROOT="${DIAG_ROOT:-$REPO_ROOT/../stm32h7b0-diag}"
    DIAG_BIN="${1:-$DIAG_ROOT/fw/build/diag.bin}"
    if [ ! -f "$DIAG_BIN" ]; then
        echo "error: diag firmware binary not found: $DIAG_BIN" >&2
        echo "  (build it first, e.g. \`make -C $DIAG_ROOT/fw\`, or pass an" >&2
        echo "   explicit path: $0 --diag <path-to-diag.bin>)" >&2
        exit 1
    fi

    AUDIODEV="${GNW_AUDIODEV:-$("$QEMU" -audiodev help 2>/dev/null | grep -m1 -E '^coreaudio$|^pa$|^sdl$|^none$')}"
    DISPLAY_BACKEND="${GNW_DISPLAY:-$("$QEMU" -display help 2>/dev/null | grep -m1 -E '^cocoa$|^sdl$|^gtk$')}"

    echo "booting diag firmware: $DIAG_BIN" >&2
    exec "$QEMU" -M gnw-h7b0 \
        -device loader,file="$DIAG_BIN",addr=0x08000000,force-raw=on \
        -audiodev "$AUDIODEV",id=snd0 \
        -global gnw-h7b0-sai1.audiodev=snd0 \
        -display "$DISPLAY_BACKEND" -s
fi

IMAGES_DIR="$REPO_ROOT/backup/qemu-images"
PATCHED=""
EPHEMERAL=""
for arg in "$@"; do
    case "$arg" in
        --patched) PATCHED="--patched" ;;
        --ephemeral) EPHEMERAL="--ephemeral" ;;
        --persist) ;; # now the default; accepted for backwards compatibility
        *) echo "unknown argument: $arg" >&2; exit 1 ;;
    esac
done

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

# Audio/display backends are compile-time options, so the ones this qemu was
# built with vary by host: `pa`/`sdl` are what a Linux build has, while a macOS
# build has `coreaudio`/`cocoa` instead. Pick the first choice the binary
# actually reports rather than hardcoding one. Override with
# GNW_AUDIODEV/GNW_DISPLAY (e.g. GNW_AUDIODEV=none to boot silently).
pick_backend() {
    local kind="$1" envvar="$2"; shift 2
    local available d
    available="$("$QEMU" "-$kind" help 2>/dev/null)"
    for d in "$@"; do
        if printf '%s\n' "$available" | grep -qx "$d"; then
            printf '%s\n' "$d"
            return 0
        fi
    done
    echo "no usable $kind backend in $QEMU -- set $envvar explicitly" >&2
    return 1
}

AUDIODEV="${GNW_AUDIODEV:-$(pick_backend audiodev GNW_AUDIODEV sdl3 coreaudio pa sdl none)}"
DISPLAY_BACKEND="${GNW_DISPLAY:-$(pick_backend display GNW_DISPLAY cocoa sdl gtk none)}"

# A host with no default output device (CoreAudio enumerating zero devices --
# e.g. a Hackintosh with no AppleALC layout-id, or a VM with no audio) makes
# coreaudio's open_out fail. QEMU treats that as non-fatal and boots anyway,
# but only after three lines of error_report noise. Degrade to the silent
# backend up front instead, and say so once. GNW_AUDIODEV=coreaudio forces the
# attempt anyway.
if [ "$AUDIODEV" = "coreaudio" ] && [ -z "${GNW_AUDIODEV:-}" ]; then
    if osascript -e 'get volume settings' 2>/dev/null \
        | grep -q 'output volume:missing value'; then
        echo "note: host has no audio output device -- booting without sound." >&2
        echo "      (set up audio on this host, or GNW_AUDIODEV=coreaudio to force)" >&2
        AUDIODEV=none
    fi
fi

# force-raw=on is required: QEMU's generic loader device auto-detects
# ELF-vs-raw-binary by probing file content, and for at least one real
# extflash image (the retro-go-merged one) that probe misfires past the
# first few KB, silently loading only a small leading fraction of the
# file and leaving the rest of guest memory zeroed -- with no error or
# warning. Confirmed live (2026-07-13): guest memory at +16MB read back
# as all-zero without force-raw, and matched the source file exactly
# with it. Apply to all three loads, not just extflash, since the same
# silent-failure risk applies to any file that might trip the same
# auto-detection heuristic.
if [ "$EPHEMERAL" = "--ephemeral" ]; then
    exec "$QEMU" -M gnw-h7b0 \
        -device loader,file="$BANK1",addr=0x08000000,force-raw=on \
        -device loader,file="$BANK2",addr=0x08100000,force-raw=on \
        -device loader,file="$EXTFLASH",addr=0x90000000,force-raw=on \
        -audiodev "$AUDIODEV",id=snd0 \
        -global gnw-h7b0-sai1.audiodev=snd0 \
        -display "$DISPLAY_BACKEND" -s
else
    exec "$QEMU" -M gnw-h7b0 \
        -global gnw-h7b0-soc.bank1-image="$BANK1" \
        -global gnw-h7b0-soc.bank2-image="$BANK2" \
        -global gnw-h7b0-soc.extflash-image="$EXTFLASH" \
        -audiodev "$AUDIODEV",id=snd0 \
        -global gnw-h7b0-sai1.audiodev=snd0 \
        -display "$DISPLAY_BACKEND" -s
fi
