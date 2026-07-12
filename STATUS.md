# Status

Last updated: 2026-07-12

## Where things stand

Repo is a fork of upstream QEMU (`qemu/qemu`), pinned to tag `v11.0.2`.
Working branch `gnw-h7b0`. Phase 0/1 (boot path, memory map) and Phase 2/3
(DMA2D, LTDC display) are done. retro-go (homebrew) firmware boots
end-to-end through the SD-backed build, with working audio, gamepad input,
JPEG-decoded cover art, and menu/gameplay rendering. Not push-worthy yet —
local commits only, push only on explicit go-ahead.

**Actively in progress:** getting stock (official Nintendo) firmware
(Mario, Zelda) booting to visible display output — not yet reached.
Three same-day 2026-07-12 sessions, in order:
1. `docs/session-2026-07-12-stock-firmware-boot-investigation.md` —
   initial trace; 2 real fixes (`RCC_RSR.SFTRSTF`, PA0/WKUP1 GPIO reset
   default); found the main-superloop `[r4+9]`/`[r4+0x60]` blocker.
2. `docs/session-2026-07-12-register-snapshot-diffing.md` — built
   `scripts/snapshot_registers.py` + friends; 3 more real reset-value
   fixes (`GPIOx_BSRR`/`RTC_WPR`/`TIM1_EGR`); standardized boot images
   (`scripts/make_boot_images.py`, `boot_qemu.sh`); confirmed RCC's
   apparent anomalies are real hardware behavior, not bugs.
3. `docs/session-2026-07-12-breakpoint-lockstep-tracing.md` — built
   `scripts/checkpoint.py` + friends (breakpoint-based, not single-step,
   live QEMU-vs-hardware comparison); confirmed QEMU tracks real
   hardware bit-for-bit identically from entry through the superloop's
   first 20 passes. Real-hardware SWD kept dropping into standby
   mid-session (stock Zelda's own state-6 standby handler, triggered by
   repeated resets); fixed via `scripts/make_zelda_patched_bank1.py`
   (replicates gnwmanager's 2-instruction standby-skip patch) + flashing
   that image to the device. **Follow-up same-day part 3**: re-ran
   `watch_loop_flag.py` against the now-stable patched hardware for 400
   passes (up from 20) — zero divergence from QEMU, and real hardware
   audibly/visibly proceeds to intro boot artwork within moments of the
   breakpoint being released. **This overturns the 3-session-old
   `[r4+9]`/`[r4+0x60]` "blocker" hypothesis**: that loop is a normal
   fast-cycling idle poll (`FUN_0800ea00` state-0 dispatch, a trivial
   `bx lr`) both targets sit in identically, not a stall point. The real
   divergence is downstream — see doc's "Follow-up session (same day,
   part 3)" section for the revised next-step (find what writes the
   state byte at `0x0800f438`, likely from an IRQ handler). QEMU has no
   standby/low-power mode modeled — future work. Read the doc's "Real,
   load-bearing bugs found and fixed in this session's own tooling"
   section before writing any new gdb-remote/OpenOCD scripting.

## What works

- Core CPU, memory map, NVIC, real clock tree (HSI/HSE/PLL1-3, live-derived
  SYSCLK/HCLK/LTDC-pixel-clock — see `docs/h7b0-clock-tree-findings.md`),
  dynamic CPU-overclock support (retro-go's NORMAL/BOOST1/BOOST2 modes).
- SPI/OSPI flash, SD card (SPI-based), RTC, DMA (accumulator-scheduled, no
  drift), TIM2, ADC, PWR, CRC, and the rest of the boot-path peripheral set.
- LTDC: real per-layer compositing (color key, window-clip/default-color,
  generalized constant-alpha/pixel-alpha blend formula, Bayer dithering,
  independent Layer1/Layer2 CLUTs), correct RRIF/reload-timing semantics,
  vblank capture now gated on firmware's actual `SRCR.VBR` write instead of
  a fixed timer (see Known issues — this was a real fix, not yet fully
  verified against every code path).
- DMA2D: real per-pixel fetch for every input format retro-go uses
  (ARGB8888/RGB565/ARGB1555/ARGB4444/L8/YCbCr/A8), real `BLEND_BG`/`BLEND_FG`
  "fixed color" semantics (confirmed against the real HAL header, not
  guessed), 3-bit `MODE` field (was silently truncating to 2 bits).
- JPEG: real polled `DOR` output register (firmware's own polling loop
  drains it into guest RAM, mirroring real polling-mode decode with zero
  DMA involved) replacing an earlier raw-pointer "hack buffer"; chroma
  subsampled per the image's real hand-parsed SOF0 factors (matches real
  hardware's output size, previously ~2x too large and got only partially
  drained by firmware — a real corruption bug, fixed); output FIFO
  threshold (`OFTF`) now modeled so firmware's polling loop takes the real
  8-words-per-check bulk path instead of one-word-at-a-time (~30%
  reduction in the dominant perf cost during coverflow/menu use, confirmed
  via `perf`).
- Audio: confirmed solid across every core tested (NES, Genesis, SMW,
  others) after the clock-tree work above.

## Known issues (open)

- **Coverflow/menu flicker**: selected item's text and the leftmost
  background cover flicker, worse with a menu open over the carousel.
  Extensively investigated this session — full narrative in
  `docs/session-2026-07-11-ltdc-flicker-investigation.md` and
  `-dma2d-jpeg-ycbcr-pipeline.md`. Ruled out: LTDC `RRIF` timing (fixed,
  didn't help alone), LTDC compositing fidelity gaps (fixed, didn't help),
  DMA2D/JPEG pixel corruption (two real bugs fixed, confirmed correct
  now — decode/compositing/blit are all deterministic). A separate,
  promising fix landed the same day (`hw/display/gnw_h7b0_ltdc.c`: gate
  framebuffer capture on firmware's `SRCR.VBR` write instead of every
  fixed vblank tick, avoiding capturing a still-mid-draw frame when the
  emulated CPU's render time varies) — **not yet confirmed live** whether
  this actually resolves the flicker; needs retest. Known gap in that fix:
  content that never writes `SRCR.VBR` (single-buffered/IMR-only paths)
  no longer gets captured at all — hasn't caused an observed regression
  yet, but worth watching.
- Real subsampled chroma storage was traded for full-resolution internal
  storage in the JPEG model (a documented scope decision, not a bug) —
  fine for correctness, just not bit-exact to real silicon's MCU-block
  output layout.
- Real JPEG-decode performance is inherently register-access-heavy (matches
  real hardware's actual polling protocol) and QEMU's per-MMIO-access cost
  is much higher than real silicon's; the `OFTF` fix helped but coverflow
  screens (5 covers redecoded every frame, matching real firmware behavior)
  are still the most expensive on-screen content by a wide margin.

## Next objective

**Get the stock (official Nintendo) Game & Watch firmware booting and
running**, as opposed to the homebrew retro-go build tested so far.
Process now: boot both QEMU and real hardware to the real reset entry
point (`scripts/halt_at_entry.py`), walk forward checkpoint-by-checkpoint
via real breakpoints (`scripts/checkpoint.py`/`step_init_calls.py`,
never single-stepping — too slow over real hardware's SWD link) cross-
referenced against Ghidra decompilation (`gnw-mario-decomp`/
`gnw-zelda-decomp`), compare core registers at each, and narrow toward
the first real divergence. Live gdb pokes and binary patches remain
diagnostic aids only, never the fix — implement fixes in the relevant
device model, rebuild, re-check.

Seven real fixes landed so far (`RCC_RSR.SFTRSTF`, PA0/WKUP1 GPIO reset
default, `GPIOx_BSRR`/`RTC_WPR`/`TIM1_EGR` readback, GPIOC bit 8/13 and
GPIOD bit 0 no longer forced low at reset — confirmed via direct
real-hardware reads that all read `0xFFFFFFFF` — and, same-day part 5,
`GNW_H7B0_ADC_FULL_BATTERY_RAW` bumped `13500` → `0xFFFF`). The
main-superloop `[r4+9]`/`[r4+0x60]` checkpoint, previously suspected as
the boot blocker, was ruled out (it's a normal idle poll, not a stall —
see the doc's part 3 section).

The ADC fix (part 5) was the deeper root cause behind the GPIO
divergence's neighbor: stock Zelda's own battery-level threshold lookup
(`FUN_0800320e`) uses up to 4 threshold tables (up to ~41974) depending
on a charging-state flag, and the ADC stub's fixed `13500` only cleared
the smaller table — QEMU read battery level 0 where real hardware (with
an actual battery) reads nonzero, silently skipping an entire
boot-progress branch. Checkpoint-confirmed after the fix: `FUN_0800ec7a`
now takes the identical internal branch as real hardware (`lr` matches,
didn't before), and the per-pass event-queue dispatch
(`FUN_0800e8e2` receiving a real event instead of always `0`) now fires
identically too.

**Still open**: real hardware's route to the LTDC HAL init
(`0x08013704`) turned out not to be the event-queue path just fixed
(confirmed both targets already match there, taking case 1 not the
case-3 path toward `0x08013704`) — a different, not-yet-identified call
to `FUN_0800eb90(3)` is the actual trigger. See
`docs/session-2026-07-12-breakpoint-lockstep-tracing.md`'s "Follow-up
session (same day, part 5)" section for the full efficient-tracing
playbook and exact next-step addresses (other callers of `FUN_0800eb90`,
or working backward from real hardware's observed `lr` at the
`0x08013704` checkpoint). No display/audio on QEMU yet.

Also landed: `scripts/make_zelda_patched_bank1.py` (local standby-patch
reproduction for real-hardware tracing stability) and
`scripts/boot_qemu.sh --patched` + real `-audiodev`/SAI1 audiodev wiring
(QEMU was launching with no audio backend at all).
