# Status

Last updated: 2026-07-13

## Where things stand

Repo is a fork of upstream QEMU (`qemu/qemu`), pinned to tag `v11.0.2`.
Working branch `gnw-h7b0`. Phase 0/1 (boot path, memory map) and Phase 2/3
(DMA2D, LTDC display) are done. retro-go (homebrew) firmware boots
end-to-end through the SD-backed build, with working audio, gamepad input,
JPEG-decoded cover art, and menu/gameplay rendering. Mario and Zelda's
gnwmanager CFW images both boot to a fully interactive clock face, with
working GAME/PAUSE menus and game launching (see below). Not push-worthy
yet — local commits only, push only on explicit go-ahead.

**Mario/Zelda CFW GAME/PAUSE menu invisibility — RESOLVED, user-confirmed
live.** Root cause was two real bugs in QEMU's LTDC compositor
(`hw/display/gnw_h7b0_ltdc.c`), not a firmware issue: (1) pixel format
AL44 (used for anti-aliased overlay text) was completely unimplemented,
silently skipping Layer2 compositing entirely; (2) Layer1/Layer2
compositing order was backwards (real STM32 LTDC always shows Layer2
above Layer1). Both fixed; GAME→A now launches a game with the menu
visibly rendering throughout. Full investigation (an extensive, multi-part
live-tracing session covering button input, a firmware state machine, and
the render dispatch chain, all eventually exonerated) is in
`docs/session-2026-07-12-breakpoint-lockstep-tracing.md` parts 15-19.

**Mario CFW running at ~half real-hardware speed — partially fixed, one
residual gap open.** User confirmed via direct real-hardware comparison
(not just expectation). Root-caused and fixed a real bug: `RCC_CR`'s
write handler mirrored every oscillator's `*ON` bit into its `*RDY` bit
except CSI, which firmware requests as part of its PLL1 setup sequence —
added the missing `CSION`→`CSIRDY` mirror in `hw/misc/gnw_h7b0_rcc.c`.
This fixed PLL1 actually locking (confirmed via `gnwmanager`'s
`OpenOCDBackend`: every clock register — `PLLCKSELR`, `PLL1DIVR`,
`CDCFGR1`, `CR`, `CFGR` — now matches real hardware bit-for-bit, where
before QEMU was permanently stuck on HSI while real hardware ran on
PLL1). **Still open**: even with every register matching real hardware
exactly, a precisely-measured firmware tick counter still runs at ~half
real hardware's rate — meaning there's one more bug, purely inside QEMU's
own internal clock-propagation/timing code (not firmware- or
register-visible). Next-session starting point (already narrowed to
`gnw_h7b0_rcc_update_sysclk_clock()`/`armv7m_systick.c` timing) is in
`docs/session-2026-07-12-breakpoint-lockstep-tracing.md` part 20.

**Actively in progress:** getting *stock* (official Nintendo, unpatched)
Game & Watch firmware booting to visible display output — separate from
the CFW work above, not yet reached. Process: boot both QEMU and real
hardware to the real reset entry point, walk forward checkpoint-by-
checkpoint via real breakpoints cross-referenced against Ghidra
decompilation, compare core registers, narrow toward the first real
divergence. Currently parked mid-trace at an OCTOSPI1 RSTEN-command
self-trap firmware takes on QEMU that it doesn't take on real hardware —
see `docs/session-2026-07-12-breakpoint-lockstep-tracing.md` (parts 6-10)
and `docs/session-2026-07-12-stock-firmware-boot-investigation.md` /
`-register-snapshot-diffing.md` for the full trail. Several real fixes
already landed along the way (`RCC_RSR.SFTRSTF`, GPIO reset defaults,
`GNW_H7B0_ADC_FULL_BATTERY_RAW`, OCTOSPI1/2 IRQ line, OSPI auto-polling,
a minimal OTFDEC model, CRYP AES-GCM).

## What works

- Core CPU, memory map, NVIC, real clock tree (HSI/HSE/PLL1-3, live-derived
  SYSCLK/HCLK/LTDC-pixel-clock — see `docs/h7b0-clock-tree-findings.md`),
  dynamic CPU-overclock support (retro-go's NORMAL/BOOST1/BOOST2 modes,
  and now Mario CFW's own PLL1 overclock sequence).
- SPI/OSPI flash, SD card (SPI-based), RTC, DMA (accumulator-scheduled, no
  drift), TIM2, ADC, PWR, CRC, CRYP (AES-GCM), OTFDEC (KEYCRC), and the
  rest of the boot-path peripheral set.
- LTDC: real per-layer compositing with the correct Layer1-below/Layer2-
  above order, real AL44 + L8 + RGB565 + ARGB8888/1555/4444 pixel formats,
  color key, window-clip/default-color, generalized constant-alpha/
  pixel-alpha blend formula, Bayer dithering, independent Layer1/Layer2
  CLUTs, correct RRIF/reload-timing semantics, vblank capture gated on
  firmware's actual `SRCR.VBR` write instead of a fixed timer.
- DMA2D: real per-pixel fetch for every input format retro-go uses
  (ARGB8888/RGB565/ARGB1555/ARGB4444/L8/YCbCr/A8), real `BLEND_BG`/`BLEND_FG`
  "fixed color" semantics, 3-bit `MODE` field.
- JPEG: real polled `DOR` output register, correct chroma subsampling,
  `OFTF`-modeled output FIFO threshold for the real bulk-read polling path.
- Audio: confirmed solid across every core tested (NES, Genesis, SMW,
  others).

## Known issues (open)

- **Mario CFW clock speed**: PLL1 now locks correctly (see above), but a
  precisely-measured firmware tick rate still runs at ~half real
  hardware's — root cause narrowed to QEMU's own clock-propagation/timing
  code, not yet found. See doc part 20.
- **Coverflow/menu flicker**: selected item's text and the leftmost
  background cover flicker, worse with a menu open over the carousel.
  Extensively investigated in `docs/session-2026-07-11-ltdc-flicker-investigation.md`
  and `-dma2d-jpeg-ycbcr-pipeline.md`. LTDC/DMA2D/JPEG pixel-fidelity gaps
  ruled out (all fixed, didn't help); a vblank-capture timing change
  landed the same investigation but was never confirmed live to actually
  fix the flicker — needs retest. Known gap in that fix: content that
  never writes `SRCR.VBR` (single-buffered/IMR-only paths) doesn't get
  captured — not yet observed as a real regression, but worth watching.
- Real subsampled chroma storage was traded for full-resolution internal
  storage in the JPEG model (a documented scope decision, not a bug).
- Real JPEG-decode performance is inherently register-access-heavy;
  coverflow screens (5 covers redecoded every frame) are still the most
  expensive on-screen content by a wide margin.
- Stock (unpatched) firmware boot is still blocked on an OCTOSPI1 RSTEN
  self-trap divergence from real hardware — see "Actively in progress"
  above.

## Tooling notes worth keeping in mind

- `gnwmanager`'s `OpenOCDBackend`/`GDBBackend` let you read real hardware
  registers directly for ground truth — use this early when a QEMU vs.
  real-hardware behavioral question comes up, not as a last resort.
- QEMU's gdbstub halts the whole VM (including peripheral input-event
  delivery, not just the vCPU) on client connect — reading peripheral
  state while halted can show stale pre-input values; verify via
  `query-status` that the VM is genuinely running, and prefer letting it
  run freely between short, deliberate halts over long watchpoint loops
  that keep it halted most of the time.
- A full (non-`-noanalysis`) Ghidra auto-analysis pass takes only ~5s on
  this binary and is needed for reliable RAM-address xref tracing —
  `-noanalysis` only finds reads of flash literal-pool pointers, not
  writes to the RAM addresses they resolve to.
- Temporary `fprintf(stderr, ...)` debug prints added directly to device
  model source (rebuilt via `ninja qemu-system-arm`) are the fastest way
  to see internal QEMU device state that guest-side gdb reads can't reach
  at all — always remove them again once their diagnostic purpose is served.
