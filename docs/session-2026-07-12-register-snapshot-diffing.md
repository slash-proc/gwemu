# 2026-07-12 (later session) — reset-register snapshot/diff tooling

## Goal

A new capability became available this session: `gnwmanager --qemu <cmd>`
(branch `support-qemu-target` in the sibling `gnwmanager` repo) lets
gnwmanager's own backend abstraction talk to QEMU's gdbstub exactly the
way it already talks to real hardware over openocd/pyocd. That means the
same script can snapshot every peripheral register on both QEMU and a
real physical Game & Watch and diff them, instead of the previous
one-bug-at-a-time PC-chase workflow (see the morning session's stock-boot
doc). Goal: use that to find a batch of real reset-default bugs in one
pass.

## Tooling built (all in `scripts/`, all new this session)

- **`snapshot_registers.py`**: parses every `<peripheral>` in
  `STM32H7B0.svd` (121 unique base addresses) and reads each one's full
  register block via `gnwmanager`'s `OCDBackend` interface
  (`read_memory()`), against either target (`--qemu` or real hardware).
  Three halt strategies, selectable by flag (see "Halt strategy" below).
- **`diff_snapshots.py`** / **`triage_diffs.py`**: diff two snapshot JSON
  files word-by-word; `triage_diffs.py` additionally cross-references
  each diffed word against the SVD's own documented `resetValue` and
  auto-classifies it (`HW-MATCHES-SVD` = likely a real bug in our model;
  `qemu-matches-svd` = HW differs, needs manual judgment; `neither-matches`
  = needs manual judgment; flags a "SUSPECTED SENTINEL" peripheral when
  every diffed word has the identical value, the signature of an
  unclocked-peripheral debug-read placeholder rather than real content).
- **`halt_at_entry.py`**: see "Halt strategy" below.
- **`make_boot_images.py`** / **`boot_qemu.sh`**: standardize QEMU launch
  inputs (see "Boot image standardization" below).

## Halt strategy: three approaches, two dead ends before the one that worked

**1. Plain `reset_and_halt()`** (`GDBBackend`'s two-step reset-then-Ctrl-C
for QEMU, OpenOCD's `reset halt` for real hardware) — **too racy to use**.
Neither is a true vector-catch: on real hardware, a real breakpoint-free
async halt request loses a race against however many instructions the CPU
executes before the halt takes effect, given USB/JTAG round-trip latency.
Confirmed via a real find: `RCC`'s peripheral-enable register block
(`AHB3ENR`/`AHB1ENR`/.../`APB4LPENR`, offsets `0xd0`-`0x17c`) showed live,
non-zero clock-enable values instead of the documented all-zero POR
default — clear evidence code had already run. A diff taken this way is
mostly unusable for finding real bugs; don't use it as ground truth for
"the reset default is wrong."

**2. `--gnwmanager-active`**: snapshot after `GnW.start_gnwmanager()`
(gnwmanager's own known RAM payload) is loaded, vectored into by
directly setting MSP/PC, resumed, and has signaled idle — on *both*
targets. This sidesteps the "how much of stock firmware executed before
halt" ambiguity, since both targets ran the exact same deterministic code
to the exact same completion point. **This is what actually found real
bugs** (see "Real bugs found" below) — worth reusing as the default
mode for future register-audit passes.

**3. `--halt-at-entry`**: the user's suggestion — set a real hardware
breakpoint at the target's own reset-vector entry point (read from
`zelda-bank1.bin` offset 4, Thumb bit stripped) *before* triggering
reset, so the halt is deterministic instead of raced. Implemented in
`scripts/halt_at_entry.py` entirely on top of `gnwmanager`'s existing
public backend primitives (`GDBBackend._send_command()`/socket for raw
gdb-remote `Z1`/`z1` breakpoint packets; `OpenOCDBackend.__call__` for
raw `bp`/`rbp`/`wait_halt` Tcl commands) — **no changes to the
`gnwmanager` package itself**, it's a pure consumer.

Confirmed working: both targets land at the identical real entry point
(`0x0801ad48`) with QEMU showing exactly clean POR defaults across all of
RCC. Surprising result: real hardware *still* showed the same
"anomalous" RCC values (mirrored `ENR`/`LPENR` block, non-zero
`BDCR`/trim registers) even at this guaranteed first instruction — which
turned out to mean those were never race artifacts to begin with (see
next section), not a tooling failure.

## RCC findings (from the `--gnwmanager-active` and `--halt-at-entry` passes)

Three real, now-explained phenomena in RCC's diff, none of them bugs:

1. **A real hardware address-decode aliasing quirk**: the entire
   `ENR`/`LPENR` register block is mirrored on real silicon at a
   constant `-0x60` offset from its documented location (e.g.
   `AHB3LPENR`'s real SVD offset `0x15c` reads correctly there *and* is
   echoed at the undocumented reserved offset `0xfc`). Confirmed present
   even at the guaranteed pre-firmware entry-point halt, so it's not
   code-execution-dependent — likely vestigial address decoding from the
   dual-core H7 die this single-core part is derived from. Firmware
   never reads the mirror; not worth modeling.
2. **Factory calibration trim** (`HSICFGR`, `CRRCR`, `CSICFGR`) is loaded
   by hardware itself at every reset (not written by firmware) and is
   unique per physical chip — not a fixed "reset default" that's
   meaningful to hardcode from one unit's readback.
3. **Backup-domain state legitimately persists across a warm reset**
   (`BDCR`'s LSE/RTC config, one `CR` bit) by real hardware design — it's
   only cleared by a true cold power-on or an explicit `BDRST`, not by
   `NRST`/software reset. Not a bug; our synthetic QEMU boots are
   effectively always cold, so this doesn't need modeling either.
4. `RCC_RSR` is a reset-*cause* register — legitimately differs based on
   which reset mechanism triggered it (debugger `SYSRESETREQ` vs. real
   POR vs. pin reset), not a fixed default.

Net result: **RCC has zero actionable reset-default bugs.** Our model's
SVD-derived defaults are already correct; every RCC diff is fully
explained by one of the four points above.

## Real bugs found and fixed (all three confirmed building clean)

All three are the same underlying bug class, found via the
`--gnwmanager-active` pass: a write-only "pulse" register (writing it has
a side effect, but real hardware/the SVD's own `resetValue` always reads
it back as `0`) whose device model has no special-case in its write
handler, so the generic mask-and-store path leaves the last-written value
sitting in the readback array instead of forcing it back to `0`.

1. **`hw/misc/gnw_h7b0_gpio.c`** — `GPIOx_BSRR` (offset `0x18`, every
   port) now forced to `0` after any write.
2. **`hw/misc/gnw_h7b0_rtc.c`** — `RTC_WPR` (offset `0x24`) now forced to
   `0` after any write. Confirmed via code search that nothing in this
   file reads back or gates on `WPR`'s value (no unlock-key-sequence
   logic exists), so this fix is complete and safe as-is.
3. **`hw/misc/gnw_h7b0_tim1.c`** — `TIM1_EGR` (offset `0x14`) now forced
   to `0` after any write. TIM1 has no counting/`ptimer` logic modeled at
   all currently, so `EGR`'s real "generate update event" side effect
   (setting `UIF` in `SR`) is *not* implemented — explicitly out of scope
   for this fix, which only corrects the readback.

Explicitly **not** pursued (would need real interpretation/new
peripheral implementations, out of scope for a mechanical register-diff
fix): `IWDG`/`LPUART1`/`SWPMI`/`SPDIFRX`/`CRS`/`DAC1`/`DAC2`/`DMA1`/
`DMAMUX1`/`FDCAN`/`CAN_CCU` diffs are all `create_unimplemented_device`
stubs with no real model to patch; `DBGMCU` diffs reflect the debug
probe's own configuration, not firmware-relevant state; `LTDC`/`SPI1`/
`SPI2`/`PWR`/`Flash`/`CRC` diffs are genuinely ambiguous (neither
snapshot matches the SVD default, meaning both reflect mid-execution
payload state rather than a knowable "correct" value).

## `create_unimplemented_device` coverage pass

`hw/arm/gnw_h7b0_soc.c` previously had `create_unimplemented_device()`
stubs for only two peripherals (`IWDG1`, `HASH`). Extended to 69 total —
every SVD peripheral base address not covered by a real device model —
so a full-address-space register sweep (or any tool poking an
unimplemented peripheral, e.g. gnwmanager's own payload) logs instead of
raising a BusFault. Two SVD `addressBlock` entries (`OTG1_HS_PWRCLK`,
`AXI`) declare implausibly large sizes (a stray/union artifact in the
vendor SVD); `snapshot_registers.py` caps any peripheral read to `0x1000`
bytes to avoid reading outside mapped memory because of these.

## Boot image standardization

Previously, QEMU was launched ad hoc with whatever partial dumps were on
hand (`backup/internal_flash_backup_zelda.bin` at 128 KiB — half of the
real 256 KiB dual-bank size; `backup/flash_backup_zelda.bin` at 4 MiB — a
fraction of the real chip's 64 MiB, confirmed via `gnwmanager info`'s
"External Flash Size (MB): 64.0"). New scripts fix this:

- **`make_boot_images.py <game>`**: builds
  `backup/qemu-images/<game>-{bank1,bank2,extflash}.bin` from the raw
  dumps, sized from `FLASH_BANK_SIZE`/`EXTFLASH_SIZE` in
  `gnw_h7b0_soc.h` directly (can't drift out of sync with the machine
  model). `bank1` is the real dump 0xFF-padded up to the full bank size;
  `bank2` is entirely 0xFF (no real dump exists, stock firmware doesn't
  use it — blank-for-now is a deliberate project decision, not a
  placeholder to fix later); `extflash` is the real 4 MiB dump 0xFF-padded
  up to the real 64 MiB chip size.
- **`boot_qemu.sh <game>`**: the one standardized launch command going
  forward. Deliberately does **not** pass `-d guest_errors,unimp` or any
  other unbounded logging flag — that flag filled `/tmp` and
  destabilized the host machine multiple times this session (real
  incident, not hypothetical). If a specific debugging session needs
  guest-error logging, add it explicitly and bound the output; don't
  leave it on for a free-running boot.

## Workflow notes for future sessions

- Prefer `--gnwmanager-active` over plain `reset_and_halt()` for any
  future register-audit pass — it's the mode that actually found real
  bugs. Use `--halt-at-entry` specifically when you need true,
  guaranteed-pre-firmware POR state (e.g. re-validating a reset-default
  fix), not for general bug-hunting (GPIO and other AHB4/D3-domain
  peripherals aren't clocked yet at real entry, so most of the interesting
  diffs are sentinel noise at that point — see the "SUSPECTED SENTINEL"
  tag in `triage_diffs.py`'s output).
- A "SUSPECTED SENTINEL" tag (every diffed word in a peripheral has the
  identical HW value, e.g. `0xabffffff`, `0xa3c5dd01`) means the debug
  probe substituted a fixed placeholder for an inaccessible/unclocked
  read — not real register content. Don't chase these as bugs.
- Don't treat a `neither-matches` or `qemu-matches-svd` diff as an
  actionable bug without first checking whether it's explained by one of:
  reset-cause register, backup-domain persistence, factory trim, or
  debug-probe-influenced state (DBGMCU). Only `HW-MATCHES-SVD` on an
  already-modeled (non-stub) peripheral is a clean, low-risk fix
  candidate.
