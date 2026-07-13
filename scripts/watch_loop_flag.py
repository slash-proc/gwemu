#!/usr/bin/env python3
"""
watch_loop_flag.py -- the main superloop's body (0x080105ee-0x08010628,
per Ghidra decompilation of FUN_0801051e) checks a loop-exit flag byte at
[r4+9] each pass (0x0801061a: ldrb r0,[r4,#9]) and a separate
counter-enable field at [r4+0x60] (read earlier in the same pass, at
0x08010604) -- both previously documented (docs/session-2026-07-12-
stock-firmware-boot-investigation.md) as central to why boot never
progresses past this loop. This breakpoints at 0x0801061a (right after
the flag byte loads into r0) and iterates, reading r0 (the flag) plus a
live memory read of [r4+0x60] (the enable field) each pass, on both
targets, to see how the loop's internal state actually evolves compared
between QEMU and real hardware.

Usage:
    ./scripts/watch_loop_flag.py --hits 20
"""
import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path.home() / "Nerd" / "git" / "gnwmanager"))

from halt_at_entry import entry_point_from_bank1, reset_and_run_to  # noqa: E402
from step_init_calls import gdb_set_bp, gdb_clear_bp, gdb_continue_and_wait, openocd_continue_and_wait  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
BANK1_PATH = REPO_ROOT / "backup" / "qemu-images" / "zelda-bank1-patched.bin"
FLAG_CHECK = 0x0801061C  # right after "ldrb r0,[r4,#9]" executes (breakpoints halt BEFORE
                          # the instruction at their address runs, so this must be one
                          # instruction past the load, not at it, or r0 is stale)
ENABLE_FIELD_OFFSET = 0x60


def run_target(backend, is_qemu, entry, hits, timeout):
    reset_and_run_to(backend, entry)
    if is_qemu:
        gdb_set_bp(backend, FLAG_CHECK)
    else:
        backend(f"bp 0x{FLAG_CHECK:08x} 2 hw", decode=False)

    results = []
    try:
        for i in range(hits):
            if is_qemu:
                hit = gdb_continue_and_wait(backend, timeout, addr=FLAG_CHECK)
            else:
                hit = openocd_continue_and_wait(backend, timeout)
            if not hit:
                print(f"  [pass {i}] timed out -- loop no longer hitting the flag check "
                      f"(exited, or genuinely stuck deeper inside one pass)")
                break

            if is_qemu:
                flag = backend.read_register("r0")
                r4 = backend.read_register("r4")
                enable_field = int.from_bytes(backend.read_memory(r4 + ENABLE_FIELD_OFFSET, 4), "little")
            else:
                flag = _openocd_reg(backend, "r0")
                r4 = _openocd_reg(backend, "r4")
                enable_field = int.from_bytes(backend.read_memory(r4 + ENABLE_FIELD_OFFSET, 4), "little")

            results.append((flag, enable_field))
            print(f"  [pass {i}] flag=[r4+9]={flag:#04x}  enable_field=[r4+0x60]={enable_field:#010x}  r4={r4:#010x}")
    finally:
        try:
            if is_qemu:
                gdb_clear_bp(backend, FLAG_CHECK)
            else:
                backend(f"rbp 0x{FLAG_CHECK:08x}", decode=False)
        except Exception as e:
            print(f"  (warning: failed to clear breakpoint, non-fatal: {e})")

    return results


def _openocd_reg(backend, name, retries=5):
    for attempt in range(retries):
        resp = backend(f"reg {name}", decode=False).decode(errors="replace").strip()
        tokens = resp.split()
        if tokens:
            return int(tokens[-1], 16)
        time.sleep(0.1 * (attempt + 1))
    raise RuntimeError(f"empty 'reg {name}' response after {retries} retries")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hits", type=int, default=20)
    ap.add_argument("--timeout", type=float, default=8.0)
    args = ap.parse_args()

    entry = entry_point_from_bank1(BANK1_PATH)
    print(f"Entry: {entry:#010x}  Flag-check breakpoint: {FLAG_CHECK:#010x}")

    from gnwmanager.ocdbackend.gdb_backend import GDBBackend
    from gnwmanager.ocdbackend import OCDBackend

    print("\n=== QEMU ===")
    qemu = GDBBackend()
    qemu.open()
    try:
        qemu_results = run_target(qemu, True, entry, args.hits, args.timeout)
    finally:
        qemu.close()

    print("\n=== Real hardware ===")
    hw = OCDBackend["openocd"]()
    hw.open()
    try:
        hw_results = run_target(hw, False, entry, args.hits, args.timeout)
    finally:
        hw.close()

    print(f"\nQEMU passes reached: {len(qemu_results)}   HW passes reached: {len(hw_results)}")


if __name__ == "__main__":
    main()
