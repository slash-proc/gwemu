#!/usr/bin/env python3
"""One-off: checkpoint FUN_0800ef44's 2nd call (r0==0xff00) on both targets,
then read the DAT_0800f4bc struct's [1] (prev-button-state) and [5] fields
directly to see which branch each target is about to take.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path.home() / "Nerd" / "git" / "gnwmanager"))

from halt_at_entry import entry_point_from_bank1, reset_and_run_to  # noqa: E402
from step_init_calls import gdb_set_bp, gdb_clear_bp, gdb_continue_and_wait, openocd_continue_and_wait, snapshot_gdb, snapshot_openocd  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
BANK1_PATH = REPO_ROOT / "backup" / "qemu-images" / "zelda-bank1-patched.bin"
ADDR = 0x0800ef44
STRUCT_PTR = 0x0800f4bc

def run(backend, is_qemu, entry, timeout):
    reset_and_run_to(backend, entry)
    if is_qemu:
        gdb_set_bp(backend, ADDR)
    else:
        backend(f"bp 0x{ADDR:08x} 2 hw", decode=False)
    try:
        for i in range(4):
            hit = gdb_continue_and_wait(backend, timeout, addr=ADDR) if is_qemu else openocd_continue_and_wait(backend, timeout)
            if not hit:
                print(f"  hit {i}: timeout")
                return
            snap = snapshot_gdb(backend) if is_qemu else snapshot_openocd(backend)
            print(f"  hit {i}: r0={snap['r0']:#010x}")
            if snap['r0'] == 0xff00:
                if is_qemu:
                    data = backend.read_memory(STRUCT_PTR, 24)
                    print(f"    struct[0x0800f4bc..+24]: {data.hex()}")
                else:
                    resp = backend(f"mdb 0x{STRUCT_PTR:08x} 24", decode=False).decode(errors="replace")
                    print(f"    struct[0x0800f4bc..+24]: {resp.strip()}")
                return
    finally:
        try:
            if is_qemu:
                gdb_clear_bp(backend, ADDR)
            else:
                backend(f"rbp 0x{ADDR:08x}", decode=False)
        except Exception as e:
            print(f"  (warn: clear bp failed: {e})")

def main():
    entry = entry_point_from_bank1(BANK1_PATH)
    from gnwmanager.ocdbackend.gdb_backend import GDBBackend
    from gnwmanager.ocdbackend import OCDBackend

    print("=== QEMU ===")
    qemu = GDBBackend()
    qemu.open()
    try:
        run(qemu, True, entry, 5.0)
    finally:
        qemu.close()

    print("=== Real hardware ===")
    hw = OCDBackend["openocd"]()
    hw.open()
    try:
        run(hw, False, entry, 5.0)
    finally:
        hw.close()

if __name__ == "__main__":
    main()
