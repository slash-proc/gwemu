#!/usr/bin/env python3
"""
make_boot_images.py -- build a standardized, correctly-sized set of boot
images (bank1, bank2, extflash) for a given game, from the raw dumps in
backup/, so every QEMU launch in this project uses the same known-good
inputs instead of ad hoc partial dumps.

Sizes are pulled directly from include/hw/arm/gnw_h7b0_soc.h
(FLASH_BANK_SIZE, EXTFLASH_SIZE) so they can't drift out of sync with the
machine model.

- bank1: backup/internal_flash_backup_<game>.bin, real dumped content,
  zero-padded... no -- 0xFF-padded (real NOR/flash erased-state byte) up
  to FLASH_BANK_SIZE if the dump is shorter.
- bank2: no real dump exists (stock firmware doesn't use it) -- entirely
  0xFF (blank/erased), sized FLASH_BANK_SIZE, per project decision to
  treat bank2 as blank for now.
- extflash: backup/flash_backup_<game>.bin, real dumped content,
  0xFF-padded up to EXTFLASH_SIZE (64 MiB) -- the real dump is a partial
  capture (4 MiB) even though the real chip is 64 MiB (confirmed via
  `gnwmanager info`'s "External Flash Size (MB): 64.0"), so the tail
  beyond the real dump is blank/erased for now, per project decision.

Usage: ./scripts/make_boot_images.py zelda
Output: backup/qemu-images/<game>-{bank1,bank2,extflash}.bin
"""
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SOC_H = REPO_ROOT / "include" / "hw" / "arm" / "gnw_h7b0_soc.h"


def read_define(name: str) -> int:
    text = SOC_H.read_text()
    m = re.search(rf"#define\s+{name}\s+\(?([^)\n]+)\)?", text)
    if not m:
        raise ValueError(f"could not find {name} in {SOC_H}")
    expr = m.group(1).strip()
    # expr is a small C arithmetic expression like "256 * 1024" or
    # "64 * 1024 * 1024" -- safe to eval, it's our own header.
    return eval(expr, {"__builtins__": {}})


def pad_with_ff(data: bytes, size: int, label: str) -> bytes:
    if len(data) > size:
        raise ValueError(f"{label}: source is {len(data)} bytes, larger than target size {size}")
    if len(data) < size:
        print(f"  {label}: padding {len(data)} -> {size} bytes with 0xFF")
    return data + b"\xff" * (size - len(data))


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <game>", file=sys.stderr)
        sys.exit(1)
    game = sys.argv[1]

    bank_size = read_define("FLASH_BANK_SIZE")
    extflash_size = read_define("EXTFLASH_SIZE")
    print(f"FLASH_BANK_SIZE={bank_size:#x}  EXTFLASH_SIZE={extflash_size:#x}")

    backup_dir = REPO_ROOT / "backup"
    out_dir = backup_dir / "qemu-images"
    out_dir.mkdir(exist_ok=True)

    internal_src = backup_dir / f"internal_flash_backup_{game}.bin"
    extflash_src = backup_dir / f"flash_backup_{game}.bin"
    if not internal_src.exists():
        sys.exit(f"missing {internal_src}")
    if not extflash_src.exists():
        sys.exit(f"missing {extflash_src}")

    bank1 = pad_with_ff(internal_src.read_bytes(), bank_size, "bank1")
    bank2 = b"\xff" * bank_size
    print(f"  bank2: blank (0xFF) x {bank_size} bytes")
    extflash = pad_with_ff(extflash_src.read_bytes(), extflash_size, "extflash")

    (out_dir / f"{game}-bank1.bin").write_bytes(bank1)
    (out_dir / f"{game}-bank2.bin").write_bytes(bank2)
    (out_dir / f"{game}-extflash.bin").write_bytes(extflash)

    print(f"Wrote {out_dir}/{game}-{{bank1,bank2,extflash}}.bin")


if __name__ == "__main__":
    main()
