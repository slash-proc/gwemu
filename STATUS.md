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
(Mario, Zelda) booting to visible display output — not yet reached. See
`docs/session-2026-07-12-stock-firmware-boot-investigation.md` for the
full narrative. Two real fixes landed so far (`RCC_RSR.SFTRSTF`, PA0/WKUP1
GPIO reset default); current blocker is a main-superloop timeout counter
that never arms because its enable field is never written — not yet
root-caused to a specific missing register/condition.

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
In progress — see `docs/session-2026-07-12-stock-firmware-boot-investigation.md`.
Stock firmware has no bootloader (linear execution from bank 1); the
process is: boot → sample PC/registers at the stuck point (verify with a
`stepi` burst before trusting it's really stuck — MMIO-instruction
monitor-sampling bias is real) → decompile via `gnw-mario-decomp`/
`gnw-zelda-decomp`'s Ghidra tooling → cross-reference `STM32H7B0.svd`,
`rm0455.pdf`, `sdk/`, and `gnwmanager`'s `mario.py`/`zelda.py` (real,
hash-verified patch offsets with comments on real firmware behavior) →
implement the fix directly in the relevant device model → rebuild →
repeat. Live gdb pokes and binary patches are diagnostic aids only, never
the fix. Two real fixes landed so far: `RCC_RSR.SFTRSTF` reset value, and
PA0/WKUP1 no longer forced low at GPIO reset. Current blocker: a
main-superloop timeout counter (both games) never arms because its enable
field is never written by anything — not yet root-caused.
