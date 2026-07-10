# Status

Last updated: 2026-07-10

## Where things stand

Repo is a fork of upstream QEMU (`qemu/qemu`), pinned to tag `v9.2.4`.
Working branch `gnw-h7b0` (based on that tag), pushed to `origin` as of
Phase 0 (not re-pushed since; push only on explicit go-ahead). **Phase 0
and the memory-map portion of Phase 1 are done**: `gnw-h7b0` boots a bare
Cortex-M7 against the real STM32H7B0 SRAM/flash bank layout.

## Current phase

**Phase 1 — Memory map + boot path** (see `docs/roadmap.md`): memory map,
RCC stub, and boot path are all done and committed.
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
- [x] **Boot-from-flash resolved**: the kernel now loads at flash bank 1
  (`FLASH_BANK1_BASE_ADDRESS`, `0x08000000`) instead of ITCM, matching
  where retro-go's own linker script (`STM32H7B0VBTx_FLASH.ld`) places
  `.isr_vector` and where the real gnwmanager debug-probe dev flow starts
  execution. The ARMv7M CPU's `init-nsvtor` property (the CPU's initial
  VTOR, which `cpu_reset()` reads SP/PC from) is set to
  `FLASH_BANK1_BASE_ADDRESS` in `gnw_h7b0_soc.c`, modeling real
  hardware's BOOT_ADD address-0 remap without a fake alias memory
  region. **Gotcha**: it's `init-nsvtor`, not `init-svtor` — Cortex-M7
  has no TrustZone-M, so the secure-VTOR property is a silent no-op on
  this core (found by first setting `init-svtor` and seeing PC/SP still
  reset to 0/0 — QEMU doesn't error, it just ignores an unknown
  property via `object_property_find`). Verified via a hand-built
  flash-linked test kernel under gdb (`-s -S`, stepi): SP/PC load as
  `0x20020000`/`0x0800000c` from the flash vector table, and a store
  instruction lands correctly in AXI SRAM a few steps later. ITCM is
  still modeled as real RAM at `0x0` (unchanged) — it's simply no longer
  where the kernel is loaded or where the CPU looks for its boot vector.
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

- [x] Real firmware boot exercised end-to-end for the first time, using
  a real `gnw-chainloader` build (`gnw_chainloader.bin`, not tracked in
  this repo — see `../gnw-chainloader`) since it doesn't require an
  extflash image to reach real init code, unlike retro-go. Found and
  fixed a chain of boot-path gaps by iterating "run under gdb, find the
  BusFault/hang, fix it, repeat" (methodology: same as roadmap Phase 4's
  intended approach, just pulled earlier because it was the fastest way
  to validate Phase 1's boot path against real firmware). In order
  found:
  - RCC extended: `RCC_CSR`'s LSION→LSIRDY and `RCC_BDCR`'s
    LSEON→LSERDY mirroring (same instant-approximation pattern as
    `RCC_CR`) — without these, `SystemClock_Config`'s LSI/LSE enable
    spins forever (LSERDY has no timeout in this firmware's clock init,
    a genuine infinite loop, not just a slow poll).
  - New real device `hw/misc/gnw_h7b0_pwr.c` (PWR, `0x58024800`):
    mirrors `PWR_CR3`/`PWR_SRDCR` writes into `PWR_CSR1.ACTVOSRDY` /
    `PWR_SRDCR.VOSRDY` instantly. Without this, `SystemClock_Config`'s
    supply-configuration step hangs forever polling a hardware-set-only
    status bit a plain RAM stub can never set.
  - New real device `hw/misc/gnw_h7b0_ospi.c` (OCTOSPI1/2,
    `0x52005000`/`0x5200A000`): mirrors CCR/IR writes into `SR.TCF`
    instantly (no real command/address/data-phase transfer yet — no
    bytes actually move). Without this, ST HAL's `HAL_OSPI_Command()`
    times out waiting for `SR.TCF` and returns an error, which real
    firmware treats as fatal (its own "spin forever" trap in
    `OSPI_WriteBytes`, not a QEMU crash).
  - New real device `hw/misc/gnw_h7b0_adc.c` (ADC1/ADC2, `0x40022000`,
    covers both instances + `ADC12_COMMON` in one 0x400 window):
    mirrors `CR.ADEN`→`ISR.ADRDY` per-instance. Without this,
    `board_adc_init()` hangs forever polling ADRDY. Reads always return
    0 — no real conversion semantics.
  - Plain-RAM placeholders added for peripherals that are read/written
    during boot but don't have hardware-set status bits blocking
    progress (so a dumb RAM shadow is enough): DBGMCU (`0x5C001000`),
    flash controller regs / `FLASH_ACR` (`0x52002000`, distinct from
    the memory-mapped flash content), FMC (`0x52004000`), GPIOA-K
    (`0x58020000`-`0x58022FFF`, no button/LCD-line semantics yet), CRS
    (`0x40008400`), the OCTOSPI IO manager (`0x5200B400`), and SPI2
    (`0x40003800`, the Tim Scheuerwegen SD-card mod's SPI path).
  - Confirmed real hardware fault-recovery behavior along the way:
    `gnw-chainloader` installs real `HardFault`/`BusFault`/etc handlers
    (`src/chainloader/system/crash_log.c`) that record fault context to
    a fixed SRAM address and then halt — so a BusFault from a still-gap
    peripheral is a clean, deliberate stop (PC parked in
    `crash_log_capture`), not a QEMU crash. This made gap-finding fast:
    attach gdb, read PC, `addr2line`, done.

- [x] New real device `hw/display/gnw_h7b0_ltdc.c` (LTDC, `0x50001000`):
  Layer1-only, RGB565-only, no timing/IRQ modeling — a real graphics
  console backed by `framebuffer_update_display`-style full-frame reads
  from guest RAM (`CFBAR`/`CFBP` from `LxCFBLR`, `CFBLNR` for row
  count), upscaled 2x nearest-neighbor into the host window
  (`GNW_H7B0_LTDC_SCALE`). **Gotcha**: `LxCFBLR`'s `CFBLL` field
  (`active_width*bpp+3`) is *not* the row pitch — use `CFBP` instead;
  using `CFBLL` gave a 2-pixel-per-row skew, found by comparing live
  register values (`CFBP=640` vs `(CFBLL-3)=644`) against the known
  320-wide real framebuffer. `SPI2` upgraded from a plain-RAM stub to a
  real device (`hw/misc/gnw_h7b0_spi.c`, `SR.TXP`/`SR.EOT` mirroring)
  after the LCD-panel init-command path (`gw_lcd_spi_tx`, separate from
  the LTDC pixel path) hung the same way OSPI/ADC did; new plain-RAM
  stub for SPI1. **Boot now runs continuously** past all of Phase 1's
  gaps — confirmed via a real SDL window showing the LCD console
  (currently near-black, matching guest framebuffer content being
  all-zero at this point in `gnw-chainloader`'s boot, before its UI
  draws anything).

## Next up

Continue the whack-a-mole: run `gnw_chainloader.bin` further and fix
whatever the next BusFault/hang is (same methodology as above — attach
gdb, read PC, `addr2line`, find the register in the SVD/CMSIS headers).
Phase 2's DMA2D device model (see `docs/roadmap.md`) is the next
big-ticket item once LTDC's neighbors stop faulting, since retro-go/
chainloader firmware uses DMA2D + LTDC together for real rendering (our
LTDC stub can already display *something*, but nothing produces
interesting pixel content without DMA2D).

extflash (needed for retro-go, not for gnw-chainloader) still isn't
populated — the user will provide a real image later for that test
path specifically.

## Known constraints

- No STM32H7B0 machine exists upstream; everything here is new.
- Keep `../minicraft-gnw`'s MPS2 fault-trap QEMU harness as the working
  regression baseline until this fork's model is proven equivalent.
- RM0455 is known-wrong about internal flash size on real H7B0 — see
  `docs/h7b0-flash-discrepancy.md` before trusting it on flash topics.
