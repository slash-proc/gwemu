#!/usr/bin/env python3
"""
hotloop_sample.py -- procedurally find guest polling/spin loops that are
burning CPU, and flag which MMIO peripheral (if any) they're spinning on.

Why: QEMU's own overhead (per-pixel MMIO, TCG interpretation) can be
profiled with `perf` on the host process, but that can't see *why* the
guest itself executes as many instructions as it does. This project has
repeatedly found bugs where a device model doesn't set a completion/ready
flag as promptly as real hardware, causing firmware to spin through many
more polling iterations than real hardware ever needs (see CHANGELOG.md's
OSPI/SPI1/JPEG/RTC "status-flag stub" fixes) -- that inflates guest
instruction count itself, which then costs real host TCG time per
instruction. This script finds those loops by statistical PC sampling
(same technique used ad hoc in docs/session-2026-07-12-breakpoint-
lockstep-tracing.md part 23), then automatically resolves any PC-relative
literal-pool loads near each hot address against this SoC's known MMIO
base addresses (from include/hw/arm/gnw_h7b0_soc.h) to name the
peripheral being polled.

Does NOT single-step (too slow/disruptive over a live target -- see
CLAUDE.md's breakpoint-based-only lockstep-tracing note) and does NOT
modify the guest: each sample is a fresh `target remote` connect, one
register read, and a clean `detach` (verified in this project not to
leave stray breakpoints -- see the session doc). Every connect briefly
halts the whole VM (including peripheral input-event delivery -- a known
QEMU gdbstub quirk documented in STATUS.md's tooling notes), so this is
a statistical sample of "where is the PC when something asks", not a
guarantee of representativeness for e.g. rare interrupt handlers -- take
the top hits as a strong lead, not gospel, and cross-check with `perf`
host-side profiling and manual disassembly for anything surprising.

Usage:
    scripts/hotloop_sample.py [--port 1234] [--samples 300] [--top 15]

Requires arm-none-eabi-gdb on PATH and a running qemu-system-arm with
`-s` (or `-gdb tcp::PORT`) already listening.
"""

import argparse
import re
import subprocess
import sys
import tempfile
import os
from collections import Counter

# MMIO peripheral base addresses for the gnw-h7b0 machine, from
# include/hw/arm/gnw_h7b0_soc.h -- kept in sync by hand since there's no
# machine-readable export of these; if this SoC file grows new
# sysbus_mmio_map() calls, add the matching *_BASE_ADDRESS here too.
PERIPHERALS = sorted([
    ("ITCM",         0x00000000),
    ("FLASH_BANK1",  0x08000000),
    ("FLASH_BANK2",  0x08100000),
    ("DTCM",         0x20000000),
    ("AXISRAM1",     0x24000000),
    ("AXISRAM2",     0x24040000),
    ("AXISRAM3",     0x240A0000),
    ("AHBSRAM1",     0x30000000),
    ("AHBSRAM2",     0x30010000),
    ("SRDSRAM",      0x38000000),
    ("BKPSRAM",      0x38800000),
    ("EXTFLASH",     0x90000000),
    ("TIM2_BLOCK",   0x40000000),
    ("SPI2",         0x40003800),
    ("CRS",          0x40008400),
    ("DAC1",         0x40007400),
    ("TIM1",         0x40010000),
    ("SPI1",         0x40013000),
    ("SAI1",         0x40015800),
    ("DMA",          0x40020000),
    ("ADC",          0x40022000),
    ("CRC",          0x40023000),
    ("CRYP",         0x48021000),
    ("WWDG",         0x50003000),
    ("LTDC",         0x50001000),
    ("DMA2D",        0x52001000),
    ("JPEG",         0x52003000),
    ("FLASH_R",      0x52002000),
    ("FMC",          0x52004000),
    ("OCTOSPI1",     0x52005000),
    ("OCTOSPI2",     0x5200A000),
    ("OCTOSPIM",     0x5200B400),
    ("OTFDEC1",      0x5200b800),
    ("OTFDEC2",      0x5200bc00),
    ("GPIO",         0x58020000),
    ("EXTI_SYSCFG",  0x58000000),
    ("SYSCFG",       0x58000400),
    ("TAMP",         0x58004400),
    ("RTC",          0x58004000),
    ("DAC2",         0x58003400),
    ("RCC",          0x58024400),
    ("PWR",          0x58024800),
    ("DBGMCU",       0x5C001000),
    ("DWT",          0xE0001000),
], key=lambda t: t[1])


def name_addr(addr):
    """Largest known base <= addr, if within a plausible peripheral page span."""
    best = None
    for name, base in PERIPHERALS:
        if base <= addr:
            best = (name, base)
    if best is None:
        return None
    name, base = best
    off = addr - base
    if off > 0x2000:
        return None
    return "%s+0x%x" % (name, off)


def gdb_batch(port, commands, timeout=10):
    lines = ["target remote localhost:%d" % port] + commands + ["detach", "quit"]
    with tempfile.NamedTemporaryFile("w", suffix=".gdb", delete=False) as f:
        f.write("\n".join(lines) + "\n")
        path = f.name
    try:
        out = subprocess.run(
            ["arm-none-eabi-gdb", "-q", "-batch", "-x", path],
            capture_output=True, text=True, timeout=timeout,
        )
        return out.stdout + out.stderr
    finally:
        os.unlink(path)


def sample_pc(port):
    out = gdb_batch(port, ['printf "PC=0x%08x\\n", $pc'])
    m = re.search(r"PC=0x([0-9a-fA-F]+)", out)
    return int(m.group(1), 16) if m else None


def disassemble_around(port, pc):
    return gdb_batch(port, ["x/12i 0x%08x-16" % pc])


def read_word(port, addr):
    out = gdb_batch(port, ["x/1xw 0x%08x" % addr])
    m = re.search(r":\s*0x([0-9a-fA-F]+)", out)
    return int(m.group(1), 16) if m else None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=1234)
    ap.add_argument("--samples", type=int, default=300)
    ap.add_argument("--top", type=int, default=15)
    args = ap.parse_args()

    print(f"Sampling PC {args.samples} times over localhost:{args.port} "
          f"(each sample briefly halts the VM)...", file=sys.stderr)
    counts = Counter()
    failures = 0
    for i in range(args.samples):
        pc = sample_pc(args.port)
        if pc is None:
            failures += 1
            continue
        counts[pc] += 1
        if (i + 1) % 50 == 0:
            print(f"  {i + 1}/{args.samples}", file=sys.stderr)

    if failures:
        print(f"warning: {failures}/{args.samples} samples failed to read PC "
              f"(is the target actually connected on port {args.port}?)",
              file=sys.stderr)

    total = sum(counts.values())
    if total == 0:
        print("No samples collected -- nothing to report.", file=sys.stderr)
        sys.exit(1)

    print(f"\n{total} samples, {len(counts)} distinct PCs. Top {args.top}:\n")
    print(f"{'PC':>12}  {'hits':>6}  {'%':>6}")
    for pc, n in counts.most_common(args.top):
        print(f"  0x{pc:08x}  {n:6d}  {100.0 * n / total:5.1f}%")

    print("\n--- Disassembly + literal-pool peripheral resolution for the "
          "hottest addresses ---")
    for pc, n in counts.most_common(min(args.top, 10)):
        pct = 100.0 * n / total
        print(f"\n=== 0x{pc:08x} ({n} hits, {pct:.1f}%) ===")
        disasm = disassemble_around(args.port, pc)
        for line in disasm.splitlines():
            line = line.rstrip()
            # Must look like an actual disassembly line ("0xADDR <sym>:\tinsn"),
            # not gdb's "0xADDR in ?? ()" connection-banner line (which prints
            # the *current* PC at connect time -- usually the idle loop, since
            # that's where the target sits most of the time -- and would
            # otherwise get misattributed to whichever hotspot we're
            # disassembling right now).
            if not re.match(r"\s*(=>)?\s*0x[0-9a-fA-F]+[^:]*:\s", line):
                continue
            print(f"  {line}")
            lit = re.search(r"@ \(0x([0-9a-fA-F]+)\)", line)
            if lit:
                lit_addr = int(lit.group(1), 16)
                val = read_word(args.port, lit_addr)
                if val is not None:
                    hint = name_addr(val)
                    if hint:
                        print(f"      -> literal 0x{lit_addr:08x} = "
                              f"0x{val:08x}  ==> looks like a pointer into "
                              f"{hint}")
        # Flag a short backward branch near the hot PC as a likely spin loop.
        back_branches = re.findall(
            r"0x([0-9a-fA-F]+).*?\b(?:b|bne|beq|bcc|bcs|bmi|bpl)\.?n?\s+0x([0-9a-fA-F]+)",
            disasm)
        for frm, to in back_branches:
            frm_i, to_i = int(frm, 16), int(to, 16)
            if 0 < frm_i - to_i <= 32:
                print(f"      -> short backward branch 0x{frm_i:08x} -> "
                      f"0x{to_i:08x}: likely a spin/poll loop")

    print("\nNote: this is a statistical sample (each connect briefly halts "
          "the VM), not an instruction trace -- treat high-hit addresses as "
          "leads to investigate (what real hardware's HAL driver polls at "
          "that address, and whether this project's device model sets the "
          "corresponding ready/done bit as promptly as real hardware), not "
          "as confirmed bugs by themselves.")


if __name__ == "__main__":
    main()
