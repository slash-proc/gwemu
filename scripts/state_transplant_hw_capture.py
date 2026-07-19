#!/usr/bin/env python3
"""Capture real Game & Watch hardware's CURRENT state (non-destructively --
no reset) into a state_transplant.py-compatible snapshot directory, so it
can be injected into a QEMU gnw-h7b0 instance via:
    scripts/state_transplant.py inject --port <target> --snapshot <dir> --resume

Deliberately does NOT call reset() -- only .halt()/.resume() around the
reads, to avoid the real-hardware standby-drop wedge risk a reset carries.
Captures whatever state the device is currently in.
"""
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
GNWMANAGER_PATH = os.environ.get("GNWMANAGER_PATH")
if GNWMANAGER_PATH:
    sys.path.insert(0, GNWMANAGER_PATH)
try:
    from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend  # noqa: E402
except ImportError:
    sys.exit(
        "error: gnwmanager not importable. Install it (pip install gnwmanager) "
        "or set GNWMANAGER_PATH to a local checkout."
    )
from state_transplant import REGIONS, REGISTERS, _read_memory_chunked, capture_peripherals  # noqa: E402


def capture_hw(port: int, out_dir: str, do_peripherals: bool):
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)

    backend = OpenOCDBackend(port=port)
    backend.open()
    try:
        backend.halt()

        regs = {}
        for name in REGISTERS:
            regs[name] = backend.read_register(name)
        print(f"captured registers: pc=0x{regs['pc']:08x} sp=0x{regs['sp']:08x}", file=sys.stderr)

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

        manifest = {"registers": regs, "regions": region_manifest, "peripherals": peripheral_manifest}
        (out / "manifest.json").write_text(json.dumps(manifest, indent=2))
        print(f"wrote real-hardware snapshot to {out}", file=sys.stderr)
    finally:
        backend.resume()
        backend.close()
        print("resumed real hardware, left running", file=sys.stderr)


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=6666)
    ap.add_argument("--out", required=True)
    ap.add_argument("--peripherals", action="store_true", help="also capture every SVD-declared peripheral register")
    args = ap.parse_args()
    capture_hw(args.port, args.out, args.peripherals)
