# Session state — 2026-07-10, part 2 (resumed after context compaction)

Continuation of `docs/session-2026-07-10-state.md`. That doc's Open
problems 1/2/3 and extflash section are all superseded by findings
below where they overlap — read this one first, it has the current
picture; fall back to part 1 only for gdb gotchas and untouched detail
(DMA2D/LTDC history, roadmap context).

## What changed this session (all committed to `hw/`/`include/hw/`,
not yet git-committed as of this doc — commit before ending the
session if not already done)

1. **`RCC_RSR_RESET_VALUE` PORRSTF fix** (`include/hw/misc/gnw_h7b0_rcc.h`):
   cleared `PORRSTF`, kept `CDRSTF|BORRSTF|PINRSTF`. Root-caused a
   permanent boot-standby trap in `gnw-chainloader`'s `stub_main.c`
   that made every QEMU boot look like a genuine cold power-on. See
   STATUS.md for full detail.

2. **Real virtual SD card on SPI1** (`hw/misc/gnw_h7b0_spi.{c,h}`):
   new `sd-card` bool qdev property; when true, SPI1 becomes a real
   `hw/ssi/ssi.h` bus controller feeding `ssi_transfer()` on every
   TXDR write to an attached `ssi-sd` + `sd-card-spi` device pair
   (upstream `hw/sd/ssi-sd.c` + `hw/sd/sd.c`). `gnw_h7b0_soc.c` sets
   this property on the SPI1 instance only (SPI2 unaffected, still
   the plain LCD-command stub). `hw/arm/Kconfig`'s `GNW_H7B0_SOC` now
   `select`s `SSI_SD`. Test image: `mkfs.vfat -F 32 -n GNWSD
   /tmp/gnw-sd-test.img` (64M), attach via
   `-drive if=sd,format=raw,file=/tmp/gnw-sd-test.img`.

3. **New RTC device** (`hw/misc/gnw_h7b0_rtc.{c,h}`, `0x58004000`):
   replaces the old plain-RAM placeholder. `ICSR` writes instantly
   mirror `INITF|RSF|INITS` set. **This was retro-go's actual crash
   root cause**, previously misattributed to extflash content and
   thought to need retro-go source — see below.

4. **DAC1 (`0x40007400`), DAC2 (`0x58003400`), TIM1 (`0x40010000`)**:
   added as plain-RAM placeholders in `gnw_h7b0_soc.{c,h}` — all three
   were previously *entirely unmapped* (not even a placeholder),
   unlike every other not-yet-modeled peripheral in the SoC.

## The retro-go crash mystery is resolved (mostly)

`docs/session-2026-07-10-state.md`'s "Open problem 3" (retro-go's
`Error_Handler`/BSOD crash, `PC=0x00000000 LR=0x08005915` in the old
`gw_retro_go_bank1.elf` build) was never actually about extflash
content, and did not need retro-go source to diagnose -- it needed a
matching source tree, which we already had without realizing it:
**`/home/doug/Nerd/git/gnw-chainloader/retro-go-sd/` is the real
source for `retro-go-temp/elf/gw_retro_go_sd_bank1.elf`** (confirmed
by gdb resolving real file/line info against it, e.g.
`Core/Src/main.c`, `Core/Src/gw_flash.c`). Use the SD-bank1 build +
this source tree for any further retro-go debugging, not the old
no-source `gw_retro_go_bank1.elf`.

The crash chain, once traceable:
- `main()` → `MX_RTC_Init()` → `HAL_RTC_Init()` times out (RTC was a
  dumb RAM stub, `INITF`/`RSF` never read back set) → `Error_Handler()`
  → jumps through a null pointer. **Fixed** by the new RTC device.
- Next: `MX_TIM1_Init()` → `HAL_TIM_Base_Init()` → real HardFault
  (TIM1 entirely unmapped) → this one cascaded into a **QEMU fatal
  `Lockup: can't escalate 3 to HardFault` abort** rather than a clean
  single fault, because a *second* fault (the DAC1 access right after)
  happened while still inside the first fault's handler at elevated
  priority, which the ARMv7-M architecture (correctly) treats as
  unrecoverable. This was much harder to read than a normal
  `crash_log`-recorded fault — no crash context gets written before a
  lockup abort, so diagnosis was purely from the QEMU fatal-exit
  register dump (`R01`/`R02` held the faulting address) +
  cross-referencing `sdk/cmsis-device-h7/Include/stm32h7b0xx.h`'s
  `_BASE` defines to identify what `0x40007410` / `0x58003410` were.
  **Fixed** by mapping DAC1 + DAC2 as plain RAM.
- Next: TIM1 unmapped, same HardFault pattern, this time caught
  cleanly (real BSOD screen, not a lockup) since it was an isolated
  single fault. **Fixed** by mapping TIM1 as plain RAM.
- **Current stopping point**: `main()` → `OSPI_Init()` (`gw_flash.c`)
  → `assert(!"Can't communicate with the external flash!")` fires
  because our OSPI device never implements real `RDID` command
  handling (JEDEC ID always reads back 0x000000, which the driver
  correctly treats as "no flash present"). This is a real `assert()`
  in firmware logic, not a HardFault/lockup — cleanly diagnosable.
  **Not yet fixed** — this is the next task (see below).

## Next task: real OCTOSPI command decoding

`hw/misc/gnw_h7b0_ospi.c` currently only mirrors `CCR`/`IR` writes
into `SR.TCF` instantly (see its own file comment) -- no real
command/address/data-phase transfer is modeled. This was sufficient
for `gnw-chainloader` (which never validates the JEDEC ID, just needs
`HAL_OSPI_Command()` to not time out) but not for retro-go's
`gw_flash.c`, which explicitly reads back and validates the ID via
`OSPI_ReadBytes(CMD(RDID), 0, &flash.jedec_id.u8[0], 3)` and asserts
if it's `0xFFFFFF` or `0x000000`.

Needed: decode at least the `RDID` command in `gnw_h7b0_ospi.c` (or a
wrapper) and return a real, valid JEDEC ID matching some entry in
`gw_flash.c`'s `jedec_map[]` table (check
`gnw-chainloader/retro-go-sd/Core/Src/gw_flash.c` for that table's
contents — need to fabricate a plausible real flash chip ID the
firmware's map recognizes, not an arbitrary value). Ideally also wire
memory-mapped reads (already working via `-device loader`) and the
command-protocol path together so they're consistent (same backing
storage), rather than parallel/independent fakery.

Also check: `OSPI_ReadBytes(CMD(RDSR), 0, &status, 1)` (status
register read) right after RDID — check what bits `gw_flash.c` expects
there for a healthy/idle chip (probably a busy/WIP bit that must read
0), or that'll be next up after RDID.

## Loose ends / things to keep in mind

- `example-extflash-backup.bin` (loaded via `-device loader` at
  `0x90000000`) has never been confirmed to contain a *valid* extflash
  image the firmware would recognize once RDID/RDSR are fixed -- once
  the OSPI command path is real, may need to separately verify this
  file's content is sane (or get a known-good one) rather than
  assuming the existing file was always fine.
- `/tmp/gnw-sd-test.img` is a scratch file (not in the repo,
  `/tmp/claude-*` scratchpad is a different path -- this one is
  directly in `/tmp`), an empty `mkfs.vfat` FAT32 image with no real
  ROM content. Fine for protocol-level testing; not useful for
  actually browsing/loading a game until populated with real files.
- The `sd-card` SPI1 property change measurably increases real MMIO
  traffic (thousands of `SPI1->CR2` writes per boot once a card
  responds) -- factor this in before assuming any future "boot still
  feels slow" report is a regression; some of that is now-expected
  real block-I/O cost that didn't exist before the card worked.
- Multiple `qemu-system-arm`/`gdb-multiarch` background processes were
  started and killed throughout this session testing different
  binaries/fixes -- before starting fresh, `pkill -f "qemu-system-arm
  -M gnw-h7b0"` and confirm nothing stale from a previous test is
  still holding port 1234 (gdbserver default) before assuming a new
  launch's gdb connection is talking to the process you think it is.
- Enabling `-d guest_errors,unimp -D /tmp/qemu-guest-errors.log` was
  very useful this session for auditing unimplemented-register
  coverage -- consider making this a documented standard debugging
  step (it was never used earlier in Phase 1's whack-a-mole, which
  relied purely on gdb PC-sampling instead).
