# Changelog

## 2026-07-10

- Added a real LTDC device (`hw/display/gnw_h7b0_ltdc.c`): Layer1/
  RGB565-only, reads the guest framebuffer each frame and blits it
  (2x nearest-neighbor upscaled) into a real QEMU display window.
  Fixed a row-pitch bug along the way (`LxCFBLR`'s `CFBLL` field isn't
  the real stride, `CFBP` is). Also upgraded SPI2 from a plain-RAM stub
  to a real device (`hw/misc/gnw_h7b0_spi.c`) after the LCD-panel
  init-command path hung the same way OSPI/ADC previously did.
  `gnw-chainloader` now boots and runs continuously with a real window
  showing the LCD console output.

- Booted a real `gnw-chainloader` firmware image end-to-end for the
  first time and fixed the resulting chain of boot-path gaps: RCC's
  `CSR`/`BDCR` LSI/LSE ready-bit mirroring; new real devices for PWR
  (`hw/misc/gnw_h7b0_pwr.c`), OCTOSPI1/2 (`gnw_h7b0_ospi.c`), and
  ADC1/2 (`gnw_h7b0_adc.c`), each mirroring a hardware-set status bit
  real firmware polls after enabling the peripheral; plain-RAM
  placeholders for DBGMCU, the flash controller register block, FMC,
  GPIOA-K, CRS, the OCTOSPI IO manager, and SPI2. Boot now reaches real
  LTDC init and stops cleanly there (BusFault caught by the firmware's
  own crash handler) — see STATUS.md for the full gap-by-gap
  breakdown and what's next (LTDC device model).

- Phase 1 boot path resolved: the kernel now loads at flash bank 1
  (`0x08000000`) instead of ITCM, with the ARMv7M CPU's `init-nsvtor`
  property pointed there so reset reads the initial SP/PC from flash's
  vector table — modeling real hardware's BOOT_ADD address-0 remap (or
  the gnwmanager debug-probe dev-flow path) without a fake alias memory
  region. Note: it's `init-nsvtor`, not `init-svtor` — Cortex-M7 has no
  TrustZone-M, so the `-s-` (secure) variant is a silent no-op on this
  core. Verified via a hand-built flash-linked test kernel under gdb
  (`-s -S`): SP/PC load as `0x20020000`/`0x0800000c` from the flash
  vector table, and a store instruction a few steps in lands correctly
  in AXI SRAM.

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
