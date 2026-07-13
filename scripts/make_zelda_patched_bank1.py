#!/usr/bin/env python3
"""
make_zelda_patched_bank1.py -- produce a standby-patched copy of
backup/qemu-images/zelda-bank1.bin for real-hardware lockstep tracing.

Stock Zelda firmware's own state-6 standby handler puts real hardware
into low-power standby during repeated resets, dropping the SWD/debug
probe connection mid-trace (see
docs/session-2026-07-12-breakpoint-lockstep-tracing.md). gnwmanager's
own firmware patcher (~/Nerd/git/gnwmanager/gnwmanager/cli/gnw_patch/
zelda.py, patch(), "Warm-boot power-off fix" block) already carries the
fix for its custom-firmware builds; this script reproduces the same two
byte-level edits directly against our own bank1 image instead of running
gnwmanager's full patch pipeline (which also does unrelated things like
disabling save encryption/erasing save data -- not appropriate for a
stock-boot trace, per CLAUDE.md's "always boot ... unmodified" rule).
This is the one documented exception: a *diagnostic* aid for hardware
tracing only, per CLAUDE.md's live-patch guidance -- fixes still land in
the device model, not here.

Patches (offsets relative to FLASH_BASE 0x08000000, i.e. direct file
offsets into bank1.bin):
  - 0xEAA0: 2-byte unconditional branch to 0xEAC2, replacing a
    conditional bpl -- skips the state-6 standby entry entirely.
  - 0xEBD0: 2-byte NOP, replacing a conditional bpl -- disables the SBF
    (standby-wake) gate so the always-standby-wake init path is taken.

Usage: ./scripts/make_zelda_patched_bank1.py
Output: backup/qemu-images/zelda-bank1-patched.bin
"""
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SRC = REPO_ROOT / "backup" / "qemu-images" / "zelda-bank1.bin"
DST = REPO_ROOT / "backup" / "qemu-images" / "zelda-bank1-patched.bin"


def twos_complement(value, bits):
    return value if value >= 0 else (1 << bits) + value


def encode_b(offset: int, dest_offset: int) -> bytes:
    """Encode a Thumb unconditional B instruction at `offset` branching to
    `dest_offset`, matching gnwmanager's FirmwarePatchMixin.b()."""
    pc = offset + 4
    jump = dest_offset - pc
    if abs(jump) > (2 * (1 << 10)):
        raise ValueError(f"jump {jump} out of Thumb B range")
    jump >>= 1
    jump = twos_complement(jump, 11)
    byte_0 = 0b1110_0000 | ((jump >> 8) & 0x7)
    byte_1 = jump & 0xFF
    return bytes([byte_1, byte_0])


def main():
    if not SRC.exists():
        sys.exit(f"missing {SRC} -- run make_boot_images.py zelda first")

    data = bytearray(SRC.read_bytes())

    # state-6 standby: bpl -> b (never standby)
    data[0xEAA0:0xEAA0 + 2] = encode_b(0xEAA0, 0xEAC2)

    # SBF gate: bpl -> nop (always return 1)
    data[0xEBD0:0xEBD0 + 2] = b"\x00\xbf"

    DST.write_bytes(bytes(data))
    print(f"Wrote {DST} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
