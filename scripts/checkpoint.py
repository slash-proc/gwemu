#!/usr/bin/env python3
"""
checkpoint.py -- generalized version of step_init_calls.py's approach:
reset both QEMU and real hardware to the real entry point, set a
breakpoint at an arbitrary target address, run to it (skipping over any
long busy-wait/delay loops in between instead of single-stepping through
them), and compare core registers. Meant for walking forward through a
sequence of call sites/checkpoints identified via Ghidra decompilation,
one at a time, to find where real execution first diverges from QEMU --
without paying the cost of single-stepping through everything in
between.

Usage:
    ./scripts/checkpoint.py 0x0801b03e
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path.home() / "Nerd" / "git" / "gnwmanager"))

from halt_at_entry import entry_point_from_bank1, reset_and_run_to  # noqa: E402
from step_init_calls import (  # noqa: E402
    gdb_set_bp, gdb_clear_bp, gdb_continue_and_wait, openocd_continue_and_wait,
    snapshot_gdb, snapshot_openocd, REGS,
)

REPO_ROOT = Path(__file__).resolve().parent.parent
BANK1_PATH = REPO_ROOT / "backup" / "qemu-images" / "zelda-bank1-patched.bin"


def run_target(backend, is_qemu, entry, addr, timeout):
    reset_and_run_to(backend, entry)
    if is_qemu:
        gdb_set_bp(backend, addr)
    else:
        backend(f"bp 0x{addr:08x} 2 hw", decode=False)

    try:
        if is_qemu:
            hit = gdb_continue_and_wait(backend, timeout, addr=addr)
        else:
            hit = openocd_continue_and_wait(backend, timeout)
        if not hit:
            print(f"  timed out waiting to reach {addr:#010x}")
            return None
        return snapshot_gdb(backend) if is_qemu else snapshot_openocd(backend)
    finally:
        try:
            if is_qemu:
                gdb_clear_bp(backend, addr)
            else:
                backend(f"rbp 0x{addr:08x}", decode=False)
        except Exception as e:
            print(f"  (warning: failed to clear breakpoint, non-fatal: {e})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("address", type=lambda s: int(s, 0))
    ap.add_argument("--timeout", type=float, default=8.0)
    args = ap.parse_args()

    entry = entry_point_from_bank1(BANK1_PATH)
    print(f"Entry: {entry:#010x}  Checkpoint: {args.address:#010x}")

    from gnwmanager.ocdbackend.gdb_backend import GDBBackend
    from gnwmanager.ocdbackend import OCDBackend

    print("=== QEMU ===")
    qemu = GDBBackend()
    qemu.open()
    try:
        q = run_target(qemu, True, entry, args.address, args.timeout)
    finally:
        qemu.close()
    if q:
        print("  " + " ".join(f"{r}={q[r]:#010x}" for r in REGS))

    print("=== Real hardware ===")
    hw = OCDBackend["openocd"]()
    hw.open()
    try:
        h = run_target(hw, False, entry, args.address, args.timeout)
    finally:
        hw.close()
    if h:
        print("  " + " ".join(f"{r}={h[r]:#010x}" for r in REGS))

    if q and h:
        mismatches = [r for r in REGS if q[r] != h[r]]
        print(f"\n{'MATCH' if not mismatches else f'DIVERGE on {mismatches}'}")
    elif q or h:
        print(f"\nONE TARGET NEVER REACHED CHECKPOINT: QEMU={'reached' if q else 'MISSING'}, "
              f"HW={'reached' if h else 'MISSING'}")


if __name__ == "__main__":
    main()
