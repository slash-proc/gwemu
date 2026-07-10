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
done, RCC stub implemented and verified but NOT YET COMMITTED (see
"Uncommitted work" below — a Claude Code session restart interrupted
this; next session should commit it first thing after checking it still
builds).
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
- [x] Minimal RCC stub implemented (`hw/misc/gnw_h7b0_rcc.{c,h}`, mapped
  at `0x58024400` per RM0455/CMSIS cross-check): mirrors RCC_CR's
  HSION/HSEON/PLL1-3ON bits into their matching RDY bits, and RCC_CFGR's
  SW field into SWS, both instantly (not cycle-accurate) so a real
  clock-init polling loop won't hang. Everything else is a plain
  read-what-was-written shadow register. **Verified working** via a
  real CPU-executed test program (not gdb debug-writes, which turned out
  to not reliably exercise MMIO access-size paths the same way real STR
  instructions do): writing `HSION|HSEON|PLL1ON` reads back with
  `HSIRDY|HSERDY|PLL1RDY` correctly OR'd in (`0x01010001` ->
  `0x03030005`), and writing `CFGR.SW=2` reads back with `SWS` correctly
  mirrored (`0x2` -> `0x12`). Deliberately NOT based on the existing
  upstream `hw/misc/stm32_rcc.c`: that device is F4-family register
  offsets and does not mirror ON->RDY bits at all, so reusing it would
  leave real H7B0 clock-init hanging.
  - Bug found and fixed along the way: initial version had
    `.valid.min_access_size = 4`, which silently dropped narrower
    (1-2 byte) writes — relaxed to `min_access_size = 1`.
  - Bug found and fixed along the way: a stray `*/` inside a doc comment
    in `gnw_h7b0_rcc.h` closed the comment block early, corrupting
    everything after it including the struct definition, causing
    "incomplete type" compile errors that looked unrelated to the actual
    typo.

## Uncommitted work (as of this restart)

The RCC device files are written, build clean, and are verified working
(see above), but were NOT git-committed before this Claude Code session
had to restart (repeated auto-mode classifier false-positives blocked
`git add`/`git commit` calls — unrelated to the actual changes, which are
a normal device-model addition). Files involved:
- `hw/misc/gnw_h7b0_rcc.c`, `include/hw/misc/gnw_h7b0_rcc.h` (new)
- `hw/arm/gnw_h7b0_soc.c`, `include/hw/arm/gnw_h7b0_soc.h` (wire up RCC)
- `hw/arm/Kconfig`, `hw/misc/Kconfig`, `hw/misc/meson.build` (build reg)

**Next session should**: `git status` to confirm these are still present
and uncommitted, rebuild to confirm it still compiles, then commit before
doing anything else.

## Known constraints

- No STM32H7B0 machine exists upstream; everything here is new.
- Keep `../minicraft-gnw`'s MPS2 fault-trap QEMU harness as the working
  regression baseline until this fork's model is proven equivalent.
- RM0455 is known-wrong about internal flash size on real H7B0 — see
  `docs/h7b0-flash-discrepancy.md` before trusting it on flash topics.
