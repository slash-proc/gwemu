# Changelog

## 2026-07-11 (part 4 — bump pinned base v9.2.4 -> v11.0.2)

- **Merged upstream `v11.0.2`** (previously pinned to `v9.2.4`), via
  `git merge` rather than rebase to preserve existing history. Nearly all
  conflicts were upstream refactors in files this fork never touches
  (target/arm, migration, etc.) and were taken wholesale from upstream;
  the only real conflicts were in the 4 files we'd actually modified
  (`hw/arm/Kconfig`, `hw/arm/meson.build`, `hw/sd/ssi-sd.c`, `ui/sdl2.c`).
  Notably, upstream's SPI-mode response refactor in `hw/sd/sd.c`
  (idle-state bit now derived from real SD card state) superseded our
  earlier CMD58 idle-bit heuristic patch in `ssi-sd.c`, which was
  dropped in favor of the upstream fix.
- Ported all `gnw_h7b0_*` device models to v11.0.2's internal API churn:
  several `hw/*.h` headers moved under `hw/core/`, `exec/memory.h` ->
  `system/memory.h`, `ObjectClass.class_init`'s `data` param is now
  `const void *`, `DEFINE_PROP_END_OF_LIST()` sentinel removed from
  `Property` arrays, and the audio backend API was rewritten
  (`QEMUSoundCard`/`AUD_*` -> `AudioBackend *`/`audio_be_*` in
  `gnw_h7b0_sai1.c`). Also had to switch `gnw_h7b0.c`'s machine
  registration from `DEFINE_MACHINE()` to `DEFINE_MACHINE_ARM()` since
  `hw/arm/meson.build` moved most boards (including ours) into a new
  shared `arm_common_ss` source set that requires the ARM target-info
  interface to actually show up in `-M help`.
- Verified `qemu-system-arm` builds clean and boots
  `gw_retro_go_bank1.elf` normally under gdb (PC and `uwTick`
  progressing steadily across repeated samples, not just process-alive).

## 2026-07-11 (part 3 — VBR-gated capture timing, JPEG OFTF perf fix)

- **Gated LTDC framebuffer capture on firmware's `SRCR.VBR` write** instead
  of an independent fixed vblank timer (`hw/display/gnw_h7b0_ltdc.c`,
  developed in a separate concurrent session, reviewed and merged here).
  The old unconditional per-tick capture could sample the framebuffer while
  the emulated CPU was still mid-draw on a variable-cost frame (e.g. a
  JPEG-heavy coverflow redraw), a plausible root cause for the "flip back
  then snap" tearing symptom distinct from anything found earlier this
  session. Capture is gated on `!s->content_dirty` so an unconsumed capture
  isn't overwritten before the UI thread picks it up. **Not yet confirmed
  live whether this resolves the flicker** — known gap: content that never
  writes `SRCR.VBR` (single-buffered/IMR-only paths) no longer gets
  captured at all.
- **Fixed a real JPEG decode performance bug**: the model never set `OFTF`
  (output FIFO threshold), so firmware's polling loop (`JPEG_Process`) was
  always forced into the one-word-at-a-time `OFNEF` path instead of the
  real 8-words-per-check bulk path, multiplying the number of separate
  SR-flag-check MMIO round trips per decode by ~8x. `perf` showed this as
  the dominant cost (>16% of total CPU) during a menu scroll-loop
  workload; setting `OFTF` correctly (mirroring real hardware's FIFO
  watermark) cut that specific overhead to ~11% with zero change to
  decoded output (total DOR word-reads are identical either way — real
  hardware's own `JPEG_StoreOutputData` still reads one word at a time
  internally regardless of threshold, so this only reduces polling-loop
  *iteration* overhead, not real data volume). Ruled out GPIO/input
  reading as a contributor via the same profile — it doesn't appear in the
  hot path at all.

## 2026-07-11 (part 2 — real DMA2D/JPEG YCbCr blend pipeline, flicker still open)

Follow-on to the same-day LTDC/JPEG work below. Full writeup:
`docs/session-2026-07-11-dma2d-jpeg-ycbcr-pipeline.md` (and its predecessor
`docs/session-2026-07-11-ltdc-flicker-investigation.md`).

- **Fixed LTDC `RRIF` unconditional-assert bug** and **generalized LTDC's
  per-layer compositing** (color key, window-clip/default-color, generalized
  `BF1`/`BF2` blend formula, Bayer dithering, Layer2 CLUT/window-clip gaps) —
  both real, both tested, **neither fixed the reported coverflow flicker**.
- **Replaced the JPEG "hack buffer"** (a raw RGB565 QEMU-heap pointer handed
  directly to DMA2D, superseding this file's earlier 2026-07-11 JPEG entry
  below, which turned out to bypass `FGMAR`/`FGOR`/chroma-subsampling
  entirely) **with a real polled `DOR` register** that firmware's own
  `HAL_JPEG_Decode`/`JPEG_Process` polling loop drains into guest RAM,
  exactly like real polling-mode JPEG decode (no DMA involved) — decode
  stays fully synchronous, only the delivery mechanism changed.
- **Fixed `DMA2D_CR_MODE_MASK`** from 2 bits to the real 3-bit width —
  firmware's real cover-art blend mode (`M2M_BLEND_BG` = 5) was silently
  truncating to `M2M_PFC` (1), meaning the real blend firmware performs
  never actually happened in emulation before this.
- **Implemented real `BLEND_BG`/`BLEND_FG` "fixed color" semantics** (one
  side is a constant `FGCOLR`/`BGCOLR`, not a fetched buffer — confirmed via
  the real HAL header) and added a real `A8` input-format fetch. An initial
  pass that treated `BLEND_BG` like a two-buffer blend (fetching a nonexistent
  `BGMAR` buffer) was a real regression, caught via live testing, fixed same
  session.
- **Fixed a chroma-subsampling storage-size mismatch**: serving full-
  resolution (non-subsampled) Cb/Cr made our JPEG `DOR` output ~2x the size
  firmware's own destination buffer expects for real (subsampled) hardware
  output, so firmware's polling loop only partially drained it — visible as
  banded/static corruption on cover art. Fixed by subsampling Cb/Cr per the
  image's real hand-parsed SOF0 sampling factors.
- **Fixed an out-of-bounds row-wrap** in the new YCbCr fetch (reading past a
  decoded image's real width using the DMA2D transfer's larger configured
  geometry wrapped into the next row's bytes).
- Both corruption fixes are confirmed live-fixed by the user. **The original
  coverflow/menu flicker itself is still unresolved** — decode, compositing,
  and the final CPU blit into the LCD framebuffer are all now confirmed
  deterministic, ruling out this pipeline as the remaining cause. Next
  suspect: LTDC scanout/vblank timing relative to firmware's real swap
  cadence, not yet live-traced.

## 2026-07-11

- **Fixed the pause/options-overlay flicker (screen tearing)**: The root cause was QEMU's asynchronous UI refresh timer occasionally reading the active frontbuffer mid-draw. The guest uses DMA2D to draw overlays directly into the frontbuffer immediately after a `VBR` buffer flip. Fixed by introducing an internal `shadow_buffer` in the LTDC module that captures the fully composited guest frame at the very end of the 16ms window (the instant before the next `VBLANK`), decoupling QEMU's UI thread from the guest rendering entirely.
- **Implemented accurate LTDC frame pacing**: Replaced `QEMU_CLOCK_VIRTUAL` with `QEMU_CLOCK_HOST` for `vblank_timer` and `line_timer` to prevent `__WFI()` from fast-forwarding the virtual clock, ensuring a true 60 Hz real-time frame rate. Added exact scanline duration calculation via `TWCR` and `LIPCR` for true mid-frame `LIF` interrupt firing.
- **Added LTDC Layer 2 hardware blending support**: Replaced the single-layer hardcoding with a full dynamic pixel compositor in the LTDC capture pass, supporting `ARGB8888`, `RGB565`, `ARGB1555`, and `ARGB4444` blending using `L2CACR` constant alpha.
- **Implemented hardware JPEG decoding for cover art**: Replaced the dumb "instant done" stub in `hw/misc/gnw_h7b0_jpeg.c` with a real bitstream parser that accumulates writes to `DIR`. It intercepts `End of Image` markers (`FF D9`) and triggers a full decode using `stb_image.h`. To avoid the complexity of YCbCr 4:2:0 MCU block conversion, the decoded RGB buffer is bypassed directly into the DMA2D engine when a YCbCr foreground format (`FGPFCCR.CM == 0xB`) is requested, resulting in perfectly accurate cover art rendering in the `retro-go` UI.
## 2026-07-10 (part 3 — flicker/speed root-cause, DMA completion, battery)

Root-caused and fixed the flicker + "runs too fast" bugs flagged as the
prior session's starting point — they turned out to be two separate real
bugs, not one. See `docs/session-2026-07-10-part5-flicker-speed-dma-battery.md`
for the full narrative, including a still-open pause/options-overlay-specific
flicker that is confirmed *not* the same bug (ruled out GPIO input bounce,
LTDC enable/format toggling, and Layer2/CLUT).

- **Fixed the real flicker root cause**: `hw/display/gnw_h7b0_ltdc.c`'s
  `SRCR.VBR` bit was cleared the instant it was written, making
  `gnw-chainloader`'s/retro-go's `gui_refresh()`-style busy-wait on
  `SRCR & (VBR|IMR)` a no-op — that wait is real firmware's *actual*
  frame-rate throttle (confirmed by reading `gnw-chainloader`'s and
  `game-and-watch-retro-go-sd`'s real source, both checked out locally).
  Measured `gui_refresh()` looping at ~1000x/sec instead of ~60Hz as a
  result. Fixed: `VBR` now stays set until the real vblank tick, which
  applies the deferred `CFBAR`/`CFBLR`/`CFBLNR`/`PFCR` shadow→active
  reload *and* clears the bit, genuinely pacing the whole redraw loop.
- **Fixed the real "runs too fast" root cause**: `hw/misc/gnw_h7b0_dma.c`
  had zero transfer-completion logic and wasn't wired to the NVIC at all
  (16 DMA1/DMA2 stream IRQs, none connected). retro-go's audio playback
  paces emulated game speed off SAI1's DMA half/full-transfer-complete
  interrupt; with none modeled, it hung solid on starting a game. Fixed:
  real per-stream completion timing (derived from `NDTR` at an assumed
  48kHz item rate, since this device has no way to know a stream's
  DMAMUX-routed peripheral) plus real NVIC wiring for all 16 stream IRQs
  in `hw/arm/gnw_h7b0_soc.c`.
- Also fixed along the way: ADC `CR.ADCAL` never self-clearing (hung
  `patched-zelda-bank1.bin`'s ADC calibration); RTC `CR.ALRAE/ALRBE/WUTE`
  never setting their matching `SR` flag (hung the same image's next
  boot stage); TIM2-block `CR1.CEN` now drives a real per-instance
  counter instead of being inert (not the actual speed-bug root cause,
  but a real correctness gap in its own right).
- Added a real ticking RTC calendar (`TR`/`DR` now reflect actual
  elapsed time, seeded from host wall-clock at reset) — `gnw-chainloader`
  has a real clock display that only ever reads it, never sets it.
- Fixed battery always reading 0%: both firmwares read ADC1 channel 4
  (no I2C fuel gauge) with the same raw-value-to-percentage thresholds;
  `gnw_h7b0_adc.c` now returns a fixed value corresponding to 100% on
  `CR.ADSTART`, with a real (newly NVIC-wired) completion IRQ for
  retro-go-sd's interrupt-driven read path.
- Workflow: `sdcard.img` (repo root, full `dd` copy of the user's real
  SD card) is now the standard SD test fixture, via the same
  qcow2-overlay trick as the real-`/dev/mmcblk0` workflow. Always
  sample PC ~5x with a real `continue` (not just gdbstub re-attach,
  which re-halts on every attach and can look identical to a real hang)
  before concluding a run is stuck, and before screenshotting/asking the
  user to look at anything.

## 2026-07-10 (part 2 — gamepad, real-SD debugging, DMA2D)

Session picked up controller input, then chased real-hardware-SD testing
through five real hangs/crashes. See
`docs/session-2026-07-10-part3-gamepad-sd-debugging.md` for the full
narrative, the debugging technique used for each (mostly disassembly-only,
no debug symbols), and the "status flag never set → infinite poll" bug
pattern that accounted for most of them.

- Real xpad/gamepad support: `ui/sdl2-gamepad.c` polls SDL2's game
  controller API and synthesizes qcode key events via
  `qemu_input_event_send_key_qcode()`, reusing `gnw_h7b0_gpio.c`'s
  existing keyboard-based button dispatch untouched. D-pad + face buttons
  only (no analog stick support yet). Verified against a Hyperkin Xenon
  controller.
- Fixed `hw/misc/gnw_h7b0_spi.c`'s SD-card mode: `SR.RXP` was never set on
  a completed byte transfer, only `TXP`/`EOT`. Real firmware's
  `HAL_SPI_TransmitReceive()` polls `RXP` (not `EOT`) before reading each
  received byte, so every real SD response silently timed out and read
  back stale stack garbage instead of the card's actual byte.
- Fixed `hw/sd/ssi-sd.c` (upstream QEMU code): CMD58 (READ_OCR)'s R1
  response byte was hardcoded to `1` (idle) unconditionally. Real cards
  clear that bit once ACMD41 succeeds; hardcoding it stuck every SDv2
  card's voltage-negotiation check in "still idle" forever, so
  `sdcard_detect()` never found a card — independent of SPI1's RXP fix
  above, and independent of real vs. synthetic card images.
- Real DMA2D (Chrom-ART) device added: `hw/display/gnw_h7b0_dma2d.c` +
  header. R2M/M2M/M2M_PFC/M2M_BLEND modes; ARGB8888/RGB565/ARGB1555/
  ARGB4444 (+ L8-via-CLUT input); clean-room pixel math (per this repo's
  licensing note on `mk-snes/gnw-mk`, referencing only
  `../minicraft-gnw/tools/retro-go-porting-toolkit/host/qemu/dma2d_emu.h`
  for register/mode semantics). Fixes a real BusFault in
  `HAL_DMA2D_Init()` — DMA2D was previously entirely unmapped.
- Fixed `hw/misc/gnw_h7b0_jpeg.c`: the real "start decode" trigger is
  `CONFR0.START`, not `CR` (CR is codec-enable/interrupt-enable, set once
  and left alone) — the initial fix hooked the wrong register. On a real
  `CONFR0.START` write, now fakes an instant "done" completion (`SR.EOCF`
  set, `OFNEF`/`OFTF`/`COF`/`IFTF`/`IFNFF` cleared) so
  `HAL_JPEG_Decode()`'s poll loop (and its input-refill sub-loop) actually
  terminates instead of walking a buffer off the end of AXISRAM. Decoded
  output is garbage (no real codec), but that's a correctness gap for
  later, not a crash.
- Fixed `hw/misc/gnw_h7b0_tim2.c` (covers the whole TIM2-TIM14 block):
  `EGR.UG` (force update event) now sets `SR.UIF`, applied generically at
  the standard 0x400-per-instance stride so it covers every timer in the
  block. Found via a silent black-screen hang in a patched, symbol-less
  firmware image — pure disassembly tracing (`r0` resolved to TIM5's base
  address) — a real hardware-timer busy-wait (`EGR.UG=1` then spin on
  `SR.UIF`) never terminated since nothing ever set `UIF`.
- Fixed `hw/misc/gnw_h7b0_rtc.c`'s `ICSR` write handler: it unconditionally
  rewrote the whole register to `(INIT)|INITF|RSF|INITS` on every write,
  silently zeroing `WUTWF`/`ALRBWF`/`ALRAWF` (bits 2/1/0, all set in
  `ICSR`'s real `0x7` reset value) after the very first write.
  `HAL_RTC_SetAlarm_IT()` polls `ALRAWF` before reprogramming the alarm
  register; once zeroed it never returned, hanging in an endless
  `WPR`/`CR`/`SCR` unlock-retry sequence before LTDC ever got configured.
  Same "instant ready" simplification as the register's other bits.
- Workflow: booting against a **real physical SD card** (`-drive
  if=sd,format=raw,file=/dev/mmcblk0`) hits two real constraints, not
  QEMU bugs: (1) QEMU's SD model requires an exact power-of-2 backing
  size, but real cards' raw byte length essentially never is one — wrap
  the real device in a qcow2 overlay with a full power-of-2 virtual size
  (`qemu-img create -f qcow2 -b /dev/mmcblk0 -F raw overlay.qcow2 8G`),
  which also means writes land in the overlay, not the physical card, no
  read-only mode needed to avoid touching real data. (2) the device node
  is normally `root:disk` and unwritable by a regular user — grant access
  via a one-off `chown root:plugdev` rather than a permanent group change
  if that's not wanted.

## 2026-07-10

- Added real button input (`hw/misc/gnw_h7b0_gpio.c`): a
  `QemuInputHandler` maps keyboard presses (and, best-effort, mouse-
  button events) onto the correct GPIO port's `IDR` bit per
  `gnw-chainloader`'s real button-pin table. Verified end-to-end by
  moving the chainloader menu cursor with a keyboard press. Real
  xpad-style gamepad support is blocked on a QEMU-version limitation,
  not a design gap: the pinned v9.2.4 base has no SDL2 joystick
  backend at all, so no real controller's buttons can reach any QEMU
  device yet — see STATUS.md.

- Fixed `hw/misc/gnw_h7b0_spi.c`'s `SR.TXP` being incorrectly gated on
  `CR1.SPE`: real hardware's TXP reflects TxFIFO occupancy and is set
  regardless of whether the peripheral is enabled. The old model only
  worked for `gnw-chainloader`'s SPI1 pattern (sets `SPE` immediately
  before every byte, only ever polls `EOT`); retro-go-sd's HAL-based
  `user_diskio_spi.c` polls `TXP` *before* `HAL_SPI_Transmit()` ever
  sets `SPE`, hanging forever. `gw_retro_go_sd_bank1.elf` now boots
  all the way to its own real UI ("No SD CARD found" screen).

- Added real LTDC vblank/line-interrupt modeling
  (`hw/display/gnw_h7b0_ltdc.c`, IRQ wired to NVIC's `LTDC_IRQn`=88 in
  `hw/arm/gnw_h7b0_soc.c`): a plain ~60Hz `QEMUTimer` now drives real
  `IER`/`ISR`/`ICR` registers instead of leaving them as inert shadow
  regs. Fixed both `gnw-chainloader`'s menu-flicker (its `gui_refresh()`
  was grabbing in-progress-redraw frames because its vblank wait never
  waited on anything real) and a hard, permanent hang in retro-go-sd's
  boot-logo animation (`lcd_wait_for_vblank()`).

- Fixed a false "button held" bug in `hw/misc/gnw_h7b0_gpio.c`: `IDR`
  was a zero-initialized plain-RAM placeholder, and since real G&W
  buttons are active-low, that read as *every* button permanently
  pressed — silently forcing `gnw-chainloader`'s boot-time "God Mode"
  button-override straight into an OFW-reflash screen on every launch.
  `IDR` now resets to `0xFFFF` per port (11 ports, A-K) instead of 0.

- Added a real hardware CRC-32 unit (`hw/misc/gnw_h7b0_crc.c`,
  `0x40023000`): a bit-serial engine matching the documented STM32
  algorithm exactly (configurable poly/init/reversal via
  `POL`/`INIT`/`CR`). `gnw-chainloader`'s `ofw_crc32()` uses this
  peripheral, not software, to verify OSPI flash content against
  baked-in checksums before trusting it — a plain-RAM stub always
  read `DR` back as 0 and made every verification fail silently.

- Added real OCTOSPI indirect-mode command decoding
  (`hw/misc/gnw_h7b0_ospi.c`): `RDID`/`RDSR`/`RDCR`/`SFDP`/`WREN`/
  `RSTEN`/`RST` now answer as a synthetic Macronix MX25U51245G
  (64MB, JEDEC `C2 25 3A`, matching `EXTFLASH_SIZE`'s existing 64M
  placeholder). Fixes retro-go's `OSPI_Init()` hard-`assert(!"Can't
  communicate with the external flash!")`.

- Added plain-RAM placeholders for TAMP (`0x58004400`), WWDG
  (`0x50003000`), and the TIM2/3/4/5/6/7/12/13/14 block
  (`0x40000000`-`0x400027FF`) — all found entirely unmapped (the
  "Lockup: can't escalate" abort pattern) while boot-testing a real
  OEM firmware image (`patched-zelda-bank1.bin`, real Zelda firmware
  with a Marian-Muller-style bank-swap patch) for the first time,
  combined with a real retro-go Bank2 image and the existing extflash
  backup. A scary-looking fault cascade this surfaced
  (`PC=0x0801ad68`, `LR=0xffffffe9`, a long tail of `Invalid read at
  0xFFFFFFEx`) turned out to be a downstream symptom of the WWDG gap,
  not a separate bug — gone entirely once WWDG was mapped.

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
- Added Xbox/xpad-style gamepad support (`ui/sdl2-gamepad.{c,h}`): polls
  SDL2's `SDL_GameController` API and synthesizes keyboard `InputEvent`s
  via `qemu_input_event_send_key_qcode()`, reusing
  `gnw_h7b0_gpio.c`'s existing keyboard qcode mapping unchanged (same
  shortcut xemu takes against its own SDL/ImGui UI, adapted to stock
  QEMU's `ui/sdl2*.c`). First connected controller only, fixed mapping
  (d-pad -> arrows, A/B -> Z/X, Start -> Return, Back -> Right-Shift,
  Guide/right-shoulder -> Escape/Pause); no qapi/ui.json changes. Builds
  cleanly under the existing `sdl.found()` gate; not yet tested against
  physical hardware.
