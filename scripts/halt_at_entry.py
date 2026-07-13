"""
halt_at_entry.py -- deterministic reset-and-halt-at-the-real-entry-point,
built entirely on top of gnwmanager's existing backend primitives (no
changes to the gnwmanager package itself).

reset_and_halt() (both GDBBackend's reset()+halt() two-step, and
OpenOCD's "reset halt") races an async halt request against however much
code the target executes before that request is serviced -- confirmed via
a real reset-and-compare snapshot session where RCC's peripheral-enable
registers already held live values by the time the "halt" landed. A real
breakpoint at the target's own first instruction removes the race
entirely: the target can only ever stop there.

Works against both backends already used by scripts/snapshot_registers.py:
- GDBBackend (QEMU): uses the raw gdb-remote Z1/z1 breakpoint packets via
  its existing _send_command()/socket -- GDBBackend doesn't expose a
  public breakpoint method, so this pokes its (already-python, not
  actually private) internals directly rather than patching the package.
- OpenOCDBackend (real hardware): uses its existing __call__ passthrough
  to issue raw OpenOCD Tcl commands ("bp", "rbp", "wait_halt"), which is
  exactly what that class's other methods already do internally.
"""
import socket


def entry_point_from_bank1(bank1_path) -> int:
    """Read the real reset-vector entry point (word at offset 4) out of a
    bank1 image, matching how the CPU itself picks its first PC on reset.
    Returns the address with the Thumb bit stripped."""
    with open(bank1_path, "rb") as f:
        f.seek(4)
        raw = int.from_bytes(f.read(4), "little")
    return raw & ~1


def reset_and_run_to(backend, addr: int, timeout: float = 5.0):
    """Reset backend's target and let it run until it hits addr, halting
    there deterministically via a real breakpoint instead of racing an
    async halt request."""
    cls_name = type(backend).__name__

    if cls_name == "GDBBackend":
        # Z-packets (and reset()'s internal command) require the target
        # halted -- it may already be running (gnwmanager's
        # GDBBackend.open() auto-resumes on connect to match physical
        # probe behavior). Halt first so this doesn't race.
        if backend._is_running:
            backend.halt()
        _gdb_set_bp(backend, addr)
        try:
            backend.reset()  # target was halted above, so this leaves it halted
            _gdb_resume_no_wait(backend)
            _gdb_wait_for_stop(backend, timeout)
        finally:
            _gdb_clear_bp(backend, addr)

    elif cls_name == "OpenOCDBackend":
        backend(f"bp 0x{addr:08x} 2 hw", decode=False)
        try:
            backend("reset run", decode=False)  # resets and resumes in one step
            backend(f"wait_halt {int(timeout * 1000)}", decode=False)
        finally:
            backend(f"rbp 0x{addr:08x}", decode=False)

    else:
        raise NotImplementedError(f"reset_and_run_to() not implemented for backend {cls_name}")


def _gdb_set_bp(backend, addr: int):
    reply = backend._send_command(f"Z1,{addr:x},2".encode("ascii"))
    if reply != b"OK":
        raise RuntimeError(f"failed to set breakpoint at {addr:#x}: {reply}")


def _gdb_clear_bp(backend, addr: int):
    reply = backend._send_command(f"z1,{addr:x},2".encode("ascii"))
    if reply != b"OK":
        raise RuntimeError(f"failed to clear breakpoint at {addr:#x}: {reply}")


def _gdb_resume_no_wait(backend):
    # Same as GDBBackend.resume(): send 'c' and only wait for the ack,
    # not the (blocking-until-stop) reply -- we do that ourselves next
    # with our own timeout via _gdb_wait_for_stop.
    backend.resume()


def _gdb_wait_for_stop(backend, timeout: float):
    old_timeout = backend._socket.gettimeout()
    backend._socket.settimeout(timeout)
    try:
        backend._wait_for_packet()
    except (socket.timeout, OSError):
        raise RuntimeError(f"timed out waiting for breakpoint hit after {timeout}s")
    finally:
        backend._socket.settimeout(old_timeout)
        # A socket timeout permanently wedges Python's buffered
        # makefile() wrapper (further reads raise "cannot read from
        # timed out object" even after resetting the timeout) --
        # recreate it so the caller's cleanup (clearing the breakpoint)
        # can still read replies.
        backend._sock_file = backend._socket.makefile("rb", buffering=8192)
    backend._is_running = False
