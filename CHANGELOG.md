# Changelog

## 2026-07-10

- Repo initialized as a fork of upstream QEMU. Remotes set: `origin` =
  `slash-proc/gwemu`, `upstream` = `qemu/qemu`. Fetched full upstream
  history/tags locally; pushed only the `v9.2.4` stable tag to `origin` as
  the pinned base (deliberately not syncing all of upstream master/tags to
  the fork).
- Initial documentation established: `CLAUDE.md`, `STATUS.md`,
  `CHANGELOG.md`, `docs/roadmap.md`.
- Phase 0 complete: `gnw-h7b0` machine added (`hw/arm/gnw_h7b0.c`,
  `hw/arm/gnw_h7b0_soc.c`) — bare Cortex-M7 + DTCM + AXI SRAM, no
  peripherals. Verified booting a hand-built spin-loop ELF via gdb
  (`-s -S`): reset SP/PC resolve correctly and PC advances on single-step.
  Branch `gnw-h7b0` (based on tag `v9.2.4`) pushed to `origin`.
- Added `rm0455.pdf` (STM32H7B0 reference manual) to the repo; used it to
  build out the real SRAM/flash memory map (ITCM, DTCM, AXI SRAM1/2/3,
  AHB SRAM1/2, SRD SRAM, backup SRAM, internal flash banks, external OSPI
  flash placeholder) in `gnw_h7b0_soc.{c,h}`, replacing Phase 0's single
  DTCM/AXISRAM blob. Discovered and documented (`docs/h7b0-flash-
  discrepancy.md`) that RM0455 is wrong about internal flash size/layout
  on real H7B0 hardware (says 128K single-bank; real silicon is 2x256K
  dual-bank, per community/project-owner knowledge) — deliberately
  overrode the reference manual here.
- Replaced the Phase-0 "alias AXI SRAM at address 0" boot hack with
  loading the test kernel directly into ITCM (genuinely mapped at `0x0`
  on real hardware) — architecturally correct rather than a stand-in.
  Re-verified boot via gdb. Flagged an open question: real firmware >64K
  needs flash-backed boot via BOOT_ADD option-byte selection, not yet
  modeled.
- Added `scripts/fetch-sdk.sh`, pulling the real STM32CubeH7 HAL driver +
  device CMSIS source into gitignored `sdk/`, pinned to the same versions
  `game-and-watch-retro-go-sd` builds real firmware against.
- Implemented and verified a minimal RCC device stub
  (`hw/misc/gnw_h7b0_rcc.{c,h}`) mirroring RCC_CR ON->RDY bits and
  RCC_CFGR SW->SWS so real firmware's clock-init polling doesn't hang.
  Verified via a real CPU-executed test program. **NOT YET COMMITTED**
  as of this entry — see STATUS.md "Uncommitted work" section; a Claude
  Code session restart interrupted the commit step (unrelated
  auto-mode-classifier false positives blocking git commands, not a
  problem with the code itself).
