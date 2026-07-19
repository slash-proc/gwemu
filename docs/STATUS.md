# Status

Last updated: 2026-07-19

Fork of upstream QEMU (`qemu/qemu`), pinned to tag `v11.0.2`. Working
branch `gnw-h7b0`.

## Where things stand

Real STM32H7B0 machine model, not a fault-trap workaround. Both homebrew
(retro-go, SD-backed) and official Nintendo stock/CFW firmware (Mario,
Zelda) boot end-to-end to a fully interactive, playable state: display,
audio, gamepad input, SD card, save/flash persistence.

## What works

- Core CPU, memory map, NVIC, real clock tree (HSI/HSE/PLL1-3, live-derived
  SYSCLK/HCLK/LTDC-pixel-clock — see `docs/h7b0-clock-tree-findings.md`),
  dynamic CPU-overclock support.
- SPI/OSPI flash (dual-bank, see `docs/h7b0-flash-discrepancy.md`), SD card
  (SPI-based), RTC, DMA, TIM1/TIM2/LPTIM1, ADC, PWR, CRC, CRYP
  (AES-ECB/CBC/CTR/GCM/CCM), OTFDEC (real AES-128-CTR extflash decryption),
  HASH, FLASH_R, TAMP, and the rest of the boot-path peripheral set.
- LTDC: real per-layer compositing (correct layer order, pixel formats,
  color key, window-clip, blend, dithering, per-layer CLUTs, real
  reload/vblank timing).
- DMA2D: real per-pixel fetch for every format firmware uses, real
  blend/fixed-color modes.
- JPEG: real polled output-register pipeline with correct chroma
  subsampling.
- Audio: confirmed solid across every core tested.

## Known issues (open)

- QEMU/TCG instruction-interpretation overhead means gameplay is not
  perfectly real-time-matched to real hardware in every scenario; most of
  the addressable overhead (per-pixel MMIO translation calls) has already
  been batched away. Remaining gap is generic TCG cost, not a device-model
  bug.
- Real subsampled chroma storage was traded for full-resolution internal
  storage in the JPEG model (a documented scope decision, not a bug).

## Now / next

- Repo cleanup pass: pruning historical investigation docs (their findings
  now live as code comments and in CHANGELOG.md, not as standalone
  narrative), auditing scripts for portability, automating first-run setup
  (SD card image, etc.) for new users.
- Next up after cleanup: a usable GUI (in the spirit of `xemu`), then CI +
  cross-platform (Linux/Mac/Windows) release builds.

## Tooling notes worth keeping in mind

- QEMU's gdbstub halts the whole VM (including peripheral input-event
  delivery) on client *connect*, and for any command besides memory
  read/write. Reading peripheral state right after connecting can show
  stale pre-input values.
- `scripts/gdb_tap.py` (a logging GDB-RSP proxy) is the fastest way to see
  what a real client is actually sending when debugging an integration
  that talks to this QEMU over GDB RSP.
- Temporary `fprintf(stderr, ...)` debug prints added directly to device
  model source (rebuilt via `ninja qemu-system-arm`) are the fastest way to
  see internal QEMU device state that guest-side gdb reads can't reach at
  all — always remove them again once their diagnostic purpose is served.
