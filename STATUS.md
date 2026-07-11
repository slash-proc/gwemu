# Status

Last updated: 2026-07-10

## Where things stand

Repo is a fork of upstream QEMU (`qemu/qemu`), pinned to tag `v9.2.4`.
Working branch `gnw-h7b0` (based on that tag), pushed to `origin` as of
Phase 0 (not re-pushed since; push only on explicit go-ahead). **Phase 0
and the memory-map portion of Phase 1 are done**: `gnw-h7b0` boots a bare
Cortex-M7 against the real STM32H7B0 SRAM/flash bank layout.

## What works

- Core CPU, memory map, NVIC, basic timers.
- SPI Flash (read-only memory-mapped).
- SD card reading via SPI stub.
- LCD display (LTDC) with accurate 60Hz frame pacing.
- DMA2D device model with basic compositing and alpha blending.
- Gamepad input.
- Hardware JPEG decoding for cover art in cover flow.

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

- [x] **RCC_RSR PORRSTF boot-standby trap fixed**: `RCC_RSR_RESET_VALUE`
  (`gnw_h7b0_rcc.h`) had `PORRSTF` set as part of a realistic
  all-four-flags power-on value (`0x00E80000`) — correct for real
  hardware's *first ever* power-on, but since QEMU resets to this same
  value on *every* launch, `gnw-chainloader`'s
  `stub_main.c`'s `if (RCC->RSR & RCC_RSR_PORRSTF)` branch (a
  deliberate real-hardware anti-auto-boot-on-USB-plug-in trap that
  parks the CPU in `SLEEPDEEP`+`WFI` forever waiting for a physical
  WKUP1 button we don't model) fired on every single emulated boot.
  Fixed by clearing just `PORRSTF` (now `0x00680000`, keeping
  `CDRSTF|BORRSTF|PINRSTF` — `PINRSTF` is a more accurate stand-in for
  "board was just reset/relaunched", which is what every QEMU boot
  actually is). Confirmed: PC now reaches the decompressed app in AXI
  SRAM and `g_scan.state` progresses through real states, instead of
  never leaving the flash stub.

- [x] **Real virtual SD card on SPI1** (`hw/misc/gnw_h7b0_spi.c`,
  `sd-card` qdev property, set `true` only for the SPI1 instance in
  `gnw_h7b0_soc.c`): SPI1 is now a real SSI bus controller — each
  TXDR write is forwarded via `ssi_transfer()` to an attached
  `hw/sd/ssi-sd.c` + `hw/sd/sd.c` virtual SD card (upstream's real
  SD-over-SPI protocol model, validated against real Linux/U-Boot
  drivers), backed by whatever `-drive if=sd,format=raw,file=...` the
  user passes. No chip-select GPIO wired up (real firmware toggles CS
  via a plain GPIO pin, not SPI1 NSS) — relies on `ssi-sd`'s default
  always-selected behavior, safe here since it's a single-device
  dedicated bus. `hw/arm/Kconfig` needs `select SSI_SD` under
  `GNW_H7B0_SOC`. With no `-drive` passed, behaves as "ejected" (same
  fast no-card signal as before, now via real protocol bytes). With a
  real FAT-formatted image attached (`mkfs.vfat`), SD detection
  proceeds through real block I/O — confirmed via `SPI1->CR2` write
  counts in the thousands per boot (real per-byte SD transfers), a
  new, previously-nonexistent cost worth knowing about when judging
  boot-time budgets.

- [x] **RTC device implemented, found via retro-go's actual crash
  root cause** (`hw/misc/gnw_h7b0_rtc.c`, `0x58004000`): RTC was a
  zero-initialized plain-RAM placeholder through all of Phase 1, so
  `ICSR.INITF`/`ICSR.RSF` never read back set, and
  `HAL_RTC_Init()` → `RTC_EnterInitMode()`/`HAL_RTC_WaitForSynchro()`
  always timed out. This is the real root cause of retro-go's
  long-standing `Error_Handler`/BSOD crash (previously mis-attributed
  to extflash content, and previously thought to need retro-go source
  to diagnose) — traced precisely once `gnw-chainloader/retro-go-sd/`
  (a source tree that turned out to match the `gw_retro_go_sd_bank1.elf`
  build byte-for-byte) let `addr2line`/gdb resolve real function names
  and line numbers (`Core/Src/main.c`'s `MX_RTC_Init()`). Fixed with
  the same instant ON→RDY-style approximation as RCC/PWR/OSPI/ADC:
  every `ICSR` write mirrors back `INITF|RSF|INITS` set immediately.

- [x] **Two entirely-unmapped peripherals found and given
  placeholders**: `DAC1` (`0x40007400`) and `DAC2` (`0x58003400`,
  SRD-domain, a separate instance) were never mapped *at all* (not
  even a plain-RAM placeholder, unlike every other not-yet-modeled
  peripheral) — retro-go's audio init touches them, `gnw-chainloader`
  never does, so this gap was invisible until now. Found the hard way:
  a genuinely unmapped MMIO access mid-fault-handler produced a QEMU
  **`Lockup: can't escalate ... to HardFault`** fatal abort (a second,
  unrecoverable fault inside the first fault's handler) instead of a
  clean single fault — much harder to diagnose than `crash_log`'s
  usual clean halt-with-recorded-context. Both now plain-RAM
  placeholders, same treatment as GPIO/CRS/FMC/etc.

- [x] **TIM1 (`0x40010000`) also entirely unmapped** — same story:
  the whole timer peripheral class had zero coverage; retro-go's
  `MX_TIM1_Init()` (likely piezo-speaker PWM) is the only timer it
  touches. Now a plain-RAM placeholder.

- [x] **Real OCTOSPI indirect-mode command decoding**
  (`hw/misc/gnw_h7b0_ospi.c`): previously only mirrored `CCR`/`IR`/`AR`
  writes into `SR.TCF` instantly with no real command/data-phase
  transfer, so `RDID` (read JEDEC ID) always read back zero and
  `OSPI_Init()` (`../gnw-chainloader/retro-go-sd/Core/Src/gw_flash.c`)
  hit its `assert(!"Can't communicate with the external flash!")`.
  Now decodes `RDID`/`RDSR`/`RDCR`/`SFDP`/`WREN`/`RSTEN`/`RST` as a
  synthetic Macronix MX25U51245G (64MB/512Mbit, JEDEC `C2 25 3A`) NOR
  flash, matching `EXTFLASH_SIZE`'s existing 64M placeholder exactly
  so SFDP-reported density and the real backing region size agree.
  `RDSR` hardwires the QE bit set so `gw_flash.c`'s MX/ISSI quad-mode
  init path (`init_mx_issi()`) skips its WRSR round-trip entirely.
  Implementation: `IR`/`AR` writes latch the instruction/address and
  set `SR.TCF|SR.FTF` instantly (satisfying both `HAL_OSPI_Command`'s
  own completion poll and each subsequent per-byte `SR.FT` poll around
  `DR` access in `HAL_OSPI_Receive`/`Transmit`); `DR` reads walk a
  per-command byte generator, `DR` writes are accepted but dropped (no
  backing store yet for program/erase — those complete instantly as
  no-ops). **Verified**: booting `gw_retro_go_sd_bank1.elf`
  (`retro-go-temp/elf/`, gitignored/user-provided) with extflash
  populated via `-device loader,file=example-extflash-backup.bin,
  addr=0x90000000` now gets **well past** `OSPI_Init()` with zero
  OSPI-related guest-error log lines, progressing all the way to
  `MX_WWDG1_Init()` in `main()` — a new, later, unrelated gap (the
  watchdog peripheral at `0x50003000` is entirely unmapped, same
  "found the hard way via a QEMU Lockup" pattern as DAC1/DAC2/TIM1
  before it).

- [x] **Real hardware CRC-32 unit** (`hw/misc/gnw_h7b0_crc.c`,
  `0x40023000`): found immediately after the OSPI fix above let
  `gnw-chainloader` boot reach `ofw_crc32()`
  (`src/chainloader/system/ofw_verify.c`), which uses the CRC
  peripheral (not software) to checksum OSPI flash content against
  baked-in expected values before ever trusting it. First gap: CRC was
  entirely unmapped (BusFault). Fixed that with a plain-RAM
  placeholder, which un-blocked boot but broke verification silently
  instead — a dumb stub always reads `DR` back as 0, so every checksum
  compare failed and `gnw-chainloader` visibly rendered a black/plain
  screen instead of its real UI (all "PREPARING"/menu-drawing code
  paths gated on a passing verify never ran). Replaced with a real
  bit-serial CRC engine matching the documented STM32 hardware
  algorithm exactly (configurable poly/init via `POL`/`INIT`,
  `CR.POLYSIZE`/`REV_IN`/`REV_OUT` all honored, not just the 32-bit/
  no-reversal path `gw_flash.c` happens to use). **Verified**: booting
  `gnw_chainloader.bin` now renders a real "FLASHING... Writing
  128KB..." progress UI (header bar, progress bar, status text) where
  it previously showed a blank/solid-color screen.
- [x] **TAMP (`0x58004400`) also entirely unmapped** — found the same
  way as CRC, one gap further into boot once CRC stopped blocking it.
  Adjacent to but distinct from RTC (RTC's own device only covers
  `0x58004000`-`0x580043FF`). Plain-RAM placeholder, same treatment as
  GPIO/CRS/FMC/etc.
- [x] **GPIO IDR false-"button held" bug, found by the user noticing
  gnw-chainloader jumping straight to the OFW-flashing screen
  unprompted** (`hw/misc/gnw_h7b0_gpio.c`, `0x58020000`-`0x58022FFF`):
  GPIO had been a plain zero-initialized RAM placeholder since Phase
  1. Real G&W buttons are active-low with external pull-ups
  (`board_check_button()`: `(port->IDR & pin) == 0` means pressed), so
  an all-zero `IDR` reads as *every button permanently held* —
  including Left/Right, which silently forced
  `app_early_logic()`'s "God Mode" boot-time override
  (`hold_left || hold_right`) straight into `partition_flash_ofw()` on
  every single boot. Not a QEMU input event, not a real press — just
  an unpressed button reading as pressed. Fixed with a real (still
  otherwise-minimal) GPIO device whose `IDR` resets to `0xFFFF` per
  port (11 ports, A-K) instead of 0; everything else is still a plain
  read/write shadow, no real pin-mux/output-loopback semantics yet.
  **Verified**: `gnw_chainloader.bin` now boots to its actual main
  menu (`GNW CHAINLOADER` header, no auto-flash) instead of the God
  Mode path.
  - **Follow-on fixed in the 2026-07-10 part-5 session**: the main menu
    used to flicker between fully-drawn and solid-red/blank frames when
    screenshotted repeatedly. Real root cause turned out to be
    `SRCR.VBR` clearing instantly instead of gating on the real vblank
    (see "Next up" below) — not just a missing vblank/IRQ model in
    general (that part was already added, see below).

- [x] **WWDG (`0x50003000`) plain-RAM placeholder** — was entirely
  unmapped (Lockup-abort pattern, same as DAC1/DAC2/TIM1 before it).
  Found via `patched-zelda-bank1.bin` (real OEM firmware) boot
  testing. No real countdown/refresh semantics yet.
- [x] **TIM2/3/4/5/6/7/12/13/14 (`0x40000000`-`0x400027FF`) plain-RAM
  placeholder** — TIM5 found entirely unmapped the same way, via the
  same OEM-firmware boot; mapped the whole contiguous block up front
  rather than one timer at a time.
- [x] **Real LTDC vblank/line-interrupt** (`hw/display/gnw_h7b0_ltdc.c`,
  `hw/arm/gnw_h7b0_soc.c` wiring `LTDC_IRQn`=88 to the NVIC): `IER`/
  `ISR`/`ICR` are now real (not shadow) registers, driven by a plain
  ~60Hz `QEMUTimer` (not derived from real pixel-clock/AWCR/TWCR
  timing) that sets `ISR.LIF|RRIF` and raises the IRQ when
  `IER.LIE`/`RRIE` are set. Root cause this fixed: both
  `gnw-chainloader`'s `gui_refresh()` and retro-go-sd's
  `lcd_wait_for_vblank()` (`Core/Src/gw_lcd.c`) `__WFI()`-busy-wait on
  a frame counter only incremented by the real Line Interrupt
  handler — which never fired without a real IRQ. For chainloader this
  was "just" the reported menu flicker (grabbing an in-progress-redraw
  frame because the wait never actually waited on anything real); for
  retro-go-sd it was a hard, permanent hang in `_draw_logos()`.
  **Verified**: retro-go-sd's boot-logo animation
  (`gw_retro_go_sd_bank1.elf`) now completes instead of hanging.
  **Follow-on fixed in the 2026-07-10 part-5 session**: chainloader's
  menu used to visibly tear on rapid re-screenshot because `SRCR`
  (shadow-reload register) writes were treated as instantaneous, so
  `fb1`/`fb2` double-buffer swaps happened immediately rather than
  deferred to the next real vblank. Fixed: `SRCR.VBR` now stays set
  until the real vblank tick, matching real hardware and (it turned
  out) also fixing firmware's own frame-pacing, since `gui_refresh()`
  busy-waits on that same bit as its actual throttle — see "Next up".
- [x] **SPI `SR.TXP` incorrectly gated on `CR1.SPE`**
  (`hw/misc/gnw_h7b0_spi.c`): found via retro-go-sd's bare-metal
  `user_diskio_spi.c` (`SPI_TxByte`/`SD_PowerOn`) hanging forever
  polling `SPI1->SR.TXP` — a real (non-fault, confirmed via two
  identical gdb snapshots) infinite loop, root-caused by a delegated
  investigation (see below). `TXP` ("Tx-Packet space available")
  reflects TxFIFO occupancy on real hardware, which is trivially
  available even with the peripheral disabled — it is **not** gated on
  `SPE`. Our model previously only set `TXP` as a side effect of a
  `CR1` write with `SPE` set, which happened to work for
  `gnw-chainloader`'s SPI1 "Tim" SD-card pattern (which sets `SPE`
  immediately before every byte and only ever polls `EOT`, never
  `TXP`) but broke retro-go-sd's HAL-based driver, which polls `TXP`
  *before* `HAL_SPI_Transmit()` — the actual `SPE`-enabling call —
  ever runs. Fixed: `TXP` is now set once at reset and left alone;
  only `EOT` is still cleared on `SPE`-disable. **Verified**: booting
  `gw_retro_go_sd_bank1.elf` now gets past `sdcard_init()` entirely and
  renders retro-go's own real "No SD CARD found" UI (frowny-face
  graphic, version string, full text) — genuinely complete boot to
  application UI, not just past a hang.
- [x] **GPIO false "button held" root-caused a `gnw-chainloader` boot
  jumping straight to an OFW-flashing screen** — see the GPIO entry
  above; caught live by the user noticing unprompted "FLASHING..." UI,
  traced to `IDR` defaulting to 0 (every active-low button reading
  permanently pressed) rather than a real input event.
- [x] **Real OEM firmware boot-tested for the first time**
  (`patched-zelda-bank1.bin`, real Zelda OFW + a Marian-Muller-style
  patch, combined with `retro-go-real-bank2.bin` as Bank2 and the
  existing extflash backup): found the WWDG and TIM2-block gaps above.
  Also surfaced, then resolved by the WWDG fix, a scary-looking fault
  cascade (`PC=0x0801ad68`, `LR=0xffffffe9` -- an `EXC_RETURN` value
  being dereferenced as data, with a long tail of `Invalid read at
  0xFFFFFFEx` log lines) that turned out to be a downstream symptom of
  the WWDG BusFault-turned-Lockup, not a separate bug -- gone entirely
  once WWDG was mapped. Real OEM firmware is closed-source (no
  `addr2line`), so treat any further faults there as much more
  expensive to root-cause than retro-go-sd/chainloader; prefer
  reproducing on those two when possible.

- [x] **Keyboard/pointer button input** (`hw/misc/gnw_h7b0_gpio.c`):
  registers a real `QemuInputHandler` (`INPUT_EVENT_MASK_KEY |
  INPUT_EVENT_MASK_BTN`) that clears/sets the correct port's `IDR` bit
  on press/release, per `board_check_button()`'s `(port->IDR & pin) ==
  0` == pressed polarity and `gnw-chainloader`'s `board.c` pin table
  (buttons are spread across GPIOA/C/D, not all on one port). Keyboard
  mapping: arrows for d-pad, X/Z for A/B (Z=B/X=A emulator
  convention), Enter/Right-Shift for Start/Select, Escape/G/T/P for
  Pause/Game/Time/Power. **Verified end-to-end**: booted
  `gnw_chainloader.bin`, sent `sendkey down` over the HMP monitor,
  confirmed via screenshot the on-screen `>` cursor actually moved
  from LAUNCH to TOOLS.
  - **Real xpad/gamepad support hit a genuine version limitation, not
    a design gap**: this project's pinned QEMU base (v9.2.4) has no
    SDL2 game-controller/joystick backend at all — `InputButton`
    (`qapi/ui.json`) is a mouse-button enum only (left/middle/right/
    wheel-\*/side/extra), with no code path by which a real
    controller's face buttons reach any QEMU device in this tree. The
    `INPUT_EVENT_MASK_BTN` path is wired and mapped (mouse
    left/right→A/B, middle/side/extra→Start/Select/Pause, wheel
    directions→d-pad) so it's structurally ready, but is unverifiable
    with an actual xpad controller until this QEMU base gains a real
    joystick backend — a bigger, separate lift (either bumping the
    pinned upstream version deliberately per CLAUDE.md's rules, or
    backporting SDL2 joystick support) than this session's scope.

## Next up

Real xpad/gamepad support landed (`ui/sdl2-gamepad.c`, d-pad + face
buttons via SDL2's controller API, no analog stick yet) and a real
DMA2D device model landed (`hw/display/gnw_h7b0_dma2d.c`) -- see
`docs/session-2026-07-10-part3-gamepad-sd-debugging.md` for the full
writeup, including the disassembly-only gdb technique used to find and
fix five separate "firmware polls a status flag our stub never sets"
hangs (SPI1 RXP, `ssi-sd.c` CMD58, JPEG EOCF/IFTF/IFNFF, TIM2-block
UIF, RTC ICSR write-flags).

**Flicker + game-logic speed, from the previous session's starting
point, are now fixed** -- turned out to be two separate real bugs, not
one. See `docs/session-2026-07-10-part5-flicker-speed-dma-battery.md`
for the full narrative:
- The real flicker root cause was `hw/display/gnw_h7b0_ltdc.c`'s
  `SRCR.VBR` bit clearing instantly on write instead of staying set
  until the real vblank tick -- that's real firmware's *actual*
  frame-rate throttle (`gui_refresh()`/`gui_redraw()` busy-wait on it),
  confirmed by reading `gnw-chainloader`'s and
  `game-and-watch-retro-go-sd`'s real source (both checked out locally
  at `../gnw-chainloader` / `../game-and-watch-retro-go-sd` -- consult
  them directly instead of guessing from disassembly when possible).
- The real "runs too fast" root cause was `hw/misc/gnw_h7b0_dma.c`
  having zero completion/interrupt logic at all (not wired to the NVIC
  either) -- retro-go paces game speed off audio DMA completion.

**Fully resolved**: the **pause/options-overlay-specific flicker**. The root cause was discovered to be QEMU's asynchronous UI refresh timer (`dpy_gfx_update`) sampling the guest's RAM directly. The guest uses DMA2D to draw overlays into the active frontbuffer immediately after a `VBR` buffer flip, and QEMU occasionally "caught" the guest mid-draw, causing the overlays to flicker out of existence. Fixed by introducing an internal `shadow_buffer` in `hw/display/gnw_h7b0_ltdc.c` that captures the fully composited frame at the exact end of its 16ms window (the instant before the next `VBLANK`), decoupling the host refresh from the guest rendering completely.

Also still open: a patched Zelda firmware image (no debug symbols)
still hangs mid-boot past ADC config -- see
`docs/session-2026-07-10-part3-gamepad-sd-debugging.md` for exactly how
far it gets and what's untried. Deprioritized in favor of the generic
retro-go SD build, which boots much further already.

Battery now reads 100% in both firmwares (was always 0%) -- see the
session-5 doc; ADC1 channel 4 (no I2C fuel gauge) now returns a
full-battery raw value with a real completion IRQ.

## Known constraints

- No STM32H7B0 machine exists upstream; everything here is new.
- Keep `../minicraft-gnw`'s MPS2 fault-trap QEMU harness as the working
  regression baseline until this fork's model is proven equivalent.
- RM0455 is known-wrong about internal flash size on real H7B0 — see
  `docs/h7b0-flash-discrepancy.md` before trusting it on flash topics.
