# Performance improvement candidates (2026-07-14)

Goal: find a genuine, code-verified path to materially more raw TCG
throughput (target discussed: ~50%), not just plausible-sounding ideas.
Everything below was checked against the actual current source, not
general QEMU knowledge. Ranked by confidence.

## 1. DMA2D YCbCr->RGB pixel conversion uses floating point per pixel (CONFIRMED, high confidence)

`hw/display/gnw_h7b0_dma2d.c:166` (`gnw_h7b0_dma2d_read_ycbcr_buf`), called
once per output pixel from both `M2M_PFC` and `M2M_BLEND*` transfer loops
whenever the foreground is JPEG YCbCr (the JPEG cover-art path
STATUS.md/comments already flag as the heaviest on-screen content, e.g.
retro-go's coverflow):

```c
int r = yi + (int)(1.402 * cri);
int g = yi - (int)(0.344136 * cbi) - (int)(0.714136 * cri);
int b = yi + (int)(1.772 * cbi);
```

Three `double` multiplies plus float->int truncation per pixel, per
transfer, at up to `pixels_per_line * lines` pixels. This is real,
avoidable cost: standard fixed-point BT.601 conversion (16-bit
scaled integer constants + shift) is bit-compatible with this rounding
behavior for all practical inputs and removes all FP ops from the
hottest per-pixel path in the file. This is the single most concrete,
verifiable win found — it's pure computation happening `width*height`
times with no memory-access or MMIO cost to hide it behind.

The `cx = x * chroma_w / plane_w` integer division just above it is also
per-pixel but only matters when `chroma_w != plane_w` scaling is active;
lower priority than the float math.

## 2. LTDC per-pixel window-clip re-evaluated every pixel (CONFIRMED redundant computation, moderate confidence on payoff)

`hw/display/gnw_h7b0_ltdc.c`'s `gnw_h7b0_ltdc_capture_rows()` recomputes
`l1_in`/`l2_in` (window-clip bounds check against `WHSTPOS`/`WHSPPOS`) for
every single pixel in every row, even though in the overwhelmingly common
case (full-screen window, which is what retro-go/OFW actually configure
almost all the time) the check is `true` for the entire row. This is a
real, verifiable branch+compare per pixel that can be hoisted to a
per-row "is this row's window the full row" precheck, with a fast path
that skips the per-pixel test entirely. Confidence on payoff is moderate
(it's a cheap branch, not FP math), so this is a real but smaller win
than #1 — worth doing only after #1 is confirmed to still leave headroom.

## 3. Non-VBR auto-capture recaptures unconditionally every LTDC tick (CONFIRMED behavior, payoff needs live verification)

`gnw_h7b0_ltdc_vblank_tick()` (~line 169-243): when `!vbr_active`, the
fallback recomposes the full frame on every vblank tick regardless of
whether layer content actually changed since the last capture — there is
no dirty-tracking. This is exactly the pause-overlay/static-menu case
already profiled this session. A dirty flag (e.g. set on any FB
address/PFC/blend-config register write, cleared after a capture) could
skip full recomposition when nothing changed. Real candidate, but payoff
is scene-dependent (helps static screens, not moving gameplay) — labeled
"needs live verification" for how much of total frame time is actually
spent in these no-op recaptures versus genuine per-frame redraws.

## 4. Static LTDC helper functions not marked `inline` (LOW confidence / likely no-op)

`gnw_h7b0_ltdc_resolve_layer()` and `gnw_h7b0_ltdc_blend_over()` are
`static` but not `inline`. At `-O2`/`-O3`, GCC/Clang auto-inline static
functions called from a single translation unit based on cost heuristics
regardless of the `inline` keyword, so this is very likely already a
no-op in the current release build. Not worth pursuing without a
disassembly check first (out of scope for this pass) — listed only so it
isn't independently "discovered" again as if unverified.

## 5. Generic BQL/MMIO dispatch overhead — architectural, not device-specific (RULED OUT as a targeted fix)

Checked `gnw_h7b0_ltdc_ops`'s `MemoryRegionOps` (`.valid`/`.impl` min/max
access size 1/4) and DMA2D's read/write dispatch (`gnw_h7b0_dma2d.c:600+`)
— both are standard, nothing unusual that would add overhead beyond
QEMU's generic MMIO/BQL architecture. Any win here would require changing
QEMU-wide TCG/BQL behavior (out of scope, high risk, not device-specific).

## 6. `accel/tcg/` generic tuning knobs — no exposed lever found (RULED OUT)

Read `accel/tcg/cpu-exec.c` for real (not assumed) tunables:
- `TCG_MAX_INSNS` (512, `include/exec/translation-block.h:75`) is a
  compile-time constant, not a runtime knob.
- `-icount` machinery (`cpu-exec.c:768,908,923`) only activates its
  per-TB instruction-counting overhead when `icount_enabled()` is true —
  already measured this session as a net throughput loss (~27% more host
  overhead), consistent with the code doing extra bookkeeping
  (`cflags_next_tb`, `insns_left` accounting) on every TB when enabled.
- `mttcg` (`tcg-accel-ops-mttcg.c`) only matters for multi-vCPU systems;
  this machine has a single Cortex-M7 vCPU, so it's structurally
  inapplicable.
- Breakpoint-check slow path (`cpu-exec.c:354`) forces single-instruction
  TBs (`CF_COUNT_MASK` cleared, count set to 1) whenever a breakpoint is
  armed anywhere in the current page — real, but only relevant while
  actively gdb-debugging, not during normal gameplay; not a lever for the
  user's "make it faster to play" goal.

No real, safe, exposed generic-QEMU knob was found. The confirmed wins
are in our own device models (#1 and #2 above), not in QEMU internals.

## Recommendation

Implement #1 first (DMA2D YCbCr fixed-point conversion) — it's the only
candidate with a clear, large, unconditional per-pixel cost and a
straightforward, low-risk fix. Profile again after landing it before
deciding whether #2/#3 are still worth the effort; neither alone is
likely to approach 50% on its own, and none of these are silver
bullets — they reduce real, identified waste, but "50% more performance"
may not be reachable purely through device-model optimization if the
dominant cost turns out to be generic TCG interpretation overhead itself
(which #5/#6 rule out as something we can safely tune).
