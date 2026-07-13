#!/usr/bin/env python3
"""
lockstep_compare.py -- reset both QEMU and real hardware to the real
entry point (via halt_at_entry.py's breakpoint mechanism, not a racy
async halt), then single-step both in alternation, comparing PC after
each step. Reports the first step where they diverge, plus a handful of
core registers at that point for triage. This replaces one-off manual
watchpoint guessing with a systematic, repeatable "where does real
firmware execution first differ from QEMU" check.

*** THE CAVEAT: real hardware single-stepping is a full SWD round trip
per instruction -- this is genuinely slow (order of 10s of ms/step at
best), so running this open-ended from the very first instruction of
boot could take a long time (thousands+ of steps) if the real divergence
is deep into initialization. Default is open-ended (no step cap) per
explicit instruction, printing progress every 20 steps so it's visible
this is still alive and where it's gotten to -- watch that output and be
ready to Ctrl-C and switch to a narrower search (bisect forward with
watch_write.py against a specific suspected address, then lockstep from
a breakpoint near there instead of from the very top) if it's clearly
going to take too long rather than letting it run for its own sake. ***

Usage:
    ./scripts/lockstep_compare.py
    ./scripts/lockstep_compare.py --max-steps 500 --start 0x0801ad48
"""
import argparse
import itertools
import socket
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path.home() / "Nerd" / "git" / "gnwmanager"))

from halt_at_entry import entry_point_from_bank1, reset_and_run_to  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
BANK1_PATH = REPO_ROOT / "backup" / "qemu-images" / "zelda-bank1-patched.bin"

CORE_REGS = ["r0", "r1", "r2", "r3", "sp", "lr", "pc"]


def gdb_step_and_read_pc(backend) -> int:
    # Mirror GDBBackend.resume()'s pattern: send 's' manually and only
    # wait for the ack, then read the stop-reply ourselves --
    # _send_command()'s generic reply-handling would otherwise treat a
    # step's own stop-reply as an "out of band" packet to discard and
    # hang waiting for a different one that never comes.
    cmd = b"s"
    packet = b"$" + cmd + b"#" + _gdb_checksum(cmd)
    backend._socket.sendall(packet)
    while True:
        ack = backend._sock_file.read(1)
        if ack == b"+":
            break
        elif ack == b"-":
            backend._socket.sendall(packet)
        elif not ack:
            raise RuntimeError("connection closed by remote during step")
    backend._wait_for_packet()
    backend._is_running = False
    return backend.read_register("pc")


def _gdb_checksum(data: bytes) -> bytes:
    c = sum(data) % 256
    return f"{c:02x}".encode("ascii")


def openocd_step_and_read_pc(backend) -> int:
    backend("step", decode=False)
    resp = backend("reg pc", decode=False).decode(errors="replace")
    return int(resp.strip().split()[-1], 16)


def read_core_regs_gdb(backend) -> dict:
    return {r: backend.read_register(r) for r in CORE_REGS}


def read_core_regs_openocd(backend) -> dict:
    out = {}
    for r in CORE_REGS:
        resp = backend(f"reg {r}", decode=False).decode(errors="replace")
        out[r] = int(resp.strip().split()[-1], 16)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-steps", type=int, default=0,
                     help="0 (default) = open-ended, run until divergence or Ctrl-C")
    ap.add_argument("--start", type=lambda s: int(s, 0), default=None,
                     help="entry point to reset-and-halt to before stepping (default: read from zelda-bank1.bin)")
    args = ap.parse_args()

    entry = args.start if args.start is not None else entry_point_from_bank1(BANK1_PATH)
    print(f"Start address: {entry:#010x}")

    from gnwmanager.ocdbackend.gdb_backend import GDBBackend
    from gnwmanager.ocdbackend import OCDBackend

    qemu = GDBBackend()
    hw = OCDBackend["openocd"]()

    print("Connecting to QEMU...")
    qemu.open()
    print("Connecting to real hardware...")
    hw.open()

    try:
        print("Resetting both targets to entry point...")
        reset_and_run_to(qemu, entry)
        reset_and_run_to(hw, entry)

        step_iter = range(args.max_steps) if args.max_steps > 0 else itertools.count()
        label = f"up to {args.max_steps} steps" if args.max_steps > 0 else "open-ended (Ctrl-C to abort)"
        print(f"Single-stepping {label}, comparing PC after each...")

        diverged = False
        i = -1
        for i in step_iter:
            try:
                pc_q = gdb_step_and_read_pc(qemu)
            except (socket.timeout, OSError) as e:
                print(f"step {i}: QEMU step failed: {e}")
                break
            pc_h = openocd_step_and_read_pc(hw)

            if pc_q != pc_h:
                diverged = True
                print(f"\nDIVERGED at step {i}: QEMU PC={pc_q:#010x}  HW PC={pc_h:#010x}")
                print("QEMU core regs:", {k: hex(v) for k, v in read_core_regs_gdb(qemu).items()})
                print("HW   core regs:", {k: hex(v) for k, v in read_core_regs_openocd(hw).items()})
                break

            if (i + 1) % 20 == 0:
                print(f"  step {i + 1}: still matching, PC={pc_q:#010x}")

        if not diverged:
            print(f"\nNo divergence found through step {i}. Both PC={pc_q:#010x}")
    finally:
        # Always tears down both connections (OpenOCDBackend.close()
        # kills its openocd subprocess) even on exception/Ctrl-C -- an
        # orphaned openocd process holds the SWD lock and breaks every
        # subsequent script's connection attempt.
        qemu.close()
        hw.close()


if __name__ == "__main__":
    main()
