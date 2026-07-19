#!/usr/bin/env python3
"""Capture a running gnw-h7b0 instance's CPU/RAM/peripheral state and inject
it into a different (halted) instance.

Unlike QEMU's own savevm/loadvm (which snapshot ONE instance to reload into
itself later), this captures guest-visible state from one running target
over the GDB Remote Serial Protocol and writes it into a DIFFERENT target --
e.g. to compare QEMU's execution against real hardware's by transplanting
real hardware's state into QEMU, or (as tested here) QEMU-to-QEMU.

CAVEAT -- read before trusting results: this only transplants what's
guest-visible via GDB register/memory reads (r0-r12, sp, lr, pc, the RAM
regions below, and -- as of this revision -- peripheral MMIO registers). It
cannot capture QEMU device-model *internal* state that isn't memory-mapped
(a timer's countdown position, a DMA controller's internal descriptor-walk
state, NVIC bookkeeping beyond its memory-mapped registers, etc.). Injected
state may therefore behave subtly differently than either a pure fresh boot
or the original source's continued execution. Treat this as a diagnostic aid
("does execution from roughly this point behave differently"), not a
faithful clone.

A real experiment (2026-07-19) confirmed why peripheral state matters: CPU
registers + RAM alone, injected into a QEMU instance halted at its own reset
vector (so its peripherals are all still at power-on-reset defaults), froze
execution completely -- the injected code's next instructions assumed
peripheral configuration (LTDC/GPIO/RCC/etc.) that real hardware had already
set up by that point in boot, but fresh QEMU peripherals hadn't. This
revision adds peripheral-register capture/injection to address that gap.

PERIPHERAL REGISTER SAFETY -- read before using --peripherals:
Not every readable peripheral register is safe to blindly write back into a
different instance. Three real hazards, all handled by NOT injecting the
affected registers (captured for reference, but skipped on inject):
  1. Read-only registers (SVD <access>read-only</access>) -- can't be
     written at all; writing would either no-op or raise a bus fault
     depending on the device model.
  2. Write-only registers (SVD <access>write-only</access>) -- reading these
     is meaningless (returns 0 or a shadow value, not real state), and
     writing the captured (meaningless) value back could trigger unintended
     side effects (this project's own history: EGR-style "event generation"
     pulse registers, already documented in hw/misc/gnw_h7b0_tim1.c and
     gnw_h7b0_rcc.h from real bugs fixed this session, re-trigger an action
     on any nonzero write).
  3. Write-1-to-clear status/flag-clear registers that are formally
     read-write per the SVD but whose bits latch a "this happened" flag
     that clears on writing 1 (SR/ISR/ICR/IFCR-style registers, and EGR
     registers specifically). Blindly writing back a captured value (which
     reflects bits that were SET, i.e. 1) into one of these on the target
     would clear real pending flags there, corrupting live state rather
     than replicating it. Detected by a conservative name-suffix denylist
     (SKIP_INJECT_NAME_SUFFIXES below) since the SVD's <access> field alone
     doesn't distinguish "plain read-write config register" from "write-1-
     to-clear status register" -- both are formally read-write.
These are captured into the manifest regardless (for inspection/diagnostic
value) but never written during inject() unless --force-unsafe-peripherals
is passed (NOT recommended -- only for deliberate, informed experimentation).

Injection order: RAM first, then peripheral registers, then core CPU
registers (including PC) last. Rationale: peripherals should be configured
before the CPU resumes running code that might immediately read them; core
registers (especially PC/SP) go last so nothing executes mid-injection with
a half-restored world.

Usage:
    Capture from a running source instance (GDB port 1234), without
    resetting it first (captures whatever state it's currently in),
    including peripheral registers:
        ./scripts/state_transplant.py capture --port 1234 --out /tmp/snap1 --peripherals

    Capture core+RAM only (original, faster, smaller -- no peripheral walk):
        ./scripts/state_transplant.py capture --port 1234 --out /tmp/snap1

    Inject into a target instance (GDB port 1235) that is currently
    halted (e.g. launched with `-S`), then resume it. Peripheral registers
    in the snapshot (if captured) are injected automatically unless
    --no-peripherals is passed:
        ./scripts/state_transplant.py inject --port 1235 --snapshot /tmp/snap1 --resume

Snapshot format: a directory containing manifest.json (registers + region
descriptors + peripheral register list) plus one <region>.bin raw dump per
captured RAM region. Peripheral register values are stored inline in
manifest.json (small enough -- a few KB of JSON for ~2100 registers across
75 peripherals, per stm32h7b0-diag's own register-audit tooling's count of
this same SVD).
"""
import argparse
import json
import re
import struct
import sys
from pathlib import Path
from xml.etree import ElementTree as ET

sys.path.insert(0, "/home/doug/Nerd/git/gnwmanager")
from gnwmanager.ocdbackend.gdb_backend import GDBBackend  # noqa: E402

# gnw-h7b0's memory map (include/hw/arm/gnw_h7b0_soc.h) -- the three RAM
# regions worth transplanting. Peripheral MMIO ranges are handled
# separately below (see --peripherals).
REGIONS = {
    "itcm": (0x00000000, 64 * 1024),
    "dtcm": (0x20000000, 128 * 1024),
    "axisram": (0x24000000, (256 + 384 + 384) * 1024),
}

REGISTERS = [f"r{i}" for i in range(13)] + ["sp", "lr", "pc"]
# NOTE: xpsr is deliberately excluded -- gnwmanager's GDBBackend maps it to
# GDB p-packet register index 0x10, which QEMU's gnw-h7b0 gdbstub target
# description doesn't recognize for this register set (returns GDB error
# E14). Not fixed here (out of scope, shared gnwmanager code) -- core
# registers + RAM are captured/injected regardless.

CHUNK_SIZE = 32 * 1024

SVD_PATH = Path("/home/doug/Nerd/git/qemu-gnw/STM32H7B0.svd")
MAX_PERIPHERAL_BLOCK_SIZE = 0x400  # cap per-peripheral read span, matches
                                    # stm32h7b0-diag/host/register_audit.py's
                                    # own MAX_BLOCK_SIZE convention.

# Conservative denylist for injection (not capture) -- register NAME suffixes
# that indicate write-1-to-clear/event-pulse semantics even when the SVD
# marks them formally read-write. See module docstring hazard #3.
SKIP_INJECT_NAME_SUFFIXES = ("SR", "ISR", "ICR", "IFCR", "EGR")


def _read_memory_chunked(backend, addr, size):
    out = bytearray()
    remaining = size
    off = 0
    while remaining > 0:
        n = min(CHUNK_SIZE, remaining)
        out += backend.read_memory(addr + off, n)
        off += n
        remaining -= n
    return bytes(out)


def _write_memory_chunked(backend, addr, data):
    off = 0
    while off < len(data):
        chunk = data[off:off + CHUNK_SIZE]
        backend.write_memory(addr + off, chunk)
        off += len(chunk)


# ---------------------------------------------------------------------------
# SVD-driven peripheral/register enumeration -- ported from stm32h7b0-diag's
# host/register_audit.py::load_svd (same file, same parsing approach; not
# imported directly since that's a separate repo/project, but the logic is
# copied deliberately to stay consistent with that project's proven SVD
# handling rather than reinventing it).
# ---------------------------------------------------------------------------
class SvdRegister:
    __slots__ = ("name", "offset", "access")

    def __init__(self, name, offset, access):
        self.name = name
        self.offset = offset
        self.access = access  # 'read-only' | 'write-only' | 'read-write' | None


def load_svd_peripherals(svd_path: Path):
    """Returns list of (name, base, size, [SvdRegister, ...]), deduped by
    base address (derivedFrom siblings like GPIOB..K share one definition
    at different bases in this SVD's structure -- but a peripheral element
    with its OWN <registers> block, like each GPIO port has, is kept as its
    own entry; only exact-duplicate base addresses are deduped)."""
    tree = ET.parse(svd_path)
    root = tree.getroot()

    peripherals = []
    seen_bases = set()
    for p in root.find("peripherals").findall("peripheral"):
        name = p.findtext("name")
        base_text = p.findtext("baseAddress")
        if name is None or base_text is None:
            continue
        base = int(base_text, 0)
        if base in seen_bases:
            continue

        size = MAX_PERIPHERAL_BLOCK_SIZE
        ab = p.find("addressBlock")
        if ab is not None:
            size_text = ab.findtext("size")
            if size_text is not None:
                size = min(int(size_text, 0), MAX_PERIPHERAL_BLOCK_SIZE)

        regs = []
        regs_el = p.find("registers")
        if regs_el is not None:
            for r in regs_el.findall("register"):
                off_text = r.findtext("addressOffset")
                if off_text is None:
                    continue
                reg_name = r.findtext("name") or f"@{off_text}"
                access = r.findtext("access")  # may be None -> treat as read-write
                regs.append(SvdRegister(reg_name, int(off_text, 0), access))

        if not regs:
            continue  # nothing to walk (e.g. a peripheral with only a size block)

        seen_bases.add(base)
        peripherals.append((name, base, size, regs))

    return sorted(peripherals, key=lambda x: x[1])


def _skip_inject(reg: SvdRegister) -> bool:
    if reg.access == "read-only":
        return True
    if reg.access == "write-only":
        return True
    if reg.name.upper().endswith(SKIP_INJECT_NAME_SUFFIXES):
        return True
    return False


def capture_peripherals(backend):
    """Read every peripheral register this SVD declares, via one whole-block
    read per peripheral (matches stm32h7b0-diag/host/register_audit.py's own
    approach -- far fewer round trips than one read per register, and this
    project's register-audit tooling already validated that pattern works
    cleanly against both QEMU's gdbstub and real hardware's OpenOCD).

    Returns {peripheral_name: {"base": int, "registers": {reg_name: {"offset":
    int, "value": int, "access": str|None, "skip_inject": bool}}}}.
    """
    peripherals = load_svd_peripherals(SVD_PATH)
    result = {}
    for name, base, size, regs in peripherals:
        try:
            block = backend.read_memory(base, size)
        except Exception as e:  # noqa: BLE001 -- record, don't abort the whole capture
            print(f"  warning: failed to read peripheral {name} @ 0x{base:08x}: {e}",
                  file=sys.stderr)
            continue
        reg_entries = {}
        for reg in regs:
            if reg.offset + 4 > len(block):
                continue  # register offset outside the (capped) read block
            value = struct.unpack_from("<I", block, reg.offset)[0]
            reg_entries[reg.name] = {
                "offset": reg.offset,
                "value": value,
                "access": reg.access,
                "skip_inject": _skip_inject(reg),
            }
        result[name] = {"base": base, "registers": reg_entries}
    return result


def inject_peripherals(backend, peripheral_manifest, force_unsafe: bool):
    written, skipped = 0, 0
    for pname, pdata in peripheral_manifest.items():
        base = pdata["base"]
        for rname, rdata in pdata["registers"].items():
            if rdata["skip_inject"] and not force_unsafe:
                skipped += 1
                continue
            addr = base + rdata["offset"]
            try:
                backend.write_memory(addr, struct.pack("<I", rdata["value"]))
                written += 1
            except Exception as e:  # noqa: BLE001 -- one bad register shouldn't abort the rest
                print(f"  warning: failed to write {pname}.{rname} @ 0x{addr:08x}: {e}",
                      file=sys.stderr)
    print(f"peripheral injection: {written} registers written, {skipped} skipped "
          f"(read-only/write-only/status-flag-clear -- see module docstring)",
          file=sys.stderr)


def capture(port: int, out_dir: str, do_reset: bool, do_peripherals: bool):
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)

    backend = GDBBackend()
    backend.host = "localhost"
    backend.port = port
    backend.open()
    try:
        if do_reset:
            backend.reset()
        backend.halt()

        regs = {name: backend.read_register(name) for name in REGISTERS}

        region_manifest = {}
        for name, (addr, size) in REGIONS.items():
            print(f"capturing {name}: 0x{addr:08x} + {size} bytes...", file=sys.stderr)
            data = _read_memory_chunked(backend, addr, size)
            (out / f"{name}.bin").write_bytes(data)
            region_manifest[name] = {"addr": addr, "size": size, "file": f"{name}.bin"}

        peripheral_manifest = {}
        if do_peripherals:
            print("capturing peripheral registers (SVD-driven walk)...", file=sys.stderr)
            peripheral_manifest = capture_peripherals(backend)
            n_regs = sum(len(p["registers"]) for p in peripheral_manifest.values())
            print(f"  captured {n_regs} registers across {len(peripheral_manifest)} peripherals",
                  file=sys.stderr)

        manifest = {
            "registers": regs,
            "regions": region_manifest,
            "peripherals": peripheral_manifest,
        }
        (out / "manifest.json").write_text(json.dumps(manifest, indent=2))
        print(f"wrote snapshot to {out}", file=sys.stderr)
    finally:
        backend.resume()
        backend.close()


def inject(port: int, snapshot_dir: str, do_resume: bool, do_peripherals: bool,
           force_unsafe: bool):
    snap = Path(snapshot_dir)
    manifest = json.loads((snap / "manifest.json").read_text())

    backend = GDBBackend()
    backend.host = "localhost"
    backend.port = port
    backend.open()
    try:
        backend.halt()

        # Order: RAM, then peripherals, then core registers (incl. PC) last
        # -- see module docstring's "Injection order" section.
        for name, desc in manifest["regions"].items():
            data = (snap / desc["file"]).read_bytes()
            print(f"injecting {name}: 0x{desc['addr']:08x} + {len(data)} bytes...", file=sys.stderr)
            _write_memory_chunked(backend, desc["addr"], data)
            check = backend.read_memory(desc["addr"], min(64, len(data)))
            if check != data[:len(check)]:
                raise RuntimeError(f"readback mismatch for region {name} -- write did not stick")

        if do_peripherals and manifest.get("peripherals"):
            inject_peripherals(backend, manifest["peripherals"], force_unsafe)
        elif do_peripherals:
            print("no peripheral data in this snapshot (captured without --peripherals)",
                  file=sys.stderr)

        for name, val in manifest["registers"].items():
            backend.write_register(name, val)
            readback = backend.read_register(name)
            if readback != val:
                raise RuntimeError(f"register {name} readback mismatch: wrote 0x{val:08x}, read 0x{readback:08x}")

        print("injection complete, all writes verified via readback", file=sys.stderr)

        if do_resume:
            backend.resume()
            print("resumed target", file=sys.stderr)
    finally:
        backend.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    cap = sub.add_parser("capture", help="capture state from a running source instance")
    cap.add_argument("--port", type=int, required=True, help="source instance's GDB port")
    cap.add_argument("--out", required=True, help="output snapshot directory")
    cap.add_argument("--reset", action="store_true", help="reset the source before capturing (for a reproducible starting point)")
    cap.add_argument("--peripherals", action="store_true", help="also capture every SVD-declared peripheral register")

    inj = sub.add_parser("inject", help="inject a captured snapshot into a halted target instance")
    inj.add_argument("--port", type=int, required=True, help="target instance's GDB port")
    inj.add_argument("--snapshot", required=True, help="snapshot directory from a prior capture")
    inj.add_argument("--resume", action="store_true", help="resume the target after injecting")
    inj.add_argument("--no-peripherals", action="store_true", help="skip peripheral injection even if the snapshot has it")
    inj.add_argument("--force-unsafe-peripherals", action="store_true",
                      help="also write read-only/write-only/status-flag-clear registers -- NOT recommended, see module docstring")

    args = ap.parse_args()
    if args.cmd == "capture":
        capture(args.port, args.out, args.reset, args.peripherals)
    elif args.cmd == "inject":
        inject(args.port, args.snapshot, args.resume, not args.no_peripherals,
               args.force_unsafe_peripherals)


if __name__ == "__main__":
    main()
