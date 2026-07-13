#!/usr/bin/env python3
"""
step_init_calls.py -- FUN_0801b04c (the runtime's init-array/constructor
dispatcher) calls a table of init functions one at a time via a fixed
call site at 0x0801b062. This resets both QEMU and real hardware to the
real entry point, sets a breakpoint at that call site, and repeatedly
resumes+halts on each hit, printing core registers each time (especially
r0, the argument passed to that iteration's init function, and the
computed callee address) so we can see exactly which constructor call
is next in sequence and compare the two targets call-by-call.

Usage:
    ./scripts/step_init_calls.py --hits 15
"""
import argparse
import socket
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path.home() / "Nerd" / "git" / "gnwmanager"))

from halt_at_entry import entry_point_from_bank1, reset_and_run_to  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
BANK1_PATH = REPO_ROOT / "backup" / "qemu-images" / "zelda-bank1-patched.bin"
CALL_SITE = 0x0801B062

REGS = ["r0", "r1", "r2", "r3", "lr", "sp"]


def gdb_set_bp(backend, addr):
    reply = backend._send_command(f"Z1,{addr:x},2".encode("ascii"))
    if reply != b"OK":
        raise RuntimeError(f"failed to set breakpoint: {reply}")


def gdb_clear_bp(backend, addr):
    backend._send_command(f"z1,{addr:x},2".encode("ascii"))


def gdb_step_one(backend):
    packet = b"$s#" + bytes(f"{sum(b's') % 256:02x}".encode())
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


def gdb_continue_and_wait(backend, timeout, addr=None):
    # If a hardware breakpoint is still armed exactly at the current PC,
    # a bare "continue" can immediately re-trap without executing
    # anything -- OpenOCD's "resume" transparently steps over an armed
    # breakpoint at the current PC before continuing, but our raw
    # gdb-remote continue does not. Do that step-over explicitly so this
    # matches OpenOCD's behavior instead of spinning in place forever.
    if addr is not None:
        gdb_clear_bp(backend, addr)
        gdb_step_one(backend)
        gdb_set_bp(backend, addr)
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
        backend._sock_file = backend._socket.makefile("rb", buffering=8192)
    backend._is_running = False
    if not hit:
        backend.halt()
    return hit


def openocd_continue_and_wait(backend, timeout):
    backend("resume", decode=False)
    resp = backend(f"wait_halt {int(timeout * 1000)}", decode=False).decode(errors="replace")
    return "timed out" not in resp.lower()


def snapshot_gdb(backend):
    return {r: backend.read_register(r) for r in REGS} | {"pc": backend.read_register("pc")}


def snapshot_openocd(backend, retries=5):
    out = {}
    for r in REGS + ["pc"]:
        for attempt in range(retries):
            resp = backend(f"reg {r}", decode=False).decode(errors="replace").strip()
            tokens = resp.split()
            if tokens:
                break
            time.sleep(0.1 * (attempt + 1))  # give the halt a moment to fully settle
        else:
            raise RuntimeError(f"empty 'reg {r}' response after {retries} retries -- target may not be halted")
        out[r] = int(tokens[-1], 16)
    return out


def run_target(backend, is_qemu, entry, hits, timeout):
    reset_and_run_to(backend, entry)

    if is_qemu:
        gdb_set_bp(backend, CALL_SITE)
    else:
        backend(f"bp 0x{CALL_SITE:08x} 2 hw", decode=False)

    results = []
    try:
        for i in range(hits):
            if is_qemu:
                hit = gdb_continue_and_wait(backend, timeout, addr=CALL_SITE)
            else:
                hit = openocd_continue_and_wait(backend, timeout)
            if not hit:
                print(f"  [iteration {i}] timed out waiting for call site -- loop likely exited")
                break
            regs = snapshot_gdb(backend) if is_qemu else snapshot_openocd(backend)
            # r0 = piVar1+1 (arg to callee), r1 typically holds piVar1 (current table entry) per the
            # decompiled loop body -- record both plus lr/sp for a fuller picture.
            results.append(regs)
            print(f"  [iteration {i}] r0={regs['r0']:#010x} r1={regs['r1']:#010x} "
                  f"r2={regs['r2']:#010x} r3={regs['r3']:#010x} lr={regs['lr']:#010x} sp={regs['sp']:#010x}")
    finally:
        if is_qemu:
            gdb_clear_bp(backend, CALL_SITE)
        else:
            backend(f"rbp 0x{CALL_SITE:08x}", decode=False)

    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hits", type=int, default=15)
    ap.add_argument("--timeout", type=float, default=5.0)
    args = ap.parse_args()

    entry = entry_point_from_bank1(BANK1_PATH)
    print(f"Entry: {entry:#010x}  Call site breakpoint: {CALL_SITE:#010x}")

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
        # Always tears down the openocd subprocess (OpenOCDBackend.close()
        # sends "exit" + terminates the process) even if run_target()
        # raised -- an orphaned openocd process holds the SWD lock and
        # breaks every subsequent script's connection attempt.
        hw.close()

    print("\n=== Comparison ===")
    for i in range(min(len(qemu_results), len(hw_results))):
        q, h = qemu_results[i], hw_results[i]
        mismatches = [r for r in REGS if q[r] != h[r]]
        status = "MATCH" if not mismatches else f"DIVERGE on {mismatches}"
        print(f"  iteration {i}: QEMU r0={q['r0']:#010x} r1={q['r1']:#010x}  "
              f"HW r0={h['r0']:#010x} r1={h['r1']:#010x}  [{status}]")
    if len(qemu_results) != len(hw_results):
        print(f"  Different iteration counts before timeout/exit: QEMU={len(qemu_results)} HW={len(hw_results)}")


if __name__ == "__main__":
    main()
