#!/usr/bin/env python3
"""
audit_peripheral_regs.py -- mechanically classify every register of a
gnw_h7b0_<periph> device model as "explicit" (has real switch/if-case
behavior in the read()/write() functions) vs "shadow" (falls through to
the default plain-array read/write).

Usage:
    ./scripts/audit_peripheral_regs.py <periph>
    ./scripts/audit_peripheral_regs.py hw/misc/gnw_h7b0_rcc.c

<periph> matches the gnw_h7b0_<periph> naming convention already used
across hw/misc/ and hw/display/ (e.g. "spi", "rcc", "tim2", "ltdc").

This is ONLY the mechanical half of a register audit. See the
disclaimer baked into the generated markdown for what it does NOT do.

Output:
    - printed to stdout
    - written to docs/peripheral-register-audit/<periph>.md
"""
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DEVICE_DIRS = ["hw/misc", "hw/display", "hw/arm", "hw/sd"]
INCLUDE_DIRS = ["include/hw/misc", "include/hw/display", "include/hw/arm", "include/hw/sd"]

DISCLAIMER = """\
> **What this table means (read before acting on it):**
>
> This table was generated mechanically by `scripts/audit_peripheral_regs.py`,
> by parsing this peripheral's register-offset header and the `case`/`if`
> labels in its `gnw_h7b0_*_read()`/`gnw_h7b0_*_write()` functions in the
> device model `.c` file.
>
> - **"explicit"** means this device model has *some* special-cased
>   behavior for that register on that access direction. It does **not**
>   mean the behavior is correct or complete -- read the referenced line(s)
>   yourself.
> - **"shadow"** means the register falls through to the default plain
>   array read/write (or, for read, is never referenced elsewhere and thus
>   just returns the backing array value). It does **NOT** mean the
>   register is safe to ignore. Real firmware may busy-wait on a status
>   bit in a "shadow" register that this model never sets, exactly as
>   found this session in LTDC and SPI/OSPI (confirmed real bugs a purely
>   mechanical pass like this one cannot catch).
>
> This script cannot tell you whether firmware depends on a shadow
> register's real hardware effect. That still requires a manual or
> research-agent pass cross-referencing real firmware source (e.g. a
> sibling checkout at `~/Nerd/git/game-and-watch-retro-go-sd`) against
> this list, following the same method used for this session's LTDC and
> SPI/OSPI audits. Treat every "shadow" row here as an open question, not
> a clean bill of health.
"""


def find_device_file(periph):
    """periph can be a bare name (e.g. 'spi') or a path to a .c file."""
    if periph.endswith(".c") and os.path.exists(periph):
        return os.path.abspath(periph)
    candidate_name = f"gnw_h7b0_{periph}.c"
    for d in DEVICE_DIRS:
        p = os.path.join(REPO_ROOT, d, candidate_name)
        if os.path.exists(p):
            return p
    # fall back: maybe they passed the full gnw_h7b0_<x> already
    candidate_name2 = f"{periph}.c" if periph.startswith("gnw_h7b0_") else None
    if candidate_name2:
        for d in DEVICE_DIRS:
            p = os.path.join(REPO_ROOT, d, candidate_name2)
            if os.path.exists(p):
                return p
    return None


def find_regs_header(periph_base, c_file):
    """
    periph_base: bare name like 'spi' or 'rcc' (derived from the .c filename)
    A peripheral may have BOTH a gnw_h7b0_regs_<periph>.h (auto-generated,
    possibly using a differently-prefixed register namespace, e.g. SPI1_ vs
    the .c file's own SPI_) and a gnw_h7b0_<periph>.h with its own inline
    offset defines that the .c file actually uses (seen on spi/ospi). Pick
    whichever candidate's register names actually appear in the .c source,
    not just whichever file exists first.
    Returns (path, layout) where layout in {"regs_generated", "inline"}.
    """
    with open(c_file) as f:
        c_src = f.read()

    candidates = []  # (path, layout)
    for d in INCLUDE_DIRS:
        p = os.path.join(REPO_ROOT, d, f"gnw_h7b0_regs_{periph_base}.h")
        if os.path.exists(p):
            candidates.append((p, "regs_generated"))
    for d in INCLUDE_DIRS:
        p = os.path.join(REPO_ROOT, d, f"gnw_h7b0_{periph_base}.h")
        if os.path.exists(p):
            candidates.append((p, "inline"))

    if not candidates:
        return None, None
    if len(candidates) == 1:
        return candidates[0]

    best = None
    best_score = -1
    for path, layout in candidates:
        regs = parse_registers(path, layout)
        score = sum(1 for name, _ in regs if name in c_src)
        if score > best_score:
            best_score = score
            best = (path, layout)
    return best


OFFSET_RE_GENERATED = re.compile(
    r"#define\s+(GNW_H7B0_\w+?)_OFFSET\s+(0x[0-9a-fA-F]+|\d+)"
)
# Inline layout (e.g. RCC): bare defines like
#   #define GNW_H7B0_RCC_CR     0x00
# Exclude anything ending in _SIZE, _HZ, or other non-offset constants by
# requiring the value look like a small hex/dec offset and the name not to
# be a known non-register suffix.
NON_REG_SUFFIXES = ("_SIZE", "_HZ", "_COUNT", "_MASK", "_SHIFT", "_OFFSET",
                    "_RESET", "_WMASK", "_ON", "_RDY")
OFFSET_RE_INLINE = re.compile(
    r"#define\s+(GNW_H7B0_[A-Z0-9_]+)\s+(0x[0-9a-fA-F]+|\d+)\s*(?:/\*.*\*/)?\s*$"
)


def parse_registers(header_path, layout):
    """Return list of (regname, offset_int) in file order, deduplicated."""
    regs = []
    seen = set()
    with open(header_path) as f:
        for line in f:
            line = line.rstrip("\n")
            if layout == "regs_generated":
                m = OFFSET_RE_GENERATED.search(line)
                if m:
                    name, off = m.group(1), int(m.group(2), 0)
                    if name not in seen:
                        seen.add(name)
                        regs.append((name, off))
            else:  # inline
                m = OFFSET_RE_INLINE.search(line)
                if not m:
                    continue
                name, offstr = m.group(1), m.group(2)
                if any(name.endswith(suf) for suf in NON_REG_SUFFIXES):
                    continue
                try:
                    off = int(offstr, 0)
                except ValueError:
                    continue
                # heuristic: register offsets are small-ish and word-aligned;
                # skip obviously-not-an-offset huge constants (e.g. Hz values)
                if off > 0x10000:
                    continue
                if name not in seen:
                    seen.add(name)
                    regs.append((name, off))
    return regs


def extract_function_body(src_lines, func_name):
    """Find a function definition by name and return (start_line, end_line, body_lines),
    1-indexed inclusive, using brace counting from the opening '{' after the
    function signature."""
    start_idx = None
    for i, line in enumerate(src_lines):
        if re.search(r"\b" + re.escape(func_name) + r"\s*\(", line) and "{" not in line.split(func_name)[0]:
            # crude filter: looks like a definition line (not just a call)
            if re.match(r"^(static\s+)?\S.*\b" + re.escape(func_name) + r"\s*\(", line):
                start_idx = i
                break
    if start_idx is None:
        return None
    # find opening brace (may be on same line or a following line)
    depth = 0
    brace_seen = False
    end_idx = None
    for i in range(start_idx, len(src_lines)):
        for ch in src_lines[i]:
            if ch == "{":
                depth += 1
                brace_seen = True
            elif ch == "}":
                depth -= 1
        if brace_seen and depth == 0:
            end_idx = i
            break
    if end_idx is None:
        return None
    return start_idx + 1, end_idx + 1, src_lines[start_idx:end_idx + 1]


# Matches: case GNW_H7B0_FOO_BAR:  or  case GNW_H7B0_FOO_BAR_OFFSET:
CASE_RE = re.compile(r"case\s+(GNW_H7B0_[A-Z0-9_]+?)(_OFFSET)?\s*:")
# Matches TIM2-style: if ((addr & 0x3ffu) == GNW_H7B0_TIM2_EGR_OFFSET ...
IF_ADDR_RE = re.compile(r"==\s*(GNW_H7B0_[A-Z0-9_]+?)(_OFFSET)?\b")


def find_explicit_regs(body_lines, body_start_line, reg_names_by_base):
    """
    Scan a function body for case labels / if-comparisons referencing
    register macros. Returns dict: reg_base_name -> (line_no, snippet)
    reg_base_name is the register name WITHOUT any _OFFSET suffix, matching
    however parse_registers named it (with or without _OFFSET stripped
    already handled by caller via reg_names_by_base lookup).
    """
    found = {}
    for i, line in enumerate(body_lines):
        lineno = body_start_line + i
        for regex in (CASE_RE, IF_ADDR_RE):
            for m in regex.finditer(line):
                base = m.group(1)
                if base in reg_names_by_base and base not in found:
                    snippet = line.strip()
                    found[base] = (lineno, snippet)
    return found


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(1)

    arg = sys.argv[1]
    c_file = find_device_file(arg)
    if not c_file:
        print(f"error: could not locate device .c file for '{arg}' "
              f"(looked for gnw_h7b0_{arg}.c under {DEVICE_DIRS})", file=sys.stderr)
        sys.exit(1)

    basename = os.path.basename(c_file)
    m = re.match(r"gnw_h7b0_(.+)\.c$", basename)
    periph_base = m.group(1) if m else arg

    header_path, layout = find_regs_header(periph_base, c_file)
    if not header_path:
        print(f"error: could not locate a register header for '{periph_base}' "
              f"(looked for gnw_h7b0_regs_{periph_base}.h and gnw_h7b0_{periph_base}.h "
              f"under {INCLUDE_DIRS})", file=sys.stderr)
        sys.exit(1)

    regs = parse_registers(header_path, layout)
    if not regs:
        print(f"error: found header {header_path} but parsed zero registers "
              f"from it (layout guessed as '{layout}') -- check the header's "
              f"#define format by eye", file=sys.stderr)
        sys.exit(1)

    reg_names = {name for name, _ in regs}

    with open(c_file) as f:
        src_lines = f.readlines()

    read_func = f"gnw_h7b0_{periph_base}_read"
    write_func = f"gnw_h7b0_{periph_base}_write"

    read_info = extract_function_body(src_lines, read_func)
    write_info = extract_function_body(src_lines, write_func)

    read_explicit = {}
    write_explicit = {}

    if read_info:
        rstart, _, rbody = read_info
        read_explicit = find_explicit_regs(rbody, rstart, reg_names)
    if write_info:
        wstart, _, wbody = write_info
        write_explicit = find_explicit_regs(wbody, wstart, reg_names)

    rel_c = os.path.relpath(c_file, REPO_ROOT)
    rel_h = os.path.relpath(header_path, REPO_ROOT)

    lines = []
    lines.append(f"# Register audit: {periph_base}\n")
    lines.append(DISCLAIMER)
    lines.append("")
    lines.append(f"- Device model: `{rel_c}`")
    lines.append(f"- Register header: `{rel_h}` (layout: {layout})")
    if not read_info:
        lines.append(f"- NOTE: could not locate `{read_func}()` in the .c file "
                      f"-- read-side classification below may be incomplete/absent.")
    if not write_info:
        lines.append(f"- NOTE: could not locate `{write_func}()` in the .c file "
                      f"-- write-side classification below may be incomplete/absent.")
    lines.append("")
    lines.append("| Register | Offset | Write | Read | Notes (line ref / snippet) |")
    lines.append("|---|---|---|---|---|")

    for name, off in regs:
        w_hit = write_explicit.get(name)
        r_hit = read_explicit.get(name)
        w_status = "explicit" if w_hit else "shadow"
        r_status = "explicit" if r_hit else "shadow"
        notes = []
        if w_hit:
            notes.append(f"write @ {rel_c}:{w_hit[0]} `{w_hit[1]}`")
        if r_hit:
            notes.append(f"read @ {rel_c}:{r_hit[0]} `{r_hit[1]}`")
        note_str = "<br>".join(notes) if notes else ""
        lines.append(f"| {name} | 0x{off:x} | {w_status} | {r_status} | {note_str} |")

    n_total = len(regs)
    n_w_explicit = sum(1 for name, _ in regs if name in write_explicit)
    n_r_explicit = sum(1 for name, _ in regs if name in read_explicit)
    lines.append("")
    lines.append(f"Summary: {n_total} registers found. "
                 f"Write: {n_w_explicit} explicit / {n_total - n_w_explicit} shadow. "
                 f"Read: {n_r_explicit} explicit / {n_total - n_r_explicit} shadow.")

    output = "\n".join(lines) + "\n"

    out_dir = os.path.join(REPO_ROOT, "docs", "peripheral-register-audit")
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, f"{periph_base}.md")
    with open(out_path, "w") as f:
        f.write(output)

    print(output)
    print(f"(written to {os.path.relpath(out_path, REPO_ROOT)})", file=sys.stderr)


if __name__ == "__main__":
    main()
