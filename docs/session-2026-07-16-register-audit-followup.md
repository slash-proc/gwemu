# 2026-07-16 register-audit follow-up: GPIO BSRR fix write-up + CRC_INIT false-alarm

Two items from the same session, tracked together since both surfaced
around the same `../stm32h7b0-diag` register-audit pass
(`../stm32h7b0-diag/register_audit.json`/`register_audit.txt`, produced by
`../stm32h7b0-diag/host/register_audit.py`, the newer sibling of this
repo's own `scripts/snapshot_registers.py` — same three-way
hardware-vs-QEMU-vs-SVD diffing idea, see
`docs/session-2026-07-12-register-snapshot-diffing.md`).

## 1. GPIO BSRR→ODR/IDR wiring gap (fixed, commit `2485f62785`)

Real bug, already fixed and committed earlier this session; recorded here
since it didn't have a dedicated writeup beyond the commit message.

**Symptom**: `../stm32h7b0-diag`'s `gpio_output_readback` case writes a
pin's own current ODR-read level back through `BSRR` (electrically a
no-op) and expects both `ODR` to still reflect it and `IDR` to loop it
back (proving the push-pull output stage works end-to-end). Failed under
QEMU; passes on real hardware.

**Root cause**: two independent gaps in `hw/misc/gnw_h7b0_gpio.c`:
1. `BSRR` writes were correctly zeroed back to 0 (real hardware: BSRR is
   write-only) but the set/reset semantics were never translated into
   `ODR` at all — the write was simply discarded.
2. `IDR` was never derived from `ODR` for output-configured pins — it was
   purely driven by simulated button-press events, so even a correct
   `ODR` write would never be observable on `IDR` readback.

**Fix**: a real `BSRR`→`ODR` translation (bits `[15:0]` set, `[31:16]`
reset, set wins ties, matching RM0455's documented BSRR semantics) plus a
new `gnw_h7b0_gpio_sync_output_idr()` helper that keeps `IDR` in sync with
`ODR` for every pin currently configured as general-purpose output
(`MODER == 01`) on every `MODER`/`ODR`/`BSRR` write. Input-configured pins
are untouched, still driven by the existing button-event path.

Verified live against the diag suite: `gpio_output_readback` passes,
`button_gpio_liveness` (the input path) still passes, no regressions
elsewhere. (`gpio_output_readback` has since intermittently reappeared as
FAIL in unrelated full-suite runs — tracked in
`docs/session-2026-07-16-diag-failure-triage.md` priority 4g as
host-contention flakiness, not a regression of this fix; not touched
further this session.)

**Files changed**: `hw/misc/gnw_h7b0_gpio.c`.

## 2. CRC_INIT reset value: audit flagged a "regression" that isn't one

### The apparent conflict

Earlier this session, commit `c2e265fe82` changed
`GNW_H7B0_CRC_INIT_RESET` in `include/hw/misc/gnw_h7b0_regs_crc.h` from
`0x00000000` to `0xFFFFFFFF`, citing RM0455 22.4.4's stated CRC_INIT reset
value, to fix the diag suite's `crypto_crc32` case (which relies on the
CRC unit's power-on-reset default INIT value without reprogramming it).

A subsequent register-audit run
(`../stm32h7b0-diag/register_audit.json`) flagged this as a new QEMU
regression:

```
CRC INIT +0x010 (0x40023010) qemu=0xffffffff hw=0x00000000 svd=0x00000000
classification: HW-MATCHES-SVD ("HW==SVD, QEMU differs -> likely real QEMU model bug")
```

i.e. real hardware reportedly reads `CRC_INIT` back as `0x00000000` at
reset, matching `STM32H7B0.svd`'s `<resetValue>0x00000000</resetValue>`
for that register, with QEMU now the outlier.

### Why this specific finding isn't trustworthy

Looking at the *other* two CRC registers in the same audit run, both
flagged `qemu-matches-svd` (not treated as a QEMU bug by the classifier,
but the same underlying "HW differs" signal is present):

```
CRC DR  +0x000  qemu=0xffffffff hw=0x00000000 svd=0xffffffff
CRC POL +0x014  qemu=0x04c11db7 hw=0x00000000 svd=0x04c11db7
```

**Every register read from the real CRC peripheral in this audit came
back `0x00000000`** — `DR`, `INIT`, and `POL` alike — including `DR`,
whose `0xFFFFFFFF` power-on default is one of the most well-established,
universally-documented facts about the STM32 CRC IP across the whole
family (and matches the SVD too). A real chip reading `DR=0` at reset
would be a far bigger, more surprising finding than an `INIT` reset-value
disagreement, and isn't independently corroborated anywhere.

The register-audit tool's own source
(`../stm32h7b0-diag/host/register_audit.py`) documents exactly this
failure mode in its own comments: peripherals whose register block reads
back as one uniform, suspicious value across the board are flagged as
"sentinel" — "almost certainly an unclocked-peripheral debug-read
placeholder, not real register content" — and are excluded from the
prominent regression list. The tool's own summary output lists five such
sentinel peripherals this run (`DMA1, FMC, GPIOA, LTDC, SAI1`), all
showing this identical pattern. **CRC exhibits the exact same pattern
(all three of its non-zero-by-spec registers reading back 0) but wasn't
auto-flagged**, purely because the sentinel filter requires
`SENTINEL_MIN_DIFFS = 4` diffed registers to trigger, and CRC — a small,
5-register peripheral where `IDR`/`CR` already have an SVD-default of `0`
and so don't count as "diffed" — only ever has 3 registers eligible to
diff (`DR`, `INIT`, `POL`). It structurally cannot reach the threshold
regardless of whether it's exhibiting the same unclocked-read artifact.

The audit's own read methodology confirms the mechanism: it attaches and
calls `reset_and_halt()` immediately, then snapshots raw registers
**without enabling any peripheral clocks first** (`register_audit.py`'s
`snapshot()`/`snapshot_target()`). `CRC`'s AHB4ENR clock-enable bit is off
at reset by default — real hardware silently returns 0 for register reads
to an unclocked peripheral over the debug port on this chip (same
mechanism already causing the five confirmed sentinel peripherals above).
So the audit's `hw=0x00000000` reading for `CRC_INIT` (and `DR`/`POL`)
reflects **"the CRC block wasn't clocked when read," not "the true
post-reset value of CRC_INIT is zero."** It tells us nothing new; SVD
already said `0x00000000` before this audit ran.

### Resolution

**No code change.** `GNW_H7B0_CRC_INIT_RESET` stays `0xFFFFFFFF`
(commit `c2e265fe82`'s value), matching RM0455 22.4.4 and the diag
suite's expectations. The audit's apparent "regression" is a false
positive caused by a gap in the audit tool's sentinel-detection threshold
for small peripherals, not a real QEMU divergence — the RM0455-vs-SVD
conflict on `CRC_INIT`'s true reset value (unlike the flash-size case in
`docs/h7b0-flash-discrepancy.md`) remains genuinely **unresolved by any
hardware measurement so far**, because no reliable clocked read of this
register has actually been taken.

Verified live post-analysis (no source changes made, so this just
reconfirms the already-fixed state): rebuilt `qemu-system-arm` clean,
booted `../stm32h7b0-diag`'s `fw/build/diag.bin` with gdbstub halted, ran
the full harness — `crypto_crc32` and `crypto_crc16_reconfig` both `OK`.
(Three unrelated cases — `boot_option_bytes`, `power_sleep_wfi`,
`ospi_dlyb_readback` — showed `FAIL` in this same run; all three are
already tracked as known host-contention flakes in
`docs/session-2026-07-16-diag-failure-triage.md` priority 4g, not new
regressions, and weren't touched.)

### Recommendation for next real-hardware audit pass

To get a real answer on `CRC_INIT`'s true reset value (and clean up the
five already-known sentinel peripherals plus any other small
peripheral hitting the same `SENTINEL_MIN_DIFFS` blind spot), a future
audit run should explicitly enable each target peripheral's RCC clock
gate before reading its registers, or else lower/remove the fixed
`SENTINEL_MIN_DIFFS = 4` threshold in
`../stm32h7b0-diag/host/register_audit.py` in favor of a per-peripheral
threshold scaled to its register count (e.g. "≥60% of a peripheral's
diffed registers share one suspicious value"), so small peripherals like
`CRC` aren't structurally exempt from sentinel detection. Both are
changes to `../stm32h7b0-diag`, not this repo, and weren't made this
session per this repo's usual "don't grow capability into sibling tooling
without being asked" default (see CLAUDE.md's `gnwmanager` note for the
same policy applied to a different sibling project) — flagged here for
whoever runs the next audit pass.
