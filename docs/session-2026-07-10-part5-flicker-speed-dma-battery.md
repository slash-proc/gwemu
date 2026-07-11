# Session 2026-07-10 part 5 — flicker/speed root-cause, DMA completion, battery spoof

Picked up exactly where part 4 (`session-2026-07-10-part3-gamepad-sd-debugging.md`'s
"Known remaining issues") left off: flicker present on both `gnw-chainloader`
and retro-go, plus retro-go's game logic running too fast. Both were
flagged there as one session's starting point, suspected related but not
root-caused. This session root-caused and fixed several real, distinct bugs
behind those symptoms — plus found a new one (pause-menu-specific flicker)
that's still open.

## Fixed this session

1. **ADC `CR.ADCAL` never self-clears** (`hw/misc/gnw_h7b0_adc.c`) — real
   hardware self-clears bit 31 (`ADCAL`, "calibration in progress") when
   calibration completes; our stub left it set forever, hanging any
   firmware that polls it. Found via `patched-zelda-bank1.bin` stalling
   completely mid-ADC-config. Fixed: cleared instantly on write, same
   "instant approximation" pattern as `ADEN`→`ADRDY`.

2. **RTC alarm/wakeup-timer polling hang + real ticking calendar**
   (`hw/misc/gnw_h7b0_rtc.c`): `CR.ALRAE/ALRBE/WUTE` enable edges now
   instantly set the matching `SR` flag (with `SCR`-based clearing and
   `MISR` masking), unblocking firmware that busy-polls for an alarm/
   wakeup to fire — found via the same Zelda image advancing past ADC
   only to hang in an RTC-alarm-adjacent state machine next. Separately,
   `TR`/`DR` (time/date) are now a **real ticking calendar** seeded from
   the host's wall-clock at reset and advanced via `QEMU_CLOCK_VIRTUAL`
   (user request — `gnw-chainloader` has a real clock display that only
   ever reads `TR`/`DR`, never sets it, so seeding from 2000-01-01 looked
   obviously wrong).

3. **TIM2-block real per-instance counter** (`hw/misc/gnw_h7b0_tim2.c`):
   `CR1.CEN` now drives a real per-stream `QEMUTimer` (`(ARR+1)*(PSC+1)`
   at an assumed 200MHz APB clock, clamped 1ms-2s) instead of being a
   plain shadow. The pre-existing `EGR.UG`→`SR.UIF` instant-hack is
   unchanged (matches real hardware: `UG` really is an immediate update).
   This was suspected but not confirmed as part of the speed bug; turned
   out not to be the actual root cause (see #5) but is a real correctness
   gap worth having regardless — any firmware timing loop that enables
   CEN and polls `UIF` now gets a plausible real duration instead of
   hanging or completing instantly.

4. **LTDC `SRCR.VBR` was cleared synchronously on write — the actual
   flicker root cause** (`hw/display/gnw_h7b0_ltdc.c`): found by reading
   `gnw-chainloader`'s actual source (`src/chainloader/gui.c`) — a local
   checkout exists at `../gnw-chainloader`, and `game-and-watch-retro-go-sd`
   at `../game-and-watch-retro-go-sd`; both were consulted directly
   instead of guessing from disassembly. `gui_refresh()` does exactly
   what real hardware expects: request the reload
   (`HAL_LTDC_Reload(..., LTDC_RELOAD_VERTICAL_BLANKING)`), then
   busy-wait on `SRCR & (VBR|IMR)` clearing as its **actual frame-rate
   throttle**. Our model cleared `VBR` the instant it was written
   (same "instant" treatment as `IMR`, which really is instantaneous on
   real hardware — but `VBR` isn't), making that wait a no-op: measured
   `gui_refresh()` looping at ~1000x/sec instead of ~60Hz. Fixed: `VBR`
   now stays set in the register until the next real vblank tick
   (`gnw_h7b0_ltdc_vblank_tick()`), which both applies the deferred
   shadow→active `CFBAR`/`CFBLR`/`CFBLNR`/`PFCR` reload *and* clears the
   bit — genuinely pacing firmware's whole redraw loop to ~60Hz, not
   just the buffer-pointer swap. **Verified**: `gnw-chainloader`'s menu
   is now visibly smooth (previously flickered constantly).
   - A companion attempt — forcing the pixel blit
     (`gnw_h7b0_ltdc_update_display()`) synchronously at the vblank tick
     instead of leaving it to QEMU's own independent UI-refresh timer —
     was tried and made things *worse*, then reverted. Left as a
     documented dead-end in the code comment.

5. **DMA controller had zero completion/interrupt logic at all — the
   actual "runs too fast" root cause** (`hw/misc/gnw_h7b0_dma.c`,
   `hw/arm/gnw_h7b0_soc.c`): `hw/misc/gnw_h7b0_dma.c` was a plain
   register shadow with no side effects, and wasn't even wired to the
   NVIC (no `sysbus_connect_irq` for any of DMA1/DMA2's 16 stream IRQ
   lines). `game-and-watch-retro-go-sd/Core/Src/gw_audio.c`'s audio
   playback starts a circular-mode DMA1 Stream0 transfer (SAI1 Tx) and
   busy-waits on `dma_counter`, incremented only by
   `HAL_SAI_Tx{Half,}CpltCallback` — themselves only ever called from a
   real DMA half/full-transfer-complete interrupt. With none modeled,
   that wait never ended: found via `retro-go-bank1-flash.bin` (a
   flash-only, no-SD build) freezing solid trying to start a game, right
   after a burst of OSPI reads (loading the game/core) and RCC writes
   (audio-clock enable). Confirmed via a genuine single-step trace
   (not just async PC sampling — see the
   `feedback_pc_sample_before_screenshot` memory) landing on the exact
   same ~35-instruction busy-wait spinning on two RAM words stuck at 0.
   - Fixed: added real per-stream (16 total, both controllers) transfer
     completion — on `CR.EN` 0→1, schedule a half-transfer tick, then a
     full-transfer tick, setting the matching `LISR`/`HISR` flag and
     firing that stream's now-real NVIC IRQ if enabled; circular mode
     auto-restarts, one-shot clears `EN`. IRQ numbers per
     `stm32h7b0xx.h`: DMA1 streams 0-6 = 11-17, stream 7 = 47; DMA2
     streams 0-4 = 56-60, streams 5-7 = 68-70 (not contiguous).
   - **First attempt used a fixed 2ms half-transfer delay and
     overcorrected**: this unblocked the freeze, but then the actual
     game ran at ~240fps instead of ~60fps — retro-go paces emulated
     game speed off audio DMA completion
     (`AUDIO_BUFFER_LENGTH`=1077 samples/48kHz ≈ 22.4ms per half,
     sized to match one real video frame per `gw_audio.h`'s own
     `GWENESIS_AUDIO_BUFFER_LENGTH_PAL` comment), and 2ms is ~11x
     faster than that — same order of magnitude as the observed ~4x
     speedup. Fixed properly: the half-transfer delay is now derived
     from `NDTR` assuming a flat 48kHz item rate (this device can't
     know a given stream's DMAMUX request-line peripheral, so it can't
     distinguish "this is SAI1 audio" from any other DMA use — 48kHz is
     a reasonable universal stand-in, clamped 1ms-500ms). **Verified**:
     the same game now runs at the intended speed.

6. **Battery always read 0%** — a background agent traced both
   firmwares' battery-percentage path to ADC1 channel 4 (PC4, no I2C
   fuel gauge involved; the bq24072 charger IC is only consulted via two
   plain GPIO status lines). Both firmwares use the same raw-value
   thresholds (`≤11000`→0%, `≥13000`→100%, linear between). Fixed in
   `hw/misc/gnw_h7b0_adc.c`/`.h`: a `CR.ADSTART` write now loads `DR`
   with a fixed value (13500, safely above the 13000 "full" threshold),
   sets `ISR.EOC`, self-clears `ADSTART`, and pulses a (newly added,
   NVIC-wired — `ADC_IRQn`=18) IRQ if `IER.EOCIE` is set — covering both
   `gnw-chainloader`'s busy-poll-`EOC` pattern and retro-go-sd's
   interrupt-driven `HAL_ADC_Start_IT()` pattern.

## Still open: pause/options-overlay-specific flicker

Confirmed **not** the same bug as #4 above. Precise symptom (direct
user observation, not inferred): the base game (or main menu) renders
at a smooth, correct ~60Hz with **no** flicker at all. The instant an
overlay is drawn on top of that (retro-go's in-game pause menu, or even
just the options submenu on top of the main menu) — real content from
*underneath* that overlay is clearly visible for what looks like ~1
frame, then it's gone, repeatedly ("looks like the overlay layer is
going in and out very quickly").

Ruled out this session, with evidence:
- **Not GPIO input bounce.** Added temporary timestamped logging of
  every raw button edge QEMU's input core delivers to
  `gnw_h7b0_gpio.c` (removed again once ruled out — if re-adding, see
  the `fprintf(stderr, "[gpio-debug] ...")` pattern near
  `gnw_h7b0_gpio_input_event()`'s `INPUT_EVENT_KIND_KEY` case). Real
  session log: 3 clean press/release pairs over 28 seconds, no
  double-events, no chatter. Whatever's happening, it isn't spurious
  input re-triggering the pause menu's open/close state machine.
- **Not GCR/PFCR/L1CR toggling.** Sampled all three live via the QEMU
  monitor while the flicker was actively happening; all stayed
  constant (`LTDCEN`/`LEN` enabled, `PFCR`=RGB565 the whole time).
- **Not the `SRCR.VBR` bug from #4** — that's fixed, and this
  reproduces even with the fix in place; it also reproduces in
  contexts (options submenu over the main menu) that don't involve any
  of the code paths #4 touched.
- **Not a Layer2/CLUT/LUT8 pixel-format issue** — grepped both firmware
  trees; the overlay/dialog code (`Core/Src/porting/odroid_overlay.c`,
  `Core/Src/porting/common.c`) never touches Layer2 or `HAL_LTDC_
  ConfigLayer` for a second layer, and never switches pixel format;
  it's plain software alpha-style darkening + drawing onto the same
  single RGB565 buffer via `lcd_get_active_buffer()`.

Current leading theory (untested): something in the interaction between
firmware's `active_framebuffer` software index (flipped *before* the
real hardware reload takes effect — see `lcd_swap()`/
`HAL_LTDC_ReloadEventCallback()` in `gw_lcd.c`) and our
`gnw_h7b0_ltdc_reload_active()`'s timing relative to when the guest's
own `RRIF`-triggered ISR actually runs within a given vblank tick. Traced
through by hand at length this session without finding a conclusive
smoking gun either way — the next step should be empirical, not more
static reasoning: log every `L1CFBAR` shadow write (i.e. every
`HAL_LTDC_SetAddress()` call) and every `gnw_h7b0_ltdc_reload_active()`
application, both timestamped, and correlate against exactly when the
overlay is opened. (Instrumentation for this was scoped but not yet
added as of this writing — see if a later session picked it up before
re-deriving it.)

## New workflow/collaboration notes from this session

- **Always sample PC ~5x (with real `continue`, not just re-attaching
  the gdbstub) before concluding a run is hung**, and before
  screenshotting/asking the user to look at anything. Re-attaching
  gdb's `target remote` halts the CPU on every attach, so repeatedly
  attaching-and-reading without an explicit `continue` in between just
  re-reads the same frozen snapshot and looks identical to a real hang
  even when the CPU is running fine. Cross-checked this the hard way
  more than once this session.
- **Always use `-display sdl`** (a real window), never `-display none`,
  for these test runs — the user wants to watch it live.
- **`sdcard.img`** (repo root, a full `dd` copy of the user's real
  physical SD card) is now the standard SD test fixture, via the same
  qcow2-overlay trick as `../minicraft-gnw`'s real-`/dev/mmcblk0`
  workflow (`qemu-img create -f qcow2 -b sdcard.img -F raw
  /tmp/sd-overlay.qcow2 8G`).
- This build has `screendump`/`ppm_save` unavailable (`CONFIG_PIXMAN`
  is `undef` — only `libpixman-1-0` is installed, not the
  `-dev` package/`.pc` file `configure` needs). Not fixed this session
  (would need a `sudo apt install libpixman-1-dev` + reconfigure/rebuild,
  deliberately not done without the user's explicit go-ahead for the
  package install). All flicker/behavior diagnosis this session was
  done via live register reads over the QEMU monitor plus gdb
  single-step tracing, not visual screenshots.
