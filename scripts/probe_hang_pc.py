#!/usr/bin/env python3
"""One-off: reset QEMU to entry, breakpoint FUN_0800ef44 entry, continue
through the 1st hit (clear+step-over+rearm), then continue again and if
no stop within 5s, halt manually and read the live PC/lr to see where the
CPU actually is (genuinely wedged at a specific address vs a breakpoint-
detection artifact)."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path.home() / "Nerd" / "git" / "gnwmanager"))

from halt_at_entry import entry_point_from_bank1, reset_and_run_to  # noqa: E402
from step_init_calls import gdb_set_bp, gdb_clear_bp, gdb_continue_and_wait, snapshot_gdb  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
BANK1_PATH = REPO_ROOT / "backup" / "qemu-images" / "zelda-bank1-patched.bin"
ADDR = 0x0800ef44

from gnwmanager.ocdbackend.gdb_backend import GDBBackend  # noqa: E402

entry = entry_point_from_bank1(BANK1_PATH)
qemu = GDBBackend()
qemu.open()
try:
    reset_and_run_to(qemu, entry)
    gdb_set_bp(qemu, ADDR)
    hit = gdb_continue_and_wait(qemu, 5.0, addr=ADDR)
    print("hit0:", hit, snapshot_gdb(qemu) if hit else None)
    hit2 = gdb_continue_and_wait(qemu, 5.0, addr=ADDR)
    print("hit1:", hit2)
    if not hit2:
        print("forcing halt to see live PC...")
        qemu.halt()
        snap = snapshot_gdb(qemu)
        print("  live state:", {k: hex(v) for k, v in snap.items()})
finally:
    try:
        gdb_clear_bp(qemu, ADDR)
    except Exception as e:
        print("clear bp failed:", e)
    qemu.close()
