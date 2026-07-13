#!/usr/bin/env python3
"""Checkpoint FUN_08011536's entry on both targets, read r4/r5 (the
unaff_r4/unaff_r5 'this'-style handle registers the decompile shows it
actually uses), plus NVIC ISER/ISPR bits for IRQ 92 (OCTOSPI1) and IRQ
150 (OCTOSPI2), plus live OSPI1 CR/SR, to re-anchor the trace on real
data instead of Ghidra's uncertain register typing.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path.home() / "Nerd" / "git" / "gnwmanager"))

from halt_at_entry import entry_point_from_bank1, reset_and_run_to  # noqa: E402
from step_init_calls import gdb_set_bp, gdb_clear_bp, gdb_continue_and_wait, openocd_continue_and_wait, snapshot_gdb, snapshot_openocd  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
BANK1_PATH = REPO_ROOT / "backup" / "qemu-images" / "zelda-bank1-patched.bin"
ADDR = 0x08011536

# NVIC ISER/ISPR base for Cortex-M armv7m (external interrupts).
NVIC_ISER0 = 0xE000E100  # ISERx, 32 bits per register, IRQ N -> ISERx[N/32] bit N%32
NVIC_ISPR0 = 0xE000E200


def irq_bit(read_word_fn, base, irqn):
    word = read_word_fn(base + 4 * (irqn // 32))
    return (word >> (irqn % 32)) & 1


def run(backend, is_qemu, entry, timeout):
    reset_and_run_to(backend, entry)
    if is_qemu:
        gdb_set_bp(backend, ADDR)
    else:
        backend(f"bp 0x{ADDR:08x} 2 hw", decode=False)
    try:
        hit = gdb_continue_and_wait(backend, timeout, addr=ADDR) if is_qemu else openocd_continue_and_wait(backend, timeout)
        if not hit:
            print("  timeout -- never reached")
            return
        snap = snapshot_gdb(backend) if is_qemu else snapshot_openocd(backend)
        print(f"  r0={snap['r0']:#010x} r1={snap['r1']:#010x} r2={snap['r2']:#010x} "
              f"r3={snap['r3']:#010x} sp={snap['sp']:#010x} lr={snap['lr']:#010x}")

        if is_qemu:
            r4 = backend.read_register("r4")
            r5 = backend.read_register("r5")
        else:
            r4 = int(backend("reg r4", decode=False).decode(errors="replace").strip().split()[-1], 16)
            r5 = int(backend("reg r5", decode=False).decode(errors="replace").strip().split()[-1], 16)
        print(f"  r4(unaff_r4)={r4:#010x} r5(unaff_r5)={r5:#010x}")
        if is_qemu:
            state15 = struct.unpack("<i", backend.read_memory(r4 + 0x15 * 4, 4))[0]
            state16 = struct.unpack("<i", backend.read_memory(r4 + 0x16 * 4, 4))[0]
            r5_0 = struct.unpack("<i", backend.read_memory(r5, 4))[0]
            r5_e = struct.unpack("<i", backend.read_memory(r5 + 0xe * 4, 4))[0]
        else:
            def mdw(addr):
                resp = backend(f"mdw 0x{addr:08x}", decode=False).decode(errors="replace")
                return int(resp.strip().split(":")[1].split()[0], 16)
            state15 = mdw(r4 + 0x15 * 4)
            state16 = mdw(r4 + 0x16 * 4)
            r5_0 = mdw(r5)
            r5_e = mdw(r5 + 0xe * 4)
        print(f"  r4[0x15](state)={state15:#010x} r4[0x16]={state16:#010x} "
              f"*r5={r5_0:#010x} r5[0xe]={r5_e:#010x}")

        def read_word(addr):
            if is_qemu:
                return struct.unpack("<I", backend.read_memory(addr, 4))[0]
            resp = backend(f"mdw 0x{addr:08x}", decode=False).decode(errors="replace")
            return int(resp.strip().split(":")[1].split()[0], 16)

        ospi1_cr = read_word(0x52005000)
        ospi1_sr = read_word(0x52005020)
        iser_92 = irq_bit(read_word, NVIC_ISER0, 92)
        ispr_92 = irq_bit(read_word, NVIC_ISPR0, 92)
        print(f"  OSPI1 CR={ospi1_cr:#010x} SR={ospi1_sr:#010x}  "
              f"NVIC ISER[92]={iser_92} ISPR[92]={ispr_92}")
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
