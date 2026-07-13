#!/usr/bin/env python3
"""
checkpoint_filtered.py -- like checkpoint.py, but for functions called
constantly with many different argument values where only one specific
call matters (e.g. FUN_0800eb90, Zelda's state-transition dispatcher,
called throughout boot with varying param_1/r0; only the param_1==3 call
matters for reaching LTDC init -- see docs/session-2026-07-12-
breakpoint-lockstep-tracing.md's part 4 section). Breaks at a function
entry, reads a register each hit, and keeps continuing (without
re-arming -- the breakpoint stays live across hits) until the register
matches the target value or --max-hits is exhausted, on both QEMU and
real hardware.

Usage:
    ./scripts/checkpoint_filtered.py 0x0800eb90 --reg r0 --value 3 --max-hits 200
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


def run_target(backend, is_qemu, entry, addr, reg, value, max_hits, timeout):
    reset_and_run_to(backend, entry)
    if is_qemu:
        gdb_set_bp(backend, addr)
    else:
        backend(f"bp 0x{addr:08x} 2 hw", decode=False)

    try:
        for i in range(max_hits):
            if is_qemu:
                hit = gdb_continue_and_wait(backend, timeout, addr=addr)
            else:
                hit = openocd_continue_and_wait(backend, timeout)
            if not hit:
                print(f"  [hit {i}] timed out -- no more calls within {timeout}s")
                return None
            snap = snapshot_gdb(backend) if is_qemu else snapshot_openocd(backend)
            got = snap[reg]
            print(f"  [hit {i}] {reg}={got:#010x}")
            if got == value:
                print(f"  MATCH on hit {i}: {reg}=={value:#x}")
                return snap
        print(f"  exhausted {max_hits} hits without {reg}=={value:#x}")
        return None
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
    ap.add_argument("--reg", default="r0")
    ap.add_argument("--value", type=lambda s: int(s, 0), required=True)
    ap.add_argument("--max-hits", type=int, default=200)
    ap.add_argument("--timeout", type=float, default=8.0)
    args = ap.parse_args()

    entry = entry_point_from_bank1(BANK1_PATH)
    print(f"Entry: {entry:#010x}  Checkpoint: {args.address:#010x}  "
          f"filter: {args.reg}=={args.value:#x}")

    from gnwmanager.ocdbackend.gdb_backend import GDBBackend
    from gnwmanager.ocdbackend import OCDBackend

    print("=== QEMU ===")
    qemu = GDBBackend()
    qemu.open()
    try:
        q = run_target(qemu, True, entry, args.address, args.reg, args.value,
                        args.max_hits, args.timeout)
    finally:
        qemu.close()
    if q:
        print("  " + " ".join(f"{r}={q[r]:#010x}" for r in REGS))

    print("=== Real hardware ===")
    hw = OCDBackend["openocd"]()
    hw.open()
    try:
        h = run_target(hw, False, entry, args.address, args.reg, args.value,
                        args.max_hits, args.timeout)
    finally:
        hw.close()
    if h:
        print("  " + " ".join(f"{r}={h[r]:#010x}" for r in REGS))

    print(f"\nQEMU {'FOUND' if q else 'NEVER CALLED'} with {args.reg}=={args.value:#x}; "
          f"HW {'FOUND' if h else 'NEVER CALLED'}")


if __name__ == "__main__":
    main()
