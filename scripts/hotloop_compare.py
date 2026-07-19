#!/usr/bin/env python3
"""
hotloop_compare.py -- run scripts/hotloop_sample.py's PC-histogram idea
against QEMU *and* real hardware in the same pass, then diff them.

Why this is different from (and stronger than) sampling QEMU alone
(scripts/hotloop_sample.py): QEMU and the physical device are both
running the byte-for-byte identical zelda-patched.7z CFW image pair (see
CLAUDE.md's "HW runs full gnwmanager CFW" note and this project's whole
lockstep-tracing methodology) -- so a given PC address means the exact
same instruction on both targets. That makes the two histograms directly
comparable: an address that's genuinely a normal, expected idle/dispatch
loop should show up at a *similar relative rate* on both targets (real
hardware idles in its own WFI loop too). An address that's disproportion-
ately hotter on QEMU than on real hardware is a real, actionable lead --
it means QEMU's guest is spending far more of its instruction budget at
that PC than real hardware needs to, which is exactly the "device model
doesn't set a status/ready flag as promptly as real hardware, so
firmware spins more" bug class this project has repeatedly found (see
CHANGELOG's OSPI/SPI1/JPEG/RTC fixes) -- surfaced here procedurally
instead of by manual tracing.

Built entirely on gnwmanager's existing GDBBackend/OCDBackend primitives
(same pattern as scripts/checkpoint.py/step_init_calls.py) -- no changes
to the gnwmanager package itself, per CLAUDE.md.

Does NOT reset either target: both are assumed already running normally
(steady-state gameplay/idle), and this only halts briefly to read $pc
before resuming -- same "each connect briefly halts delivery of
peripheral input events" caveat as hotloop_sample.py applies doubly here
since we alternate between two targets.

Usage:
    ./scripts/hotloop_compare.py --samples 300 --top 20
"""
import argparse
import sys
import time
from pathlib import Path
from collections import Counter

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path.home() / "Nerd" / "git" / "gnwmanager"))


def sample_pc_qemu(backend):
    # read_register() already halts (if running) and resumes (if it was
    # running) around the read internally -- see GDBBackend.read_register().
    return backend.read_register("pc")


def sample_pc_hw(backend):
    backend("halt", decode=False)
    resp = backend("reg pc", decode=False).decode(errors="replace").strip()
    tokens = resp.split()
    pc = int(tokens[-1], 16) if tokens else None
    backend("resume", decode=False)
    return pc


def collect(sample_fn, backend, n, label):
    counts = Counter()
    failures = 0
    for i in range(n):
        try:
            pc = sample_fn(backend)
            if pc is None:
                failures += 1
                continue
            counts[pc] += 1
        except Exception as e:
            failures += 1
        if (i + 1) % 50 == 0:
            print(f"  [{label}] {i + 1}/{n}", file=sys.stderr)
    if failures:
        print(f"  [{label}] warning: {failures}/{n} samples failed", file=sys.stderr)
    return counts


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--samples", type=int, default=300,
                     help="samples per target (interleaved one-QEMU-then-one-HW at a time)")
    ap.add_argument("--top", type=int, default=20)
    args = ap.parse_args()

    from gnwmanager.ocdbackend.gdb_backend import GDBBackend
    from gnwmanager.ocdbackend import OCDBackend

    print("Connecting to QEMU (GDBBackend) and real hardware (OpenOCD)...",
          file=sys.stderr)
    qemu = GDBBackend()
    hw = OCDBackend["openocd"]()
    qemu.open()
    try:
        hw.open()
    except Exception as e:
        qemu.close()
        sys.exit(f"failed to open real-hardware backend: {e}\n"
                 f"(is a physical device connected and flashed with the "
                 f"zelda-patched.7z CFW pair?)")

    try:
        print(f"Interleaved sampling, {args.samples} samples per target "
              f"(each sample briefly halts that target)...", file=sys.stderr)
        qemu_counts = Counter()
        hw_counts = Counter()
        qemu_fail = hw_fail = 0
        for i in range(args.samples):
            try:
                pc = sample_pc_qemu(qemu)
                if pc is not None:
                    qemu_counts[pc] += 1
                else:
                    qemu_fail += 1
            except Exception:
                qemu_fail += 1
            try:
                pc = sample_pc_hw(hw)
                if pc is not None:
                    hw_counts[pc] += 1
                else:
                    hw_fail += 1
            except Exception:
                hw_fail += 1
            if (i + 1) % 50 == 0:
                print(f"  {i + 1}/{args.samples}", file=sys.stderr)
    finally:
        qemu.close()
        hw.close()

    if qemu_fail:
        print(f"warning: {qemu_fail}/{args.samples} QEMU samples failed", file=sys.stderr)
    if hw_fail:
        print(f"warning: {hw_fail}/{args.samples} real-hardware samples failed", file=sys.stderr)

    qemu_total = sum(qemu_counts.values())
    hw_total = sum(hw_counts.values())
    if qemu_total == 0 or hw_total == 0:
        sys.exit("No usable samples from one or both targets -- nothing to compare.")

    print(f"\nQEMU: {qemu_total} samples, {len(qemu_counts)} distinct PCs")
    print(f"Real hardware: {hw_total} samples, {len(hw_counts)} distinct PCs\n")

    all_pcs = set(qemu_counts) | set(hw_counts)
    rows = []
    for pc in all_pcs:
        q_pct = 100.0 * qemu_counts.get(pc, 0) / qemu_total
        h_pct = 100.0 * hw_counts.get(pc, 0) / hw_total
        rows.append((pc, q_pct, h_pct, q_pct - h_pct))

    print(f"--- Top {args.top} addresses where QEMU spends disproportionately "
          f"MORE time than real hardware ---")
    print(f"{'PC':>12}  {'QEMU %':>8}  {'HW %':>8}  {'delta':>8}")
    for pc, q_pct, h_pct, delta in sorted(rows, key=lambda r: -r[3])[:args.top]:
        flag = "  <-- investigate" if delta > 2.0 and h_pct < q_pct / 3 else ""
        print(f"  0x{pc:08x}  {q_pct:7.2f}%  {h_pct:7.2f}%  {delta:+7.2f}{flag}")

    print(f"\n--- Top {args.top} addresses overall on real hardware (for context) ---")
    print(f"{'PC':>12}  {'HW %':>8}  {'QEMU %':>8}")
    for pc, n in hw_counts.most_common(args.top):
        h_pct = 100.0 * n / hw_total
        q_pct = 100.0 * qemu_counts.get(pc, 0) / qemu_total
        print(f"  0x{pc:08x}  {h_pct:7.2f}%  {q_pct:7.2f}%")

    print("\nRows flagged '<-- investigate' spend meaningfully more relative "
          "time on QEMU than on real hardware for the *same instruction* -- "
          "disassemble around that PC (scripts/hotloop_sample.py's "
          "disassemble_around()/literal-pool resolution logic can be reused "
          "for this) and check what status/ready flag it's waiting on, and "
          "whether this project's device model for that peripheral sets it "
          "as promptly as real hardware does.")


if __name__ == "__main__":
    main()
