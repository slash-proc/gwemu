# Status

Last updated: 2026-07-10

## Where things stand

Repo is a fork of upstream QEMU (`qemu/qemu`), pinned to tag `v9.2.4`.
Working branch `gnw-h7b0` (based on that tag) pushed to `origin`
(`slash-proc/gwemu`). **Phase 0 complete**: a new `gnw-h7b0` machine boots
a bare Cortex-M7 to a working spin loop.

## Current phase

**Phase 0 — Fork setup**: done.
- [x] Working branch `gnw-h7b0` created from `v9.2.4`, pushed to origin.
- [x] Baseline `arm-softmmu` build confirmed working unmodified.
- [x] `hw/arm/gnw_h7b0_soc.c` + `include/hw/arm/gnw_h7b0_soc.h`: SoC
      container with Cortex-M7, DTCM (`0x20000000`, 128K), AXI SRAM
      (`0x24000000`, 1M) — no peripherals yet. `hw/arm/gnw_h7b0.c`: thin
      machine-init wrapper, `-M gnw-h7b0`. Registered in
      `hw/arm/{Kconfig,meson.build}`.
  - Temporary Phase-0-only hack in the SoC realize function: AXI SRAM is
    aliased at address `0x0` so a kernel loaded there is reachable at the
    Cortex-M hardwired reset-vector address. Real hardware does this via
    flash/XIP boot; this alias must be removed once Phase 1 adds a real
    flash model — see the comment at its definition in
    `hw/arm/gnw_h7b0_soc.c`.
- [x] Verified via gdb (`-s -S`, stepi): reset SP loads as `0x24100000`
  (top of AXI SRAM, matches a hand-built vector table), PC starts at the
  vector table's reset handler and advances correctly on single-step —
  confirms the boot/build/board-registration pipeline works end to end.

**Next: Phase 1 — Memory map + boot path** (see `docs/roadmap.md`):
ITCM/AHB SRAM/internal flash/QSPI regions, a minimal RCC stub, and
removing the Phase-0 address-0 alias hack in favor of a real flash-at-0x0
mapping.

## Known constraints

- No STM32H7B0 machine exists upstream; everything here is new.
- Keep `../minicraft-gnw`'s MPS2 fault-trap QEMU harness as the working
  regression baseline until this fork's model is proven equivalent.
