# Status

Last updated: 2026-07-10

## Where things stand

Repo is a fork of upstream QEMU (`qemu/qemu`), pinned to tag `v9.2.4`.
Working branch `gnw-h7b0` (based on that tag), pushed to `origin` as of
Phase 0 (not re-pushed since; push only on explicit go-ahead). **Phase 0
and the memory-map portion of Phase 1 are done**: `gnw-h7b0` boots a bare
Cortex-M7 against the real STM32H7B0 SRAM/flash bank layout.

## Current phase

**Phase 1 — Memory map + boot path** (see `docs/roadmap.md`): memory map
done, RCC stub not started.
- [x] Full real SRAM map added, sourced from RM0455 Table 6 (`rm0455.pdf`,
  repo root), cross-checked against `STM32H7B0.svd`'s RCC clock-enable bit
  names: ITCM (`0x0`, 64K), DTCM (`0x20000000`, 128K), AXI SRAM1/2/3
  (`0x24000000`/256K, `0x24040000`/384K, `0x240A0000`/384K), AHB SRAM1/2
  (`0x30000000`/64K, `0x30010000`/64K), SRD SRAM (`0x38000000`/32K),
  backup SRAM (`0x38800000`/4K).
- [x] Internal flash: bank1 `0x08000000`, bank2 `0x08100000`, 256K each.
  **Deliberately overrides RM0455**, which says H7B0 has only 128K
  single-bank flash — that's wrong for real silicon per the project owner
  (community-verified, undocumented by ST). See
  `docs/h7b0-flash-discrepancy.md` before touching this.
- [x] External OSPI flash reserved at `0x90000000`, 64M placeholder (real
  size 1-256M). Not yet modeled as a real OCTOSPI device — plain RAM for
  now. Target is dual-quad OCTOSPI1+OCTOSPI2 (the Tim Scheuerwegen SD-card
  mod's SPI2/OSPI2 path), not yet implemented at the register level.
- [x] Removed the Phase-0 "alias AXI SRAM at address 0" hack: ITCM is
  genuinely RAM at address `0x0` on real hardware, so the test kernel now
  loads there directly — architecturally correct, not a stand-in.
  Re-verified via gdb (`-s -S`, stepi): SP loads as `0x10000` (top of real
  64K ITCM), PC starts at the vector table's reset handler and advances on
  single-step.
- [ ] **Open question, not yet resolved**: real firmware bigger than 64K
  can't fit in ITCM the way this test kernel does. Real hardware boots
  from flash bank 1 via BOOT_ADD option-byte selection, which is a real
  address-0 remap distinct from ITCM's own fixed mapping — not yet
  modeled. Needs solving before any real (non-toy) firmware image can
  boot. See the comment in `hw/arm/gnw_h7b0_soc.c` above the ITCM region
  init.
- [ ] Minimal RCC stub: not started. Needed so real firmware's clock-init
  polling loops don't hang forever on a permanently-zero status bit.

## Known constraints

- No STM32H7B0 machine exists upstream; everything here is new.
- Keep `../minicraft-gnw`'s MPS2 fault-trap QEMU harness as the working
  regression baseline until this fork's model is proven equivalent.
- RM0455 is known-wrong about internal flash size on real H7B0 — see
  `docs/h7b0-flash-discrepancy.md` before trusting it on flash topics.
