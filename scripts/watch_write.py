#!/usr/bin/env python3
"""
watch_write.py -- reset the target, set a write watchpoint at a given
address on QEMU or real hardware (via gnwmanager's backend), let it run
from reset, and report whether/where a write happens within a bounded
timeout. Turns a one-off manual "watched hardware for ~12s, it was never
written" trace into a repeatable, scripted, reset-to-reset-comparable
check -- always resets first so results aren't polluted by whatever ad
hoc state a prior connection/attach left the target in.

Usage:
    ./scripts/watch_write.py --qemu 0x2000abf0
    ./scripts/watch_write.py 0x2000abf0 --timeout 8
"""
import argparse
import socket
import sys
from pathlib import Path

sys.path.insert(0, str(Path.home() / "Nerd" / "git" / "gnwmanager"))


def watch_gdb(backend, addr: int, length: int, timeout: float):
    # Reset and halt first (at whatever point reset_and_halt lands --
    # good enough here since we just need a known starting point before
    # arming the watchpoint, not a precise entry-point halt) so the
    # watchpoint is armed before any code has had a chance to run.
    backend.reset_and_halt()
    reply = backend._send_command(f"Z2,{addr:x},{length:x}".encode("ascii"))
    if reply != b"OK":
        raise RuntimeError(f"failed to set watchpoint at {addr:#x}: {reply}")
    try:
        backend.resume()
        old_timeout = backend._socket.gettimeout()
        backend._socket.settimeout(timeout)
        try:
            backend._wait_for_packet()
            hit = True
        except (socket.timeout, OSError):
            hit = False
        finally:
            backend._socket.settimeout(old_timeout)
            # A socket timeout permanently wedges Python's buffered
            # makefile() wrapper (further reads raise "cannot read from
            # timed out object" even after resetting the timeout) --
            # recreate it so subsequent commands (halt/clear-watchpoint)
            # can still read replies.
            backend._sock_file = backend._socket.makefile("rb", buffering=8192)
        if hit:
            backend._is_running = False
        else:
            # Still running -- interrupt it so we can safely read state
            # and clear the watchpoint below.
            backend.halt()
        pc = backend.read_register("pc") if hit else None
        return hit, pc
    finally:
        try:
            backend._send_command(f"z2,{addr:x},{length:x}".encode("ascii"))
        except Exception as e:
            print(f"  (warning: failed to clear watchpoint, non-fatal: {e})")


def watch_openocd(backend, addr: int, length: int, timeout: float):
    backend("reset halt", decode=False)
    backend(f"wp 0x{addr:08x} {length} w", decode=False)
    try:
        backend("resume", decode=False)
        resp = backend(f"wait_halt {int(timeout * 1000)}", decode=False).decode(errors="replace")
        hit = "timed out" not in resp.lower()
        pc = None
        if hit:
            try:
                reg_resp = backend("reg pc", decode=False).decode(errors="replace")
                pc = int(reg_resp.strip().split()[-1], 16)
            except Exception as e:
                print(f"  (warning: failed to parse PC from {reg_resp!r}: {e})")
        return hit, pc
    finally:
        backend(f"rwp 0x{addr:08x}", decode=False)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("address", type=lambda s: int(s, 0))
    ap.add_argument("--qemu", action="store_true")
    ap.add_argument("--length", type=int, default=4)
    ap.add_argument("--timeout", type=float, default=5.0)
    args = ap.parse_args()

    if args.qemu:
        from gnwmanager.ocdbackend.gdb_backend import GDBBackend
        backend = GDBBackend()
    else:
        from gnwmanager.ocdbackend import OCDBackend
        backend = OCDBackend["openocd"]()

    backend.open()
    try:
        target = "QEMU" if args.qemu else "real hardware"
        print(f"Watching {args.address:#010x} ({args.length} bytes) for a write on {target}, "
              f"timeout {args.timeout}s...")

        if args.qemu:
            hit, pc = watch_gdb(backend, args.address, args.length, args.timeout)
        else:
            hit, pc = watch_openocd(backend, args.address, args.length, args.timeout)

        if hit:
            print(f"WRITE DETECTED. Halted at PC={pc:#010x}" if pc is not None else "WRITE DETECTED (PC unknown)")
        else:
            print(f"No write observed within {args.timeout}s.")
    finally:
        # Always tears down the connection (for OpenOCDBackend, this
        # kills the openocd subprocess) even on exception -- an orphaned
        # openocd process holds the SWD lock and breaks every subsequent
        # script's connection attempt.
        backend.close()


if __name__ == "__main__":
    main()
