#!/usr/bin/env python3
"""
snapshot_registers.py -- reset-halt every peripheral in STM32H7B0.svd and
dump its raw register bytes, against either QEMU (--qemu, via gnwmanager's
gdbstub client) or real hardware (default openocd/pyocd backend).

Meant to be run twice (once per target) and the two JSON outputs diffed
to find reset-default mismatches in one pass, instead of chasing them one
at a time during a boot trace.

Caveat: write-1-to-clear / clear-on-read status registers and free-running
counters (RTC, DWT cycle counter, etc.) can differ between the two
snapshots simply because they weren't read at the exact same wall-clock
instant -- don't treat every diff as a reset-value bug without checking
whether the register is one of those.

Usage:
    ./scripts/snapshot_registers.py --qemu -o snapshot-qemu.json
    ./scripts/snapshot_registers.py -o snapshot-hw.json

--gnwmanager-active snapshots after gnwmanager's own known RAM payload
(GnW.start_gnwmanager()) is loaded, vectored into, and has signaled idle,
instead of after a bare reset_and_halt(). This sidesteps the "how much of
stock firmware executed before our halt landed" ambiguity that made the
plain reset_and_halt() snapshot mostly unusable for finding real
reset-default bugs (see docs/ for that session's writeup) -- both targets
run the exact same known payload to the exact same known completion
point, so any diff here is much more likely to be a genuine peripheral
model gap.
"""
import argparse
import json
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SVD_PATH = REPO_ROOT / "STM32H7B0.svd"
BANK1_PATH = REPO_ROOT / "backup" / "qemu-images" / "zelda-bank1.bin"

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path.home() / "Nerd" / "git" / "gnwmanager"))


def load_peripherals():
    tree = ET.parse(SVD_PATH)
    peripherals = []
    for p in tree.getroot().find("peripherals").findall("peripheral"):
        name = p.findtext("name")
        base_text = p.findtext("baseAddress")
        if name is None or base_text is None:
            continue
        base = int(base_text, 0)
        size = 0x400
        ab = p.find("addressBlock")
        if ab is not None:
            size_text = ab.findtext("size")
            if size_text is not None:
                size = int(size_text, 0)
        # A few SVD entries (e.g. OTG1_HS_PWRCLK, AXI) declare an
        # addressBlock far larger than any real register file -- almost
        # certainly a stray/union artifact in the vendor SVD, not a real
        # peripheral extent. Cap to keep snapshot reads inside mapped
        # memory.
        size = min(size, 0x1000)
        peripherals.append((name, base, size))
    # Some SVD entries (derivedFrom siblings, e.g. GPIOB..K) share a base
    # region size but list separately -- dedupe by base address, keep the
    # first name seen at that address.
    seen = {}
    for name, base, size in peripherals:
        if base not in seen:
            seen[base] = (name, base, size)
    return sorted(seen.values(), key=lambda t: t[1])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", action="store_true", help="target QEMU's gdbstub instead of real hardware")
    ap.add_argument("-o", "--output", required=True, type=Path)
    ap.add_argument("--frequency", type=int, default=None)
    ap.add_argument(
        "--gnwmanager-active",
        action="store_true",
        help="snapshot after gnwmanager's own RAM payload is running and idle, instead of at bare reset_and_halt()",
    )
    ap.add_argument(
        "--halt-at-entry",
        action="store_true",
        help="reset and run to the real reset-vector entry point (from zelda-bank1.bin), halting there via a "
             "real breakpoint instead of racing an async halt request. Mutually exclusive with --gnwmanager-active.",
    )
    args = ap.parse_args()

    if args.halt_at_entry and args.gnwmanager_active:
        sys.exit("--halt-at-entry and --gnwmanager-active are mutually exclusive")

    if args.qemu:
        from gnwmanager.ocdbackend.gdb_backend import GDBBackend
        backend = GDBBackend()
    else:
        from gnwmanager.ocdbackend import OCDBackend
        backend = OCDBackend["openocd"]()

    backend.open()
    try:
        if args.frequency is not None and hasattr(backend, "set_frequency"):
            backend.set_frequency(args.frequency)

        if args.gnwmanager_active:
            from gnwmanager.gnw import GnW
            gnw = GnW(backend)
            gnw.start_gnwmanager()
            mode = "gnwmanager-active"
        elif args.halt_at_entry:
            from halt_at_entry import entry_point_from_bank1, reset_and_run_to
            entry = entry_point_from_bank1(BANK1_PATH)
            print(f"Entry point (from {BANK1_PATH.name}): {entry:#010x}")
            reset_and_run_to(backend, entry)
            mode = f"halt-at-entry ({entry:#010x})"
        else:
            backend.reset_and_halt()
            mode = "reset-halt"

        peripherals = load_peripherals()
        print(f"Snapshotting {len(peripherals)} peripherals "
              f"({'QEMU' if args.qemu else 'real hardware'}, {mode})...")

        result = {}
        for name, base, size in peripherals:
            try:
                data = backend.read_memory(base, size)
            except Exception as e:
                print(f"  {name} @ {base:#010x} (size {size:#x}): READ FAILED: {e}")
                result[name] = {"base": base, "size": size, "error": str(e)}
                continue
            result[name] = {"base": base, "size": size, "hex": data.hex()}
    finally:
        # Always tears down the connection (for OpenOCDBackend, this
        # kills the openocd subprocess) even on exception -- an orphaned
        # openocd process holds the SWD lock and breaks every subsequent
        # script's connection attempt.
        backend.close()

    args.output.write_text(json.dumps(result, indent=2))
    print(f"Wrote {args.output}")


if __name__ == "__main__":
    main()
