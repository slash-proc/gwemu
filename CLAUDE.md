# CLAUDE.md

Rules and orientation for working in this repo. Keep this short and
directive; put rationale and history elsewhere (see below).

## What this repo is

`gwemu` (fork name `slash-proc/gwemu`) is a hard fork of upstream QEMU,
adding a real machine model for the Nintendo Game & Watch's STM32H7B0 SoC.
It exists because no STM32H7B0 machine exists in upstream QEMU. Modeled
after how `xemu` gives Xbox homebrew devs a fast, accurate dev loop against
real NV2A GPU behavior — see `docs/roadmap.md` for the full phased plan.

## Doc map

- `STATUS.md` — current snapshot only (what phase, what works, what's next).
  Rewritten in place as state changes, not appended to. Keep under ~100
  lines.
- `CHANGELOG.md` — dated one-line entries of what landed. This is where
  "what happened" lives, not STATUS.md.
- `docs/` — design rationale and reference material that doesn't fit as a
  code comment (e.g. clock-tree math, known hardware/datasheet
  discrepancies). Not a running diary: once an investigation's findings are
  acted on, the "why" belongs as a comment next to the code it justifies,
  and the writeup gets deleted rather than accumulated. If you're about to
  write a dated `session-*.md` narrative doc, prefer a code comment or a
  CHANGELOG entry instead.
- `docs/peripheral-register-audit/` — per-peripheral register-behavior notes
  from cross-checking against the SVD/reference manual/HAL source. Reference
  material, not narrative.
- `docs/h7b0-clock-tree-findings.md` — hard-won HSI-vs-HSE, PLL2 VCO/
  fractional-N formula, SAI1SEL mux, and TIM2/HCLK clock-tree facts;
  consult before adding real-clock-dependent behavior to any new peripheral
  instead of re-deriving or re-guessing a frequency.
- **`gnwmanager` (a separate tool, not part of this repo)**: if you use it
  for real-hardware or `--qemu` gdbstub workflows, default to not adding
  new capabilities to it for this repo's convenience — treat it as a
  dependency with its own independent development, and prefer writing
  pure-consumer scripts against its existing public API. Small, targeted
  bugfixes in its own code are fine when something it does is actually
  broken; growing new features into it to serve this repo is not the
  default.

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
  STM32CubeH7 HAL driver + device-specific CMSIS source used by real
  Game & Watch firmware, so it reflects what real firmware actually does.
  Run `scripts/fetch-sdk.sh` to populate it if missing. Use
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
- Real hardware reports imprecise BusFaults; QEMU's bus model is fully
  synchronous — don't assume QEMU's default fault timing matches real
  silicon without checking.
- Stock (official Nintendo) firmware ships with no debug symbols, so
  tracing what it's actually waiting on requires disassembly/decompilation
  (e.g. Ghidra, imported at base `0x08000000`, `ARM:LE:32:Cortex`). Always
  boot the user's supplied stock firmware images unmodified for
  stock-accuracy work — don't apply community firmware patches (save-data
  erasure, encryption bypass, etc.) meant for producing custom-firmware
  builds; those are a different use case.

## Build

Standard QEMU meson build, ARM softmmu target:
```
mkdir build && cd build
../configure --target-list=arm-softmmu
ninja
```
(Board-specific build/run instructions will be added here once the
`gnw-h7b0` board exists.)
