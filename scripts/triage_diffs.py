#!/usr/bin/env python3
"""
triage_diffs.py -- for every word-diff between two snapshot_registers.py
dumps, look up the SVD's own documented resetValue for that peripheral+
offset (if any) and classify:

  - "hw-matches-svd"   : HW value matches SVD reset value exactly -> our
                          model's default is wrong, real actionable bug.
  - "neither-matches"  : neither QEMU nor HW matches the SVD reset value
                          (or SVD has no entry there) -> needs manual
                          judgment call (aliasing, trim, cause-register,
                          not-yet-clocked, etc), NOT auto-fixable.
  - "qemu-matches-svd" : QEMU matches SVD (expected -- our model is
                          usually the SVD default) but HW differs -> also
                          needs manual judgment (context-dependent HW
                          state), listed separately for visibility.

Also flags the "sentinel" signature: a peripheral where every single
diffed word in the HW column has the *same* repeated value -- almost
certainly an unclocked-peripheral debug-read placeholder, not real
register content.

Usage: ./scripts/triage_diffs.py snapshot-qemu.json snapshot-hw.json
"""
import json
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

SVD_PATH = Path(__file__).resolve().parent.parent / "STM32H7B0.svd"


def load_svd_reset_map():
    """peripheral name -> {offset: reset_value}"""
    tree = ET.parse(SVD_PATH)
    result = {}
    for p in tree.getroot().find("peripherals").findall("peripheral"):
        name = p.findtext("name")
        regs = p.find("registers")
        entry = {}
        if regs is not None:
            for r in regs.findall("register"):
                off_text = r.findtext("addressOffset")
                rv_text = r.findtext("resetValue")
                if off_text is None or rv_text is None:
                    continue
                entry[int(off_text, 0)] = int(rv_text, 0)
        if entry:
            result[name] = entry
    return result


def main():
    a_path, b_path = sys.argv[1], sys.argv[2]
    a = json.loads(open(a_path).read())
    b = json.loads(open(b_path).read())
    svd = load_svd_reset_map()

    for name in sorted(set(a) | set(b)):
        ea, eb = a.get(name), b.get(name)
        if ea is None or eb is None or "error" in ea or "error" in eb:
            continue
        hexa, hexb = bytes.fromhex(ea["hex"]), bytes.fromhex(eb["hex"])
        n = min(len(hexa), len(hexb))
        diffs = []
        off = 0
        while off < n:
            wa = int.from_bytes(hexa[off:off + 4], "little")
            wb = int.from_bytes(hexb[off:off + 4], "little")
            if wa != wb:
                diffs.append((off, wa, wb))
            off += 4
        if not diffs:
            continue

        hw_vals = {wb for _, _, wb in diffs}
        sentinel = len(diffs) >= 4 and len(hw_vals) == 1

        reset_map = svd.get(name, {})
        print(f"\n=== {name} ({len(diffs)} diffs){' [SUSPECTED SENTINEL: all HW words == ' + hex(next(iter(hw_vals))) + ']' if sentinel else ''} ===")
        for off, wa, wb in diffs:
            svd_rv = reset_map.get(off)
            if svd_rv is None:
                tag = "no-svd-register-at-this-offset"
            elif svd_rv == wb:
                tag = "HW-MATCHES-SVD (likely real bug in our model)"
            elif svd_rv == wa:
                tag = "qemu-matches-svd (HW differs -- context-dependent, judgment call)"
            else:
                tag = f"neither-matches (svd={svd_rv:#010x})"
            print(f"  +{off:#05x}: qemu={wa:#010x} hw={wb:#010x}  [{tag}]")


if __name__ == "__main__":
    main()
