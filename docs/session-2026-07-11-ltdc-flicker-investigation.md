# LTDC menu-flicker investigation — state dump (context cutoff)

Written in a hurry to preserve findings before context ran out. Not
polished; straighten out later. Everything below is from research this
session, some verified live, some not yet acted on.

## Status of the plan in progress

Plan file: `/home/doug/.claude/plans/i-want-you-to-enumerated-breeze.md`
("LTDC full-fidelity pass + the specific menu flashback bug"). Task list:

- **#12 DONE, tested, no improvement**: fixed `hw/display/gnw_h7b0_ltdc.c`'s
  `gnw_h7b0_ltdc_vblank_tick()` — `RRIF` was being raised unconditionally
  every vblank tick instead of only when a real reload (VBR or IMR)
  occurred. Real bug, fixed, build clean, live-tested by user: **did not
  fix the reported flicker**. Kept anyway (correct regardless).
- **#13 NOT STARTED**: generalize `gnw_h7b0_ltdc_capture_rows()` into a
  shared per-layer pipeline — color keying (L1CKCR/L2CKCR + COLKEN),
  window-clip + default color (WHPCR/WVPCR + DCCR/BCCR), generalized
  BF1/BF2 blend formula (currently hardcoded Layer2-only
  PAxCA-with-implicit-complement, ignoring BFCR), Bayer dithering
  (GCR.DEN, currently a true no-op given fully-32bpp internal pipeline but
  user wants it implemented anyway for fidelity), Layer2 gaps (no L8/CLUT
  support, shared single `clut[]` only correct for L1, no window
  clipping). Full register bit-layouts and per-register implementation
  sketches are in the plan file already — read that, not this doc, when
  resuming #13.
- **#14 NOT STARTED**: JPEG low-risk fidelity (`hw/misc/gnw_h7b0_jpeg.c`):
  precise CONFR1/CONFR3/CONFR4-7/CR bit-layout defines, widen CFR write
  mask to real JPEG_FLAG_ALL, CR flush-pulse self-clear, derive NF/
  subsampling from real `stbi_info_from_memory()` instead of a hardcoded
  placeholder. Explicitly do NOT add QEMUTimer-deferred EOCF (firmware's
  real call path uses `HAL_MAX_DELAY`, zero timeout escape — a stalled
  timer here hangs forever, worse than the SPI regression) and do NOT add
  finite input-FIFO backpressure (no drain mechanism exists to relieve it).

Decision from user: finish #13/#14 first, retest the flicker afterward.
Only revisit the firmware-side finding below if the flicker still exists
after #13/#14 land.

## Separate finding: a likely REAL FIRMWARE bug (not a qemu-gnw emulation gap)

This one is NOT in `qemu-gnw` — it's in the sibling firmware repo
`~/Nerd/git/game-and-watch-retro-go-sd`. Found via an Explore-agent research
pass (not yet independently re-verified by me after its report — treat as
a lead, not gospel, until re-checked):

- `Core/Src/porting/odroid_overlay.c`'s `odroid_overlay_dialog()` has an
  internal `_repaint()` closure (~line 897-919) called every frame of its
  modal input-polling loop. When given a non-NULL `repaint` callback
  (e.g. the coverflow's context menu, `rg_emulators.c:1022`, passing
  `&gui_redraw_callback`), it correctly does
  `lcd_sleep_while_swap_pending()` → full redraw → `lcd_swap()` each time —
  a clean, correct double-buffer cycle, same as normal gameplay/coverflow
  rendering.
- But several call sites pass `repaint == NULL`:
  `rg_emulators.c:75` (corrupted-ROM-file dialog), `rg_emulators.c:803`
  (cheat-codes dialog), and every `odroid_overlay_alert()` call
  (`odroid_overlay.c:1089`). When `repaint == NULL`, `_repaint()` skips the
  ENTIRE background-redraw branch — no `lcd_clear_active_buffer()`, no
  redraw, no buffer sync — and draws the dialog box directly onto whatever
  bytes are already sitting in the current physical buffer, then still
  calls `lcd_swap()` every loop iteration.
- Since `lcd_swap()` just toggles which of the two physical framebuffers
  is "active" without ever equalizing their contents, and these two
  buffers can hold DIFFERENT stale content (e.g. from two different past
  coverflow animation frames), this modal loop alternates the visible
  background behind the dialog between two different stale snapshots on
  every polled frame — a plausible, textbook mechanism for exactly the
  reported "flicker especially bad when opening a menu over the carousel"
  symptom.
- Corroborating evidence this is a known hazard pattern in the codebase:
  `show_preview_cb()` (`odroid_overlay.c:1479-1521`, save-state preview
  thumbnails) writes directly into `lcd_get_active_buffer()` OUTSIDE the
  normal swap cycle, and explicitly does a manual
  `memcpy(lcd_get_inactive_buffer(), lcd_get_active_buffer(), frame_size)`
  (line ~1515) specifically to avoid this exact two-buffers-diverge
  problem. The `repaint == NULL` dialog path has no equivalent sync step —
  that's the gap.
- Proposed fix (NOT YET APPLIED, and NOT YET FULLY CONFIRMED as root
  cause): add an `lcd_sync()`/one-time `lcd_clone()` call immediately
  before entering `_repaint()`'s modal `while(1)` loop whenever
  `repaint == NULL`, so both physical buffers hold identical pixels before
  the popup starts alternating between them.

## IMPORTANT open contradiction — user says real hardware does NOT show this flicker

If the above firmware-bug theory were the true, sole root cause, real
physical Game & Watch hardware running this same firmware should show the
identical flicker (it's a firmware logic bug, not an emulation-accuracy
gap) — but the user reports real hardware does NOT exhibit this. This
directly challenges the theory above. Possible explanations not yet
investigated:
1. The `repaint == NULL` call sites found (corrupted-ROM dialog, cheat
   codes, generic alerts) may not actually be the specific menu the user
   means by "opening a menu over the carousel" — there may be a DIFFERENT,
   not-yet-found call site that's the real culprit, possibly one that IS
   qemu-gnw-specific in nature.
2. Real hardware's two physical framebuffers might happen to be
   byte-identical much more often in practice (e.g. due to real timing
   making the "stale ghost" content coincidentally match), masking the bug
   there while qemu-gnw's different real-time behavior exposes it.
3. The actual root cause could still be qemu-gnw-side after all (something
   about our capture/vblank timing, buffer initialization, or the LTDC
   compositing gaps #13 is meant to fix) and the firmware finding above,
   while real, might be an unrelated red herring for THIS specific
   symptom.
4. Something about which exact buffer state qemu-gnw initializes
   `framebuffer1`/`framebuffer2` RAM to at boot/reset could make our
   "stale ghost" content more visually jarring/obviously-different than
   whatever real hardware's RAM happens to contain, even if the underlying
   firmware logic gap is identical on both.

**Do not act on the firmware fix until #13/#14 are done and the flicker is
retested.** If it persists, come back to this doc, re-verify the Explore
agent's findings firsthand (file:line references given above), and
seriously investigate the real-hardware-doesn't-show-it contradiction
before deciding whether to patch `game-and-watch-retro-go-sd` or keep
looking for a qemu-gnw-side cause.

## Misc context for whoever resumes this

- Session also completed (all committed, see git log on `gnw-h7b0` branch,
  commit around "Add remaining H7B0 peripheral models; fix audio clock
  tree and LTDC perf bug"): HSI-vs-HSE oscillator fix, PLL2 fractional-N
  off-by-one fix, SAI1SEL mux fix, dynamic PLL1/SYSCLK modeling for
  retro-go's CPU overclock feature, TIM2 live-clock fix, DMA timer drift
  fix, LTDC content-dirty redraw fix (was 35% of total CPU), LTDC L8/CLUT
  support, rate-limited shadow-register logging, OSPI DLR bounds-check +
  CCR address-size check, and tooling (`scripts/audit_peripheral_regs.py`,
  `scripts/perf_smoke.sh`, `docs/h7b0-clock-tree-findings.md`,
  `docs/peripheral-register-audit/*.md`). Audio confirmed solid across all
  tested cores (NES, Genesis, SMW, others) after that work. This flicker
  investigation is the one remaining loose end from that broader session.
- The SPI per-byte deferred-completion timing fix attempted earlier in the
  session caused a real hang and was reverted to synchronous completion —
  keep that in mind as precedent: this codebase has now twice flagged
  "deferred/timer-based completion is higher risk than it looks" (SPI,
  and the JPEG audit's explicit warning against the same pattern for
  EOCF) — don't re-attempt either without strong justification.
