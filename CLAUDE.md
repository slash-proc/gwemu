# CLAUDE.md

Rules and orientation for working in this repo. Keep this short and
directive; put rationale and history elsewhere (see below).

## What this repo is

`gwemu` (fork name `slash-proc/gwemu`) is a hard fork of upstream QEMU,
adding a real machine model for the Nintendo Game & Watch's STM32H7B0 SoC.
It exists because no STM32H7B0 machine exists in upstream QEMU, and the
fault-trap-based workaround used in `../minicraft-gnw` for pre-hardware
testing is slow and only covers a narrow slice of real hardware behavior.
Modeled after how `xemu` gives Xbox homebrew devs a fast, accurate dev loop
against real NV2A GPU behavior — see `docs/roadmap.md` for the full
phased plan.

## Doc map

- `STATUS.md` — current snapshot only (what phase, what works, what's next).
  Rewritten in place as state changes, not appended to. Keep under ~100
  lines.
- `CHANGELOG.md` — dated one-line entries of what landed. This is where
  "what happened last week" lives, not STATUS.md.
- `docs/` — design rationale, architecture notes, dated investigation
  writeups for anything gnarly (bug hunts, root-causes). Narrative and
  history belong here, not in STATUS.md.
- `docs/session-2026-07-10-state.md` / `-part2-state.md` — historical
  investigation writeups from an earlier cut-off session. All four
  open problems they tracked (chainloader's menu not drawing,
  retro-go's `Error_Handler` crash, extflash not wired up, a
  release-build perf hang) are now resolved — see CHANGELOG.md's
  2026-07-10 entries (LTDC vblank/IRQ, RTC device, SPI `TXP` fix, real
  OSPI/CRC command decoding) for what actually fixed them. Kept for
  gdb-workflow notes and DMA2D/LTDC history, not as a live TODO list.
- `docs/h7b0-clock-tree-findings.md` — hard-won HSI-vs-HSE, PLL2 VCO/
  fractional-N formula, SAI1SEL mux, and TIM2/HCLK clock-tree facts
  confirmed via live gdb tracing against real firmware; consult before
  adding real-clock-dependent behavior to any new peripheral (ADC timing,
  timer-driven devices, SPI/OSPI baud-rate fidelity) instead of
  re-deriving or re-guessing a frequency.
- `docs/session-2026-07-10-part3-gamepad-sd-debugging.md` — real xpad
  gamepad support, a real physical SD card testing session, and a real
  DMA2D device model. Documents the disassembly-only (no debug symbols)
  gdb technique used to find most of that session's bugs, and the
  "firmware polls a status flag our stub never sets" pattern that
  accounted for most of them (SPI1 RXP, ssi-sd.c CMD58, JPEG EOCF/IFTF/
  IFNFF, TIM2-block UIF, RTC ICSR write-flags). Has an open problem (a
  patched Zelda firmware image still hangs past ADC config) and two
  known-but-not-root-caused issues (LTDC flicker, game logic running too
  fast) flagged as the next session's starting point.
- `docs/session-2026-07-11-ltdc-flicker-investigation.md` /
  `-dma2d-jpeg-ycbcr-pipeline.md` — the LTDC/JPEG/DMA2D coverflow-flicker
  investigation: RRIF fix and LTDC per-layer compositing generalization
  (color key/window-clip/blend/dither/L2 fixes) landed but didn't fix the
  flicker; a real DMA2D `BLEND_BG`/`BLEND_FG` ("fixed color") mode
  implementation and a real JPEG `DOR`-register/guest-RAM output pipeline
  (replacing an RGB565 "hack buffer" pointer handoff) fixed two real
  corruption bugs (a stale-`BGMAR` read, a chroma-subsampling size
  mismatch) but still didn't fix the underlying flicker. Read the second
  doc's "Workflow/tooling notes" section before debugging LTDC/DMA2D/JPEG
  timing further — it has a real gotcha about misreading shared
  scratch-buffer reuse as instability, plus current gdb/monitor/launch
  workflow notes. Flicker root cause is still open; next steps are listed
  there.

## Repo/remote conventions

- `origin` = `slash-proc/gwemu` (this fork, push target).
- `upstream` = `qemu/qemu` (read-only, fetch only, never push).
- Pinned base: tag `v11.0.2`. Don't casually rebase onto upstream `master`;
  bump the pin deliberately and note it in CHANGELOG.md when we do.
- All game-and-watch-specific additions live in-tree (like xemu's
  `hw/xbox/`), primarily under `hw/arm/` (SoC/board) and `hw/display/`
  (DMA2D device model), not as an out-of-tree build — QEMU's Kconfig/
  meson board registration isn't designed for out-of-tree boards.

## Source-of-truth rules

- `STM32H7B0.svd` at repo root is the authoritative register/address map.
  Use it for peripheral base addresses and register layouts instead of
  hand-transcribing from the reference manual.
- `rm0455.pdf` at repo root (STM32H7A3/7B3/7B0 reference manual) is
  expected to exist locally for memory-map/register lookups but is
  gitignored (58MB, copyrighted ST document) — not tracked, won't survive
  a fresh clone. Re-fetch it yourself if it's missing.
- `sdk/` (gitignored, ~65MB, not a build dependency) is the real
  STM32CubeH7 HAL driver + device-specific CMSIS source, pinned to the
  same versions `../game-and-watch-retro-go-sd/Makefile.common` builds
  real firmware against (so it reflects what real firmware actually
  does). Run `scripts/fetch-sdk.sh` to populate it if missing. Use
  `sdk/stm32h7xx-hal-driver/Src/stm32h7xx_hal_{rcc,dma2d,ospi}.c` and
  `sdk/cmsis-device-h7/Include/stm32h7b0xx.h` as ground truth for
  peripheral register behavior when writing device models — same role as
  `STM32H7B0.svd`/`rm0455.pdf`, but showing actual driver logic (e.g.
  which status bits a real init sequence polls for) rather than just
  register layout.
- RM0455 is wrong about internal flash on real H7B0 silicon (says 128K
  single-bank; real hardware is 2x256K dual-bank, community-verified, not
  documented anywhere official). Trust the project owner over RM0455 here.
  See `docs/h7b0-flash-discrepancy.md` before touching flash sizing.
- Cross-check emulated behavior against real findings already documented in
  `../minicraft-gnw/docs/qemu-testing.md` and
  `../minicraft-gnw/docs/real-hardware-testing.md` (e.g. real hardware
  reports imprecise BusFaults; QEMU's bus model is fully synchronous —
  don't assume QEMU's default fault timing matches real silicon without
  checking).
- DMA2D pixel-math semantics (RGB565/ARGB8888 conversion, CLUT blend) should
  be reimplemented from understood semantics referencing
  `../minicraft-gnw/tools/retro-go-porting-toolkit/host/qemu/dma2d_emu.h`
  and friends — not copied verbatim from `mk-snes/gnw-mk` (license status
  there isn't established for reuse).
- Keep `../minicraft-gnw`'s existing MPS2 fault-trap harness working and
  untouched throughout this project — it's the regression baseline until
  the real machine model is proven equivalent.
- Stock (official Nintendo) firmware ships with no debug symbols, so
  tracing what it's actually waiting on requires Ghidra decompilation.
  `../gnw-mario-decomp` and `../gnw-zelda-decomp` are sibling repos (one
  per game) holding that work: a Ghidra project per game
  (`ghidra-proj/{Mario,Zelda}Proj`, imported from
  `backup/internal_flash_backup_{mario,zelda}.bin` at base `0x08000000`,
  `ARM:LE:32:Cortex`) plus a shared headless-scripting toolkit in each
  repo's `scripts/` (`DecompAt.java` decompiles a given address,
  `FindCallers2.java`/`FindXrefs3.java` find callers/references,
  `FindMovtScalar.java` finds MOVW/MOVT-split 32-bit immediate loads that
  raw literal-pool byte search misses, `CreateFunctions2.java` forces a
  function boundary Ghidra's auto-analysis didn't create). Run via
  `$GHIDRA_HOME/support/analyzeHeadless <repo>/ghidra-proj <ProjName>
  -process internal_flash_backup_<game>.bin -noanalysis -scriptPath
  <repo>/scripts -postScript <Script>.java <args>` (`-noanalysis` reuses
  the project's already-computed auto-analysis). Each repo's own
  `README.md` has more detail; `docs/session-2026-07-12-stock-firmware-boot-investigation.md`
  has the current findings.
- `gnwmanager` (separate local repo,
  `gnwmanager/gnwmanager/cli/gnw_patch/{mario,zelda}.py`) is a real,
  SHA1-hash-verified firmware patcher for these same stock images —
  its inline comments describe what specific patched addresses actually
  do on real hardware (e.g. the "warm-boot power-off fix" / "state-6
  standby" comments that led directly to a real `gnw_h7b0_gpio.c` fix).
  Treat it as ground truth, not a theory, and check it periodically when
  stuck on a boot-path address — it covers more of stock firmware's
  behavior than has been traced here so far. Its *data*-modification
  patcher steps (erasing save regions, disabling save encryption,
  removing games to save space) are for producing gnwmanager's own
  custom-firmware builds and should not be replicated when testing stock
  boot in qemu-gnw — always boot the user's supplied
  `backup/flash_backup_{mario,zelda}.bin` unmodified.

## Build

Standard QEMU meson build, ARM softmmu target:
```
mkdir build && cd build
../configure --target-list=arm-softmmu
ninja
```
(Board-specific build/run instructions will be added here once the
`gnw-h7b0` board exists.)
