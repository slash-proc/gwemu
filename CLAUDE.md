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

## Repo/remote conventions

- `origin` = `slash-proc/gwemu` (this fork, push target).
- `upstream` = `qemu/qemu` (read-only, fetch only, never push).
- Pinned base: tag `v9.2.4`. Don't casually rebase onto upstream `master`;
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

## Build

Standard QEMU meson build, ARM softmmu target:
```
mkdir build && cd build
../configure --target-list=arm-softmmu
ninja
```
(Board-specific build/run instructions will be added here once the
`gnw-h7b0` board exists.)
