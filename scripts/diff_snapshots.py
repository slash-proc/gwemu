#!/usr/bin/env python3
"""
diff_snapshots.py -- compare two snapshot_registers.py JSON dumps
(typically snapshot-qemu.json vs snapshot-hw.json) and report per-word
mismatches, grouped by peripheral.

Usage: ./scripts/diff_snapshots.py snapshot-qemu.json snapshot-hw.json
"""
import json
import sys


def main():
    a_path, b_path = sys.argv[1], sys.argv[2]
    a = json.loads(open(a_path).read())
    b = json.loads(open(b_path).read())

    names = sorted(set(a) | set(b))
    total_diffs = 0
    for name in names:
        ea, eb = a.get(name), b.get(name)
        if ea is None or eb is None:
            print(f"{name}: missing from {'A' if ea is None else 'B'}")
            continue
        if "error" in ea or "error" in eb:
            continue
        hexa, hexb = ea["hex"], eb["hex"]
        if hexa == hexb:
            continue
        base = ea["base"]
        byte_a = bytes.fromhex(hexa)
        byte_b = bytes.fromhex(hexb)
        n = min(len(byte_a), len(byte_b))
        diffs = []
        off = 0
        while off < n:
            wa = int.from_bytes(byte_a[off:off + 4], "little")
            wb = int.from_bytes(byte_b[off:off + 4], "little")
            if wa != wb:
                diffs.append((off, wa, wb))
            off += 4
        if diffs:
            print(f"\n{name} @ {base:#010x} ({len(diffs)} word diffs, A={a_path} B={b_path}):")
            for off, wa, wb in diffs:
                print(f"  +{off:#05x}: A={wa:#010x}  B={wb:#010x}")
            total_diffs += len(diffs)

    print(f"\nTotal word diffs: {total_diffs}")


if __name__ == "__main__":
    main()
