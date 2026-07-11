# DMA2D/JPEG real YCbCr-blend pipeline — session writeup (2026-07-11)

Continuation of the same-day LTDC flicker investigation
(`docs/session-2026-07-11-ltdc-flicker-investigation.md`). That doc's plan
(RRIF fix, LTDC per-layer compositing generalization, JPEG bit-layout
fidelity) landed, built clean, and was live-tested: **no improvement** to the
reported menu/coverflow flicker. This document covers the follow-on work and
where things stand now.

## What changed in this session

Replaced the JPEG "hack buffer" (`gnw_h7b0_jpeg_get_hack_buffer()`, a raw
QEMU-heap RGB565 pointer handed directly to DMA2D, bypassing `FGMAR`/`FGOR`
entirely) with an architecturally real pipeline:

- `hw/misc/gnw_h7b0_jpeg.c`/`.h`: decode still happens synchronously at
  EOI-detection (unchanged, no timers — this project has twice flagged
  deferred/timer-based completion as a real hang risk, see the SPI
  regression precedent), but now produces a real `DOR` (Data Output
  Register) that firmware's own polling loop (`HAL_JPEG_Decode`'s
  `JPEG_Process`) drains via ordinary CPU reads into a **guest-RAM** buffer
  it controls — exactly mirroring how real polling-mode JPEG decode has no
  DMA involved at all (confirmed by reading
  `sdk/stm32h7xx-hal-driver/Src/stm32h7xx_hal_jpeg.c`'s `HAL_JPEG_Decode`/
  `JPEG_Process`: it's plain register-polling CPU code, no DMA channel
  setup). That guest buffer address then becomes a legitimate `FGMAR` for
  DMA2D — no more special-cased pointer handoff.
- `hw/display/gnw_h7b0_dma2d.c`/`.h`: fixed `DMA2D_CR_MODE_MASK` from 2 bits
  to the real 3-bit width (firmware's actual `MODE=5` "`BLEND_BG`" was being
  truncated to `1` "`PFC`", so the real blend firmware performs for cover art
  literally never happened in emulation before this). Added real `A8` format
  fetch (`FGCOLR`/`BGCOLR` wired in, previously dead registers), a real
  YCbCr fetch reading planes out of guest memory, and implemented
  `M2M_BLEND_FG`/`M2M_BLEND_BG` as genuine "blend with fixed color" modes.

## Real bugs found (and fixed) during this work

1. **`BLEND_BG`/`BLEND_FG` are "fixed color" modes, not "two-buffer blend"
   modes.** Confirmed via `sdk/.../stm32h7xx_hal_dma2d.h`: "DMA2D memory to
   memory with blending transfer mode **and fixed color FG/BG**". The first
   implementation pass treated them identically to plain `M2M_BLEND`
   (fetching BOTH sides from real buffers via `FGMAR`/`BGMAR`) — but real
   firmware never configures `BGMAR` for `BLEND_BG` (confirmed live:
   `bgmar=0x0`), so that first pass was reading uninitialized/stale guest
   memory as the background on **every single transfer**, producing garbage
   pixels and severe flicker (a real regression, caught via live testing,
   fixed by treating one side as a synthesized constant from `FGCOLR`/
   `BGCOLR` instead of a fetch).
2. **Chroma-subsampling storage-size mismatch.** The original design choice
   (documented in the plan as a deliberate scope trade-off) was to store
   Cb/Cr at full resolution rather than real subsampled resolution, for
   simplicity. This was wrong: real firmware sizes its JPEG output
   destination buffer for the **real, subsampled** byte count (e.g. 1.5×
   width×height for 4:2:0), and its polling loop only drains up to what it
   believes is the full output — so serving ~2× that many bytes meant the
   tail of firmware's buffer never got written, leaving stale/uninitialized
   bytes there. This showed up as a **banded/static corruption pattern**
   directly visible on screen (confirmed via user screenshot: a horizontal
   noise band across the selected cover art). Fixed by actually subsampling
   Cb/Cr at DOR-serve time per the image's real SOF0 H/V sampling factors
   (hand-parsed, since `stbi_info_from_memory()` doesn't expose them),
   matching real hardware's real output size.
3. **Out-of-bounds row-wrap in the YCbCr fetch.** `gnw_h7b0_dma2d_read_ycbcr`
   addressed planes using the JPEG's real decoded width as row stride, but
   didn't bounds-check against the DMA2D transfer's own (potentially larger)
   configured geometry — e.g. a 100×100 "no cover" placeholder image blended
   into a 128×96 transfer box read 28 extra columns per row that wrapped
   into the *next* row's real pixel bytes (no padding exists in the tightly
   packed plane storage). Fixed with an explicit bounds check returning
   transparent (`0x00000000`) for out-of-range columns/rows.

## A false lead worth remembering

After fixing the two corruption bugs above, the coverflow still flickered
noticeably worse than the pre-session baseline. Investigating further, a
checksum trace on DMA2D's blended output showed the **same output address**
(`omar`) producing ~10 different checksums cycling repeatedly, even at total
idle (no user input) — which looked exactly like an active instability bug.

It wasn't one. Tracing the firmware (`Core/Src/retro-go/gui.c`'s
`gui_draw_list`/`gui_draw_coverlight_h`) showed that `pCover_Buffer` is a
**shared compositing scratch buffer**: DMA2D composites each of up to 5
visible cover images into it, one at a time, and a *separate* plain-CPU-
memcpy step (`odroid_display_write_rect` in
`Core/Src/porting/odroid_display.c`) then positions each composited result
onto its real on-screen coordinates. So "the same address producing several
different checksums" is not instability at all — it's 5 different real cover
images legitimately sharing one scratch buffer, exactly as real hardware
would do it too. **Lesson: when using a memory checksum/stability trace to
hunt a bug, verify what the traced address actually represents (a stable
per-frame target vs. a shared/reused scratch buffer) before treating
variance there as evidence of a bug.** This cost a significant amount of
time before the mistake was caught by cross-referencing the firmware's own
draw-loop code.

## Current status of the original flicker report

- **Confirmed fixed**: visible static/noise/miscolored-pixel corruption on
  cover art (both bugs above). User confirmed this fix directly.
- **Confirmed NOT fixed**: the original menu/coverflow flicker (selected
  item's text, leftmost background cover) — reported as "worse than the
  original baseline" even after both corruption fixes landed.
- **Ruled out** as the remaining cause, with reasoning:
  - JPEG decode itself: confirmed fully deterministic at idle (checksum-
    stable per source image across hundreds of redraws).
  - DMA2D compositing: now architecturally correct (real fetch, real
    blend-with-fixed-color semantics); the earlier "still varying" signal
    was the false lead above, not real instability.
  - The final CPU `memcpy` blit (`odroid_display_write_rect`) into the LCD's
    software framebuffer: plain deterministic guest-CPU memory operation,
    not something a device-model bug could touch.
  - The main idle redraw path (`gui.c:425-433`,
    `gui_draw_status`/`gui_draw_list` → `lcd_sleep_while_swap_pending()` →
    `lcd_swap()`) appears to throttle to real vblank the same way the modal
    dialog's `_repaint()` loop does (already fixed/confirmed via the earlier
    RRIF work) — reasoned through by reading the code, **not yet verified
    live** (see Next steps).
- **Not yet investigated**: actual LTDC-side scanout/capture timing relative
  to firmware's real double-buffer swap cadence, under live tracing (not
  just static code reading). This is the most likely remaining place to
  look — the earlier RRIF/compositing fixes address *correctness* of the
  reload mechanism but not necessarily *fine-grained timing* relative to
  QEMU's own ~60Hz vblank capture vs. the UI's independent redraw-poll rate.

## Next steps (not yet started)

1. Live-trace the actual real-vblank cadence vs. firmware's `lcd_swap()`
   call cadence during the idle coverflow screen specifically (not a
   game-core scenario) — confirm firmware really is throttling to ~60Hz
   here and not spinning faster.
2. Consider whether QEMU's UI refresh timer (`gnw_h7b0_ltdc_update_display`,
   driven by `content_dirty`) polling at a different rate than the device's
   own 60Hz vblank capture could produce a *visual* stutter/flicker even
   when the underlying framebuffer content is itself stable and correct —
   i.e. a presentation-cadence issue distinct from content correctness.
3. Only revisit the previously-deferred `odroid_overlay.c` `repaint==NULL`
   firmware-bug lead if a *different* call site than the one already ruled
   out (`rg_emulators.c:1022`, confirmed to use the safe `repaint != NULL`
   path) turns out to be the actual repro path — re-confirm the exact
   button/menu the user means before spending more time there.

## Workflow/tooling notes for whoever resumes this

- **Live memory-content diffing across two points in time is not enough
  evidence of a bug on its own** — see the "false lead" section above. Pair
  it with understanding what the traced address structurally represents
  (checked against the actual firmware source) before concluding instability.
- **Temporary `fprintf(stderr, ...)` instrumentation directly in device-model
  code**, gated by a `static int` counter cap (e.g. `if (count < N)`), was an
  effective and fast way to get real, non-speculative evidence during this
  session — used to confirm decode determinism, DMA2D mode/format dispatch,
  and plane-dimension mismatches. Always remove it before handing back (grep
  for the debug tag string, e.g. `dma2d-blend-debug`, to confirm full
  removal) rather than leaving it silently in place.
- **QEMU's monitor in this build has no `screendump` command** (checked via
  `help` over a telnet-connected monitor socket,
  `-monitor telnet:127.0.0.1:PORT,server,nowait`) — screenshots for visual
  verification currently require asking the user to share one manually;
  there is no way to self-serve a screenshot from this build as currently
  configured.
- **`gdb-multiarch -batch -ex "target remote localhost:1234" -ex "x/32xb <addr>"`**
  (with qemu launched with `-s`) works fine for one-shot physical-memory
  peeks without halting the guest — used to directly confirm guest RAM
  content at a suspected buffer address matched/mismatched expectations.
- **Chaining `pkill -9 -f "build/qemu-system-arm"` and a new `nohup ... &`
  launch in the same Bash tool call was unreliable** in this session
  (repeatedly returned a bare "Exit code 144" with no process actually
  started, several times in a row) — the fix that consistently worked was
  issuing the `pkill` and the relaunch as two **separate** Bash tool calls.
  Worth remembering as a standing workaround, not just a one-off flake.
- The SOF0-hand-parse / chroma-subsampling logic added this session
  (`gnw_h7b0_jpeg_parse_sof0_luma_sampling`) is a real, if minimal, JPEG
  marker parser now living in this codebase — worth reusing rather than
  re-deriving if any future work needs real per-image sampling-factor
  information again.

## Session end-state (2026-07-11, later same day)

Two more things landed after the pipeline work above:

1. **A likely-real flicker fix from a concurrent session**:
   `hw/display/gnw_h7b0_ltdc.c`'s vblank capture was changed to fire only
   on firmware's `SRCR.VBR` write (gated on `!s->content_dirty`) instead of
   an independent fixed-rate timer. This addresses a timing axis nothing
   in this doc's investigation touched: the *old* unconditional per-tick
   capture could sample the framebuffer mid-draw whenever the emulated
   CPU's render time for a given frame varied (which JPEG/DMA2D-heavy
   coverflow frames do, relative to fixed-cost gameplay blits) — a
   plausible root cause for exactly the reported "flip back then snap"
   symptom. **Not yet confirmed live as of this doc's writing** — see
   `STATUS.md` for current confirmation status before assuming this is
   settled. Known gap: content that never writes `SRCR.VBR` no longer gets
   captured at all (an earlier draft, preserved in `test_patch.diff` in the
   repo root at the time of writing, kept a fallback path for this case;
   the version that landed dropped it).
2. **A real JPEG decode performance bug, unrelated to the flicker
   investigation**: `perf` during a menu scroll-loop showed >16% of total
   CPU in QEMU's own MMIO-dispatch/BQL-lock machinery. Root cause: the
   JPEG model never set `OFTF` (output FIFO threshold), forcing firmware's
   polling loop into a one-word-at-a-time drain path instead of the real
   8-words-per-check bulk path — ~8x more separate flag-check MMIO round
   trips per decode for zero reason (real hardware's own
   `JPEG_StoreOutputData` still reads DOR one word at a time internally
   regardless of threshold, so this only affects polling-*loop* overhead,
   not real data volume). Fixing it cut that overhead to ~11%. GPIO/input
   reading was checked and ruled out as a contributor via the same
   profile — it doesn't appear in the hot path.

Next session should start by confirming (1) live, then decide whether to
keep chasing the flicker or move on, per `STATUS.md`'s "Next objective"
(stock/official firmware bring-up is the next planned pivot regardless).
