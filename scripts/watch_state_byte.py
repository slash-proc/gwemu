#!/usr/bin/env python3
"""
watch_state_byte.py -- repeatedly hit a write-watchpoint on the boot
state byte at r4+0 (0x2000ab90, same base pointer as the superloop's
[r4+9]/[r4+0x60] fields -- see docs/session-2026-07-12-breakpoint-
lockstep-tracing.md's "Follow-up session (same day, part 3)" section)
and log the value written + PC at each hit, on both QEMU and real
hardware, to find what actually advances the boot state machine past
state 0 (FUN_0800ea00's dispatch table).

Usage:
    ./scripts/watch_state_byte.py --hits 15
"""
import argparse
import socket
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path.home() / "Nerd" / "git" / "gnwmanager"))

STATE_ADDR = 0x2000AB90


def watch_gdb(backend, hits, timeout):
    backend.reset_and_halt()
    reply = backend._send_command(f"Z2,{STATE_ADDR:x},1".encode("ascii"))
    if reply != b"OK":
        raise RuntimeError(f"failed to set watchpoint: {reply}")
    try:
        for i in range(hits):
            try:
                backend.resume()
                old_timeout = backend._socket.gettimeout()
                backend._socket.settimeout(timeout)
                try:
                    backend._wait_for_packet()
                finally:
                    backend._socket.settimeout(old_timeout)
            except (socket.timeout, OSError):
                print(f"  [hit {i}] timed out -- no more writes within {timeout}s")
                backend._sock_file = backend._socket.makefile("rb", buffering=8192)
                return
            pc = backend.read_register("pc")
            val = backend.read_memory(STATE_ADDR, 1)[0]
            print(f"  [hit {i}] state={val:#04x}  written from PC={pc:#010x}")
    finally:
        try:
            backend._send_command(f"z2,{STATE_ADDR:x},1".encode("ascii"))
        except (socket.timeout, OSError):
            backend._sock_file = backend._socket.makefile("rb", buffering=8192)


def watch_openocd(backend, hits, timeout):
    backend("reset halt", decode=False)
    backend(f"wp 0x{STATE_ADDR:08x} 1 w", decode=False)
    try:
        for i in range(hits):
            backend("resume", decode=False)
            backend(f"wait_halt {int(timeout * 1000)}", decode=False)
            resp = backend("reg pc", decode=False).decode(errors="replace")
            pc = int(resp.strip().split()[-1], 16)
            val = backend.read_memory(STATE_ADDR, 1)[0]
            print(f"  [hit {i}] state={val:#04x}  written from PC={pc:#010x}")
    finally:
        backend(f"rwp 0x{STATE_ADDR:08x}", decode=False)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hits", type=int, default=15)
    ap.add_argument("--timeout", type=float, default=10.0)
    args = ap.parse_args()

    from gnwmanager.ocdbackend.gdb_backend import GDBBackend
    from gnwmanager.ocdbackend import OCDBackend

    print("=== QEMU ===")
    qemu = GDBBackend()
    qemu.open()
    try:
        watch_gdb(qemu, args.hits, args.timeout)
    finally:
        qemu.close()

    print("\n=== Real hardware ===")
    hw = OCDBackend["openocd"]()
    hw.open()
    try:
        watch_openocd(hw, args.hits, args.timeout)
    finally:
        hw.close()


if __name__ == "__main__":
    main()
