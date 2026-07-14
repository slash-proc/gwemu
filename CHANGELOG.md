# Changelog

## 2026-07-14 — IWDG/LPUART1 device models, CRC table-driven perf fix, DWT debug-print removal

- Added real minimal IWDG/LPUART1 device models replacing bare
  `create_unimplemented_device()` stubs, fixing wrong reset values
  (`IWDG_RLR`/`IWDG_WINR` real value `0x00000FFF`, `LPUART1_ISR` real
  value `0x000000C0` — both previously read `0x00000000`) found by the
  sibling `stm32h7b0-diag` suite's real-hardware comparison
  (`347ca514dc`). Also fixed two latent bugs in `scripts/gen_stub.py`
  itself this exposed (outdated `hw/sysbus.h` include path, outdated
  `class_init` signature — both predating this fork's v11.0.2 pin).
- Replaced `hw/misc/gnw_h7b0_crc.c`'s bit-serial (32-iteration-per-word)
  CRC-32 computation with the standard table-driven byte-at-a-time
  algorithm, fixing a ~11x QEMU-vs-hardware wall-clock slowdown the
  diag suite's `crypto_crc32` benchmark found. Verified bit-identical
  across 64,000 random trials (`1d6506baea`).
- Removed an unconditional, un-rate-limited `fprintf()` on *every
  single* DWT register access in `hw/misc/gnw_h7b0_dwt.c` (committed
  since `d6a56195c7`, never noticed) — `DWT_CYCCNT` is the standard ARM
  cycle counter real firmware uses for precise timing measurement, so
  this was a real, previously-unnoticed source of wall-clock inflation
  for exactly the kind of timing-sensitive benchmarks the diag suite
  has been flagging as broadly slower in QEMU (`93d54eb378`). Also
  cleaned up two lower-severity one-shot (not per-access) debug prints
  in `hw/arm/armv7m.c` found in the same sweep; confirmed via a full
  audit that no other `gnw_h7b0_*` device model has an unconditional
  per-access print (the one exception, `gnw_h7b0_gpio.c`'s
  `[gpio-debug]`, is gated on human button-press events, not a hot
  path, and was left alone).
- Investigated (not fixed) `hash_sha256`'s reported ~400x QEMU slowdown:
  confirmed `hw/misc/gnw_h7b0_hash.c` is already properly incremental
  (buffers into a message array, calls `qcrypto_hash_bytes()` once at
  finalize, not a naive per-word recompute) and the crypto backend
  itself is fast in isolation (~2ms for the exact benchmark workload on
  this host) — the remaining gap is most likely generic QEMU MMIO/TCG
  dispatch overhead across many register accesses (compounded by the
  DWT bug above, now fixed) rather than a HASH-specific bug. Not fully
  root-caused; live in-QEMU profiling was attempted but blocked by
  environment process-management flakiness.

## 2026-07-14 — fixed DMA2D R2M and CRC_POL bugs; narrowed LTDC idle-fallback trigger

- Fixed two real device-model bugs reported by the sibling
  `stm32h7b0-diag` correctness-test suite (real-hardware comparison,
  both confirmed against RM0455): DMA2D's R2M fill mode was
  double-converting `OCOLR` (real firmware already pre-packs it into
  the output format, so the emulator's own ARGB8888-assuming encoder
  was converting it a second time); `CRC_POL`'s reset value was
  `0x00000000` instead of the real `0x04C11DB7`, inherited from an
  incorrect `STM32H7B0.svd` reset-value entry (`7f5c5afbe0`).
- Narrowed the LTDC non-VBR idle-fallback (`srcr_idle_ticks`, from the
  GBC-menu black-screen fix) to also require evidence of a genuine
  structural layer-config change (pixel format/layer-enable/window
  geometry, excluding CFBAR which flips every frame during normal
  double buffering) before overriding a still-true `vbr_active`
  (`87168aeef3`) — confirmed live that ordinary in-game frame-skipping
  (the same `frame_integrator` mechanism behind the general stutter)
  also suppresses VBR reloads for the same duration as a genuine
  abandoned transition, so the idle timer alone fired just as often
  during normal stutter as during the real bug, risking mid-draw
  tearing/flicker. **This fix has not yet been re-confirmed against the
  actual reported menu-flicker regression** — it was developed while
  chasing a different, unconfirmed lead (zelda3's attract loop) and still needs
  verification against the real repro.

## 2026-07-14 — fixed two RAM dirty-bitmap bugs found while re-profiling the frame_integrator stutter

- LTDC dirty-bitmap teardown (added alongside `3b946997d8`, meant to
  disable `DIRTY_MEMORY_VGA` logging on the framebuffer once normal
  VBR-paced gameplay resumes) never actually worked: it called
  `framebuffer_update_memory_section(..., 0, 0, 0)`, which internally
  does a zero-size `memory_region_find(root, 0, 0)` whenever the old
  section was bound, silently re-enabling logging on whatever real RAM
  sits at guest address 0 instead of leaving nothing tracked. Fixed by
  tearing down directly (`memory_region_set_log(false)` + `unref` +
  clear, no redundant lookup).
- AXISRAM1/2/3 were declared as three separate `MemoryRegion` objects in
  `hw/arm/gnw_h7b0_soc.c` despite real H7B0 AXI SRAM being one genuinely
  contiguous ~1MB block (RM0455, confirmed zero gap between the three
  subdivisions). Any guest buffer straddling one of those artificial
  boundaries — including the actual framebuffer, which spans
  AXISRAM1/AXISRAM2 — was unreachable via a single `memory_region_find()`
  call, meaning the LTDC non-VBR fallback's RAM dirty-bitmap tracking
  (landed in `343fbc19ed`) had silently failed to bind on every single
  call since it landed, always conservatively defaulting to "assume
  dirty." Fixed by merging the three into one `MemoryRegion` spanning
  the real contiguous range — a more faithful hardware model, not just a
  workaround (`a03ba2854a`).
- Neither bug explains the pause-resume stutter's own CPU cost during
  the burst itself (profiled at >50% of cycles in generic
  `notdirty_write`/CPU-TLB-path machinery specifically during the
  burst) — `tb_flush` and `tlb_flush` call frequency were both directly
  measured and ruled out as the cause. Root mechanism still open; likely
  in `notdirty_write`'s/`physical_memory_is_clean()`'s own dirty-bitmap-
  client interaction, not yet traced to a conclusion.
- A detour investigating a suspected excessive-MPU-write/`tlb_flush`
  theory (Cortex-M `prbar_write`/`prlar_write` in `target/arm/helper.c`
  unconditionally call `tlb_flush()` on every MPU region-config write)
  was ruled out by direct measurement: only ~200 `tlb_flush` calls total
  across the whole session, none during the active burst window.

## 2026-07-14 — fixed GBC-core-to-menu black screen (vbr_active idle-fallback gap)

- Root cause confirmed live: the transition can end on a VBR-type
  `SRCR` reload (not IMR) with no further reloads ever coming, so
  `vbr_active` (only ever reset on IMR) latches true permanently and
  blocks the no-VBR auto-capture fallback forever, freezing on whatever
  frame was active at that last VBR reload.
- Fix: a new `srcr_idle_ticks` counter (reset on every `SRCR` write,
  incremented each vblank tick otherwise) lets the fallback also engage
  once VBR has gone idle for ~130ms, but only combined with the RAM-
  dirty check landed in `343fbc19ed` — the same combination that makes
  this safe where a bare elapsed-time idle guess previously wasn't (a
  stalled game isn't writing new framebuffer content during its stall,
  so it can't spuriously re-arm and reintroduce mid-draw tearing the way
  the earlier reverted timeout attempt did) (`3b946997d8`).
- A detour mid-investigation into a suspected DMA1-Stream0/SAI1-audio
  NVIC interrupt-storm hang was pursued and then retracted: real-time
  breakpoints at `HAL_DMA_IRQHandler`'s entry and return both hit
  cleanly in sequence, proving the handler runs to completion normally.
  The earlier "stuck forever" reading came from naive `halt()`-based PC
  sampling in this gdbstub, which appears to snap to interrupt-vector
  boundaries rather than the true instantaneous PC — not a real hang.
  Not a device-model bug; no code change from that detour.

## 2026-07-14 — landed all four perf-improvement candidates from the research plan

- DMA2D: replaced the YCbCr->RGB conversion's floating-point BT.601
  constants with Q16 fixed-point equivalents (verified bit-identical
  output across the full cb/cr range), and batched per-pixel
  `cpu_physical_memory_read`/`write` calls (and per-pixel CLUT lookups)
  into one read/write per row plus one CLUT load per transfer
  (`160e516579`).
- LTDC: added a `blend_over` fast path for fully-opaque/fully-transparent
  per-pixel-alpha pixels (measured 10.29%->2.40% CPU in one profiling
  scenario), and hoisted the per-pixel horizontal window-clip test
  (WHSTPOS/WHSPPOS, row-invariant) out of the row loop into a
  precomputed per-column array (`279b08904d`).
- LTDC: the non-VBR auto-capture fallback now skips a full frame
  recomposite when nothing that affects pixel output has changed, using
  QEMU's real RAM dirty-bitmap mechanism (`DIRTY_MEMORY_VGA`, the same
  approach `hw/display/vga.c` uses) over Layer1/Layer2's actual
  framebuffer ranges, not a register-write proxy — a first attempt using
  register writes only was rejected mid-review because it would have
  frozen a game still rendering behind a static overlay with no further
  register writes; the RAM-dirty version was live-verified to not affect
  the (separately tracked) GBC-menu black-screen bug below (`343fbc19ed`).
- Confirmed via release-build disassembly that `gnw_h7b0_ltdc_resolve_layer`/
  `gnw_h7b0_ltdc_blend_over` are already fully inlined by GCC at `-O3` —
  no code change needed for that candidate.
- Full research writeup: `docs/session-2026-07-14-perf-improvement-candidates.md`.

## 2026-07-14 — performance-improvement research + new GBC-core-to-menu black-screen bug found

- Dispatched a forked agent to research a code-verified plan for finding
  materially more raw TCG throughput. Full writeup:
  `docs/session-2026-07-14-perf-improvement-candidates.md`. Top finding:
  DMA2D's YCbCr->RGB conversion (`hw/display/gnw_h7b0_dma2d.c:166`) uses
  floating-point math per output pixel on the JPEG cover-art path — a
  fixed-point BT.601 conversion is the highest-confidence, lowest-risk
  win identified. No safe generic QEMU/TCG-level tuning knob was found
  (BQL/MMIO dispatch and `accel/tcg/` both checked and ruled out); `-icount`
  confirmed to cost ~27% more host CPU for no throughput gain (determinism
  feature, not a speed fix). No code changes made yet — plan only.
- Found (not yet fixed) a new black-screen repro: returning to retro-go's
  main menu specifically from the GBC core (Link's Awakening DX)
  intermittently freezes on black. Confirmed via live debug logging that
  this transition never issues an `LTDC_SRCR.IMR` write, so the
  `vbr_active` flag added in `7ac66634e5` never resets, permanently
  disabling the no-VBR auto-capture fallback for this specific
  transition — same symptom as that commit's bug, different trigger its
  fix didn't cover.

## 2026-07-14 — general post-pause stutter: definitive root cause confirmed via direct hardware-vs-QEMU comparison

- Full writeup: `docs/session-2026-07-14-frame-integrator-hw-vs-qemu-comparison.md`.
- Supersedes the 2026-07-13 part 6 SysTick/`-icount` hypothesis below with
  a precise, confirmed mechanism, found by directly comparing identical
  breakpoint-based traces on QEMU and real hardware side by side (no
  resets, attached to an already-paused live repro on both), at the
  user's explicit direction after pushing back on treating this as
  inherent/unfixable.
- Root cause: `game-and-watch-retro-go-sd`'s `Core/Src/porting/common.c`
  (`common_emu_frame_loop()`/`open_pause_menu()`, shared by every core)
  tracks a leaky integrator (`frame_integrator`) of how far behind real
  time the emulated core is, and runs the core 2x per iteration
  (`skip_frames=2`) to pay off a backlog. On real hardware that costs a
  negligible fraction of a real frame at native clock speed, so the
  integrator stays in a small, bounded, spike-free steady-state
  (confirmed live: -2500 to -5000 across 60 samples, zero spikes). Under
  QEMU/TCG, that same catch-up work is measurably slow, and its own
  execution cost inflates the *next* iteration's measured elapsed time —
  feeding back into the integrator and demanding more catch-up, a
  genuine positive feedback loop confirmed live on QEMU (real spikes:
  6445→8112→11279 across three consecutive samples) that's structurally
  impossible on real hardware but forms naturally under TCG.
- Not a QEMU device-model bug. Two candidate real fixes identified, not
  yet implemented: clamping `frame_integrator`'s growth in the firmware
  (lowest-risk, now explicitly in-scope per the user), or `-icount`
  (bigger, not yet confirmed against this specific mechanism in
  isolation).
- Tooling notes: this fork's gdbstub acks `Z1` (hardware breakpoint) set
  requests but they silently never trigger — use `Z0` (software
  breakpoint) instead. `OCDBackend["openocd"]().open()` in default
  `attach` mode does not reset the target, confirmed safe to attach to a
  live, already-running/paused real device mid-session.

## 2026-07-13 (later same day, part 6) — general stutter root cause found: SysTick tick-loss under TCG load; `-icount` scoped as the real fix

- Full writeup: `docs/session-2026-07-13-part5-retro-go-ltdc-vbr-and-stutter-investigation.md`
  (continuation of part 5's investigation).
- Replaced `vbr_ever_used` (permanent latch, committed `014612c688`) with
  `vbr_active` in `hw/display/gnw_h7b0_ltdc.c`/`gnw_h7b0_ltdc.h`: the
  latch fixed the pause-overlay flicker but permanently blacked out
  retro-go's main menu after playing any game (the menu never writes
  `SRCR` again to re-arm the no-VBR auto-capture fallback). `vbr_active`
  resets on every `SRCR.IMR` write instead — real screen/config
  transitions apply their layer config via IMR — rather than a one-way
  latch or an idle-timeout (idle-timeout was tried and reverted: real
  in-game firmware can leave multi-hundred-ms gaps between VBR writes on
  its own, so any timeout short enough to un-stick the menu was also
  short enough to spuriously re-arm the fallback mid-game).
- Found and resolved a second, unrelated black-screen cause: this
  project's flash images are persistent by default, and repeated hard
  `kill`s of QEMU mid-session while games were running had corrupted the
  backing files — fresh copies of `backup/qemu-images/zelda-*.bin` fixed
  it immediately, no code involved.
- Root-caused the general gameplay stutter the user flagged as the real
  priority (previously suspected SMW-specific): PC-sampled both SMW
  (SNES engine) and Link's Awakening DX (gnuboy GBC core) mid-stutter;
  both showed their hottest code in their own independent
  audio-rendering function, confirming a shared cause rather than a
  per-game one. Ruled out (via live tracing) a dynamic DMAMUX-based
  DMA/SAI1 rebind mechanism added in `77e86eacdd` for stock-Zelda/OFW
  compatibility as a suspect. Found the actual mechanism: SysTick's
  pending-IRQ state is a single bit, not a counter, so under QEMU/TCG (where
  `SysTick_Handler` takes far longer in real wall-clock time than on real
  hardware) a tick that fires while the CPU is still busy with the
  previous one is silently lost, regardless of how correctly
  `hw/core/ptimer.c` schedules its own deadlines. This corrects a specific
  claim in `docs/session-2026-07-12-breakpoint-lockstep-tracing.md` part
  22 (which attributed the gap to a ptimer catch-up policy bug — see the
  correction note added there) while keeping that doc's empirical
  host-throughput-correlation finding intact.
- Decision, agreed with the user: fixing this for real needs `-icount`
  (deterministic instruction-scaled virtual time), not another
  device-model patch. Scoped as a separate, larger follow-up effort
  rather than folded into this session, since it's a global timing-model
  change needing re-validation of `gnw_h7b0_dma.c`'s accumulator-scheduled
  timers, LTDC's vblank period, and SAI1 audio pacing.

## 2026-07-13 (later same day, part 5) — retro-go pause-overlay flicker/menu black-screen fixes, SMW stutter root-caused (not fixed here)

- Full session writeup: `docs/session-2026-07-13-part5-retro-go-ltdc-vbr-and-stutter-investigation.md`.
- Fixed a real tearing regression in `hw/display/gnw_h7b0_ltdc.c`'s
  no-VBR auto-capture fallback (`gnw_h7b0_ltdc_vblank_tick()`): it fired
  on any vblank with `content_dirty` false, unable to tell "firmware that
  never uses VBR" apart from "firmware using VBR that just landed
  between two writes" (the latter grabbed a mid-draw frame). First fix
  (committed `014612c688`): a `vbr_ever_used` latch gating the fallback
  off once any `SRCR.VBR` write happens.
- Found that latch then permanently blacks out retro-go's main menu
  after playing any game (the menu paints pixels directly without ever
  writing `SRCR` again to re-arm the fallback). Replaced the permanent
  latch with `vbr_active`, reset on every `SRCR.IMR` write (real
  screen/config transitions use IMR) instead of a `vbr_ever_used`
  one-way latch or an idle-timeout (idle-timeout was tried and reverted:
  real in-game firmware can leave multi-hundred-ms gaps between VBR
  writes on its own, so any timeout short enough to un-stick the menu
  was also short enough to spuriously re-arm the fallback mid-game).
- Root-caused (not fixed here — belongs in the `game-and-watch-retro-go-sd`
  sibling repo) a real stutter during SMW gameplay to
  `external/smw/src/common_rtl.c`'s `RtlSetUploadingApu()` forcing a
  synchronous 10,000-emulated-cycle APU catchup burst on every SPC
  sound-driver reupload — negligible on real hardware, hundreds of ms
  under QEMU's TCG interpretation. Whether this generalizes to a shared
  qemu-gnw-side cause behind other cores' stutter (vs. SMW-specific) is
  still open per the user's own correct pushback — not yet confirmed
  either way.
- A separate, unrelated black-screen cause (corrupted persistent
  `backup/qemu-images/zelda-*.bin` state from repeated hard `kill`s of
  QEMU mid-session) was found and resolved with fresh image copies — no
  code involved; a reminder that this project's flash images are
  persistent by default and abrupt kills mid-write are a real corruption
  risk.

## 2026-07-13 (later same day, part 3) — gnw-web-builder integration: real HASH/erase device-model bugs, gdbstub live memory access, persistent flash images

- Full session writeup: `docs/session-2026-07-13-web-builder-integration-fixes.md`.
- Added a real STM32H7B0 HASH peripheral device model
  (`hw/misc/gnw_h7b0_hash.c`, MD5/SHA-1/SHA-224/SHA-256 backed by QEMU's
  own `crypto/hash.h`), replacing `create_unimplemented_device("HASH",
  ...)`. gnwmanager's RAM stub calls `HAL_HASHEx_SHA256_Start(...,
  HAL_MAX_DELAY)` after every flash write to verify it; the stub always
  reading 0 meant the digest-complete flag never set, permanently
  wedging every internal-flash write.
- Fixed `hw/misc/gnw_h7b0_flash_r.c`: internal-flash erase
  (`FLASH_CR1`/`CR2`'s `SER`/`BER`+`START` bits) was a pure register stub
  with no connection to the actual flash memory — erasing finished
  instantly and silently changed nothing. Now wired to the real
  `flash_bank1`/`flash_bank2` memory (`gnw_h7b0_flash_r_set_banks()`,
  called from `gnw_h7b0_soc.c`), so a sector/bank erase actually
  `memset`s the real backing memory to `0xFF`.
- `gdbstub/gdbstub.c`: memory read/write (`m`/`M`) packets no longer halt
  the VM. Upstream's blanket "any byte while running halts the target"
  rule was a protocol default, not a real requirement —
  `cpu_memory_rw_debug()` is exactly as safe to call from a running VM as
  a halted one. Every other command (registers, continue/step,
  breakpoints, etc.) still halts first, unchanged.
- Added optional persistent flash-image backing: `gnw_h7b0_soc.c` gained
  `bank1-image`/`bank2-image`/`extflash-image` string properties
  (`-global gnw-h7b0-soc.<name>=<path>`) that back that region directly
  with the named file (`memory_region_init_ram_from_file`, `RAM_SHARED`)
  instead of anonymous RAM seeded once via `-device loader` — guest
  writes (flashing, erasing) now persist to the file live. This is now
  `scripts/boot_qemu.sh`'s default (`--ephemeral` opts back out to the
  old discard-on-exit behavior) since it matches how real hardware
  actually behaves.
- New tool: `scripts/gdb_tap.py`, a logging TCP proxy for QEMU's GDB RSP
  port, with `TCP_NODELAY`/re-armed `TCP_QUICKACK` (fixes a ~40ms Nagle/
  delayed-ACK stall per request) and single-active-client preemption
  (QEMU's gdbstub only serves one client; a stale browser-tab connection
  no longer starves out a one-shot `gnwmanager --qemu` invocation).
- Cross-repo fixes found via the above (not in this repo, noted for
  context): `gnw-web-builder`'s mailbox status-poll interval (10ms ->
  150ms, was starving the guest CPU against the gdbstub's old
  resume-debounce), `backend/src/server.ts`'s QEMU-bridge socket
  (`TCP_NODELAY`), `qemuTransport.ts` (memory ops no longer pre-emptively
  halt), and `gnwmanager/ocdbackend/gdb_backend.py`'s socket
  (`TCP_NODELAY`, ~12x speedup on `gnwmanager --qemu info`).

## 2026-07-13 (later same day, part 2) — Fixed real per-pixel MMIO overhead in LTDC Layer2 and DMA2D, cutting effective slowdown roughly in half

- Investigated Zelda CFW's own reported ~50% real-hardware speed
  slowdown (separate from the Mario CSI/PLL1 fix below). Found LTDC's
  Layer2 compositing path (`hw/display/gnw_h7b0_ltdc.c`) and DMA2D's
  `M2M_PFC`/`M2M_BLEND*` transfer modes (`hw/display/gnw_h7b0_dma2d.c`)
  both issued a full `cpu_physical_memory_read()`/`_write()` guest-memory-
  translation call *per pixel* instead of batching a row at a time (JPEG
  cover-art's YCbCr decode did 3 such calls per pixel; DMA2D's L8 CLUT
  lookup re-fetched the same 256-entry table from guest memory once per
  pixel too). `perf record` on the live QEMU process confirmed this
  address-translation machinery (`phys_page_find`/`flatview_*`/
  `address_space_translate_internal`) was ~30% of total process CPU time.
  Batched both to fetch/write one row per `cpu_physical_memory_read()`/
  `_write()` call (CLUT loaded once per DMA2D transfer instead of once per
  pixel); confirmed via `perf` that this overhead is now gone from the hot
  path. Measured effect via SysTick-fire-rate counting (Zelda CFW):
  ~415Hz -> ~643Hz (real hardware is 1000Hz) — real, substantial, but not
  a full fix; remaining gap is genuine QEMU TCG instruction-interpretation
  cost, not further addressable at the device-model level. Also removed a
  stray capped-but-still-leftover debug `fprintf` in
  `hw/misc/gnw_h7b0_dma.c`'s stream-tick handler. Full investigation in
  `docs/session-2026-07-12-breakpoint-lockstep-tracing.md` parts 20-23.

## 2026-07-13 (later same day) — Fixed: RCC_CR never mirrored CSION into CSIRDY, blocking Mario CFW's PLL1 overclock

- User reported Mario CFW running at roughly half real-hardware speed;
  confirmed via direct real-hardware register comparison (`gnwmanager`'s
  `OpenOCDBackend`) that QEMU's CPU was permanently stuck on HSI (64MHz)
  while real hardware runs the identical CFW image on PLL1. Root cause:
  `hw/misc/gnw_h7b0_rcc.c`'s `RCC_CR` write handler mirrored every other
  oscillator's `*ON` bit into its `*RDY` bit (HSI, HSE, PLL1/2/3, and
  `RCC_CSR`'s LSI) but had no case for CSI at all —
  `RCC_CR_CSION`/`RCC_CR_CSIRDY` weren't even defined. Firmware's
  `SystemClock_Config()` requests CSI ON as part of the same
  `HAL_RCC_OscConfig()`-style call that also configures PLL1; with
  `CSIRDY` never reachable, the oscillator-config sequence never
  reliably completed, and PLL1 never locked. Added the missing bit
  definitions (`include/hw/misc/gnw_h7b0_rcc.h`) and mirror logic
  (`hw/misc/gnw_h7b0_rcc.c`), matching the existing pattern for every
  other oscillator. User-confirmed real effect: every clock register
  (`PLLCKSELR`, `PLL1DIVR`, `CDCFGR1`, `CR`, `CFGR`) now matches real
  hardware bit-for-bit after boot, where before QEMU was permanently
  stuck on HSI. **A residual ~2x-slow tick rate remains despite every
  register now matching** — narrowed to QEMU's own internal clock-
  propagation/timing code (not firmware- or register-visible), not yet
  found. See `docs/session-2026-07-12-breakpoint-lockstep-tracing.md`
  part 20 for the full investigation and next-session starting point.

## 2026-07-13 — Fixed: two real LTDC bugs (AL44 unimplemented + Layer2/Layer1 compositing order backwards), resolving GAME/PAUSE menu invisibility

- Root cause of the Mario (and likely Zelda) GAME/PAUSE submenu bug
  from 2026-07-12 part 15, fully resolved and user-confirmed live.
  Button input, the menu's internal state machine, and the render
  dispatch were all working correctly the whole time (extensively
  re-verified this session) — the actual gap was entirely in QEMU's
  LTDC compositor, `hw/display/gnw_h7b0_ltdc.c`, and turned out to be
  two separate bugs stacked on top of each other:
  1. LTDC pixel format `6` (AL44 — 4-bit alpha + 4-bit luminance, used
     for anti-aliased overlay text) had no case in
     `gnw_h7b0_ltdc_capture_rows()`'s Layer2 bpp switch, so it fell
     through to `l2_bpp = 0` and silently skipped Layer2 compositing
     entirely even though the layer was enabled and had real content.
     Fixed with real AL44 decoding: alpha nibble applied directly,
     luminance nibble indexes a 16-entry CLUT sub-palette at `n*17`
     (confirmed against `sdk/stm32h7xx-hal-driver`'s
     `HAL_LTDC_ConfigCLUT()` AL44 branch and live-observed `L2CLUTWR`
     write patterns — firmware only ever loads the 16 diagonal
     entries, never a flat 256-entry table, ruling out an earlier
     attempt that treated AL44 like L8).
  2. Layer compositing order was backwards: Layer2 was composited
     first/bottom, Layer1 second/top — but real STM32 LTDC hardware
     always shows Layer2 *above* Layer1. Layer1 (RGB565, no alpha
     channel, always fully opaque) was therefore completely hiding
     Layer2's overlay content even after AL44 decoding was fixed
     correctly (confirmed via pixel-level tracing that Layer2 was
     computing real, correct values the whole time). Fixed by
     compositing Layer1 onto the background first, Layer2 onto that
     result second.
  Added `LTDC_PF_AL44` to `include/hw/display/gnw_h7b0_ltdc.h`. See
  `docs/session-2026-07-12-breakpoint-lockstep-tracing.md` part 19 for
  the full investigation, including two real tooling lessons from this
  session (QEMU's gdbstub halts input-event delivery, not just the
  vCPU -- reading peripheral state while halted can show stale
  pre-input values; and a full, non-`-noanalysis` Ghidra pass is
  needed for reliable RAM-address xref tracing).

## 2026-07-12 (later session, part 15) — Mario GAME/PAUSE submenu bug investigated, not yet fixed

- User-reported: on Mario CFW, GAME/PAUSE do nothing from the clock
  face's main loop (POWER wake and TIME both work fine; GAME *does*
  correctly wake the device from its screensaver, confirming the raw
  input path is fine). Extensive investigation ruled out: the GPIO/
  EXTI/`read_buttons()` chain (confirmed correct), the GAME+LEFT
  retro-go-jump combo (correctly declines given our placeholder,
  content-free `mario-bank2.bin`), the per-frame dispatch gate at
  `FUN_0801056c` (`[ctx+0x770]`/`[ctx+0x772]` already satisfied on
  QEMU), and LTDC Layer 2 being undrawn (it's actively populated with
  real content on QEMU). Root cause not yet found — full findings and
  concrete next steps in session doc part 15. No code changes this
  part; also documented a recurring stale-GDBBackend-connection
  tooling gotcha that produced one false-negative capture this
  session.

## 2026-07-12 (later session, part 14) — real CRYP (AES-GCM) device model; Mario boots

- New device model `hw/misc/gnw_h7b0_cryp.c` + header: real AES-128/
  192/256 (own key schedule + cipher, no external crypto lib) and a
  spec-correct AES-GCM engine (GHASH, CTR keystream, INIT/HEADER/
  PAYLOAD/FINAL phase state machine) matching the real hardware
  register protocol, wired to CRYP_IRQn=79 at 0x48021000. Also
  implements ECB/CBC/CTR (ready for future use, not yet exercised by
  any traced boot path). Root cause: Mario's CFW boot performs a real
  AES-GCM decrypt (integrity-checked blob) from CRYP's own ISR and
  sleeps until it completes; CRYP was `create_unimplemented_device`
  (silent stub, never interrupts), so the ISR never ran and boot hung
  forever. User-confirmed: Mario now boots past this point.
- Found via the systematic stuck-PC -> caller -> divergence method
  (see session doc part 14) after a longer, less disciplined detour
  earlier in the session — noted there as a process lesson.

## 2026-07-12 (later session, part 14) — LTDC display frozen after dynamic framebuffer reconfiguration

- Fixed hw/display/gnw_h7b0_ltdc.c: dynamic LTDC framebuffer/format
  changes (e.g. retro-go's lcd_setup_framebuffers() RGB565↔LUT8 switch
  via HAL_LTDC_SetPixelFormat() + HAL_LTDC_Reload(VBR)) no longer leave
  the host display stuck on the last pre-change frame. Three
  interacting gaps: (1) SRCR.IMR reloads updated the active register
  set but never triggered a host capture (only VBR-write-time capture
  existed); (2) when a SRCR.VBR write was skipped because content_dirty
  was still true, the deferred vblank reload applied the new shadow
  registers but also never captured, permanently blocking further
  captures via the !content_dirty gate; (3) gnw_h7b0_ltdc_enabled()
  consulted shadow regs[] instead of the active set actually used for
  scanout. Fix: capture immediately after IMR reload, add
  vbr_deferred_capture to capture right after the vblank reload when
  the VBR-write capture was skipped, use active_* for enable checks,
  and clear content_dirty on gfx_update() early-return paths so the
  capture pipeline cannot wedge.

## 2026-07-12 (later session, part 13) — OFW input + audio + display transitions working in QEMU

- Buttons now raise real EXTI interrupts (SYSCFG EXTICR-muxed); TIME
  dual-wired PC5+PA2 (stock reads PA2/WKUP2 in default mode); keyboard
  map now configurable via `-global gnw-h7b0-gpio.keymap=...`.
- SAI1's DMA binding resolved at runtime from DMAMUX request 87 (was
  hardcoded to retro-go's DMA1 Stream0; stock uses DMA2 Stream6).
- DMA double-buffer mode (SxCR.DBM) modeled (CT toggle, EN stays set) —
  stock's audio engine streams via DBM; was killed after one buffer.
- DMA2D IRQ line now honors all six ISR flags (CTCIF etc.) — stock's
  palette-fade transitions sleep on CLUT-transfer-complete.
- SD SPI-mode fix (colleague report): ACMD41 now → transfer state once
  powered up; CMD17/CMD55 no longer rejected without a prior CSD read.
- SAI NODIV=1 rate decoding fixed (stock: PLL2P 12.288MHz / MCKDIV 4 /
  64-slot frames = 48kHz; was decoded as 12kHz through the NODIV=0
  formula, running audio AND all audio-paced firmware at 1/4 speed —
  also the real cause of "buttons don't react").
- Removed the vestigial mouse-button input mapping (clicks/wheel were
  silently pressing A/START/d-pad; a swallowed release latched START
  low forever, freezing OFW menu input — the "stops responding after
  the menu" bug).
- Result: interactive OFW fully working in QEMU — POWER wake, TIME
  transitions, correct-pitch audio, responsive input (user-confirmed).

## 2026-07-12 (later session, parts 11-12) — ZELDA BOOTS TO VISIBLE DISPLAY in QEMU

- The physical device's exact image pair (repo-root `zelda-patched.7z`:
  gnwmanager-CFW bank1 + patched 4MB extflash) is now what
  `boot_qemu.sh --patched` boots — bank1 verified byte-for-byte
  identical to live device flash over SWD. The pivot to the full CFW
  was an intentional project decision that had been lost across
  session summaries; QEMU had been booting a stock+2-byte-patch image
  with the wrong (stock, 64MB) extflash. All tracing scripts'
  entry-point source repointed to the patched bank1 (CFW replaces the
  reset vector).
- With the correct image pair plus this session's OSPI-auto-polling
  and OTFDEC fixes, **QEMU boots Zelda CFW to visible display output**
  (user-confirmed on screen; `LTDC_GCR.LTDCEN=1`, steady-state PC
  matches real hardware's). Remaining follow-ups (real OTFDEC AES-CTR
  for genuinely-stock encrypted extflash, true-stock boot's WFI wait,
  audio/input/gameplay verification) recorded in the session doc's
  part 12.

## 2026-07-12 (later session, parts 7-10) — RSTEN trap root-caused and fixed (OSPI auto-polling), OTFDEC device model added, two big methodology corrections

Full narrative: `docs/session-2026-07-12-breakpoint-lockstep-tracing.md`
(parts 7 through 10).

- **Real fix (root cause of the part-6 self-trap)**: OSPI automatic
  status-polling (CR.FMODE==2) was completely unmodeled, so `SR.SMF`
  never set and stock Zelda's `HAL_OSPI_AutoPolling()` wait timed out
  into firmware's own `b .` error trap at `0x080164b8`. Implemented in
  `hw/misc/gnw_h7b0_ospi.c`/`.h` (`ospi_autopoll_evaluate()`: PSMKR/
  PSMAR match, AND/OR per CR.PMM, evaluated at command trigger).
- **Real fix (the next trap after that)**: new minimal OTFDEC device
  model (`hw/misc/gnw_h7b0_otfdec.c` + header, both instances wired at
  `0x5200b800`/`0x5200bc00`), implementing the key-CRC readback
  (`CONFIGR.KEYCRC`, exact `HAL_OTFDEC_KeyCRCComputation()` algorithm)
  that `HAL_OTFDEC_RegionSetKey()` verifies. Actual AES-CTR decryption
  of memory-mapped reads is NOT yet modeled — stock extflash is
  encrypted, so data (not control flow) read through OTFDEC regions is
  still wrong on QEMU; flagged as the known next gap.
- QEMU stock-Zelda boot now clears the whole OSPI/OTFDEC init sequence
  and parks in a legitimate `while (!flag) WFI;` wait (`0x0800e5a8`,
  flag `0x2000ad40`) for a not-yet-identified IRQ — no longer in any
  error path.
- **Methodology correction #1 (parts 7-9)**: the parts-6-8 "real
  hardware has code-like bytes at address 0, QEMU has zeroes" thread
  was a testing artifact — stale SRAM from a previous boot surviving
  SWD/`nSRST` resets (only a power cycle clears it), not a QEMU bug.
  Also documented: OpenOCD `wp` silently fails to register >4KB
  watchpoints on this board's `hla_target` (false-negative timeouts).
- **Methodology correction #2 (part 10)**: the physical device is
  running gnwmanager's FULL CFW patch set (reset vector replaced,
  OTFDEC disabled, save-crypto skips, ~100 flash diff ranges vs stock),
  not our 2-byte standby patch — real hardware is no longer a valid
  stock-behavior reference for OTFDEC/bootloader/save-crypto regions or
  anything above `0x1B3E0`. Check `gnw_patch/zelda.py`'s patch list
  before trusting any hardware trace in a given region.

## 2026-07-12 (later session, part 6) — real OCTOSPI IRQ modeled; firmware self-trap found, root cause still open

Full narrative: `docs/session-2026-07-12-breakpoint-lockstep-tracing.md`
("Follow-up session (same day, part 6)" section).

- **Real fix**: `hw/misc/gnw_h7b0_ospi.c`/`.h` gained a real, level-
  sensitive IRQ line for OCTOSPI1/2 (`ospi_update_irq()`, gated by CR's
  `TEIE`/`TCIE`/`FTIE`/`SMIE`/`TOIE` against SR, re-evaluated on every
  SR/CR-affecting write) — neither instance had any IRQ modeled before
  this. Wired to NVIC in `hw/arm/gnw_h7b0_soc.c`
  (`OCTOSPI1_IRQn=92`/`OCTOSPI2_IRQn=150`, per
  `sdk/cmsis-device-h7/Include/stm32h7b0xx.h`), which also required
  bumping `armv7m`'s `num-irq` property `96`→`160` (NVIC sizes must be
  multiples of 32; 150 didn't fit in 96, tripped an assertion on boot).
- Found (not yet fixed): stock Zelda firmware deliberately traps itself
  in an infinite `b .` loop at `0x080164b8` on a HAL-style error
  return, tracing back to an OCTOSPI1 RSTEN-command helper
  (`FUN_0800e45c`/`FUN_080112b6`). The OCTOSPI IRQ fix above didn't
  resolve it — every core register matches between QEMU and real
  hardware at the exact failing checkpoint except one register's
  dereferenced value, which reads real code-like bytes at address
  `0x0` on real hardware but all-zeroes on QEMU. Root cause still
  open; leading theory is a firmware ITCM-populating copy loop that
  doesn't run identically (or at all) on QEMU. See the doc for the
  full trace and concrete next steps.
- A same-session attempt to fix the address-`0x0` divergence by
  aliasing flash bank 1 there was **wrong and reverted** — the SoC
  already has a legitimate ITCM region at that address
  (`ITCM_BASE_ADDRESS = 0x00000000`), and the new alias just shadowed
  it instead of fixing anything.
- New tooling: `scripts/probe_11536_state.py` (arbitrary-register +
  dereferenced-pointer + NVIC/peripheral-state comparison at a single
  checkpoint on both targets) and `scripts/resume_and_sample.py`
  (resume + repeated halt-and-sample-PC, to tell a genuine CPU stall
  apart from a breakpoint-detection artifact).

## 2026-07-12 (later session, part 5) — real ADC battery-threshold bug found and fixed via lockstep tracing

Full narrative: `docs/session-2026-07-12-breakpoint-lockstep-tracing.md`
("Follow-up session (same day, part 5)" section, which also documents
the efficient checkpoint-bisection playbook used to find this).

- **Real fix**: `GNW_H7B0_ADC_FULL_BATTERY_RAW` (`include/hw/misc/gnw_h7b0_adc.h`)
  bumped `13500` -> `0xFFFF`. The old value only cleared one of stock
  Zelda's own 4 battery-level threshold tables (`FUN_0800320e` in a
  Ghidra decompile, thresholds up to ~41974) -- QEMU read battery level
  0 where real hardware reads a real nonzero level, silently skipping an
  entire boot-progress branch. Checkpoint-confirmed: `FUN_0800ec7a` now
  takes the same internal branch as real hardware, and the per-pass
  event-queue dispatch now fires identically on both targets, neither of
  which matched before this fix.
- Also fixed same day (part 4, folded in here since it's the same
  investigation thread): `hw/misc/gnw_h7b0_gpio.c` no longer forces
  GPIOC bit 8/13 or GPIOD bit 0 low at reset -- confirmed via direct
  real-hardware register reads that all read `0xFFFFFFFF`. This
  unblocked QEMU reaching the LTDC display-init entry point and a
  layer-window register write, matching real hardware at both, which it
  never did before.
- `scripts/boot_qemu.sh` gained `-audiodev pa,id=snd0` +
  `-global gnw-h7b0-sai1.audiodev=snd0` (QEMU had no audio backend
  configured at all) and a `--patched` flag to boot the
  standby-patched bank1 image for a fair comparison against patched real
  hardware.
- New `scripts/make_zelda_patched_bank1.py` (reproduces gnwmanager's
  standby-skip patch bytes locally) and `scripts/watch_state_byte.py`.
- Still open: real hardware's route to the LTDC HAL init (`0x08013704`)
  isn't the event-queue path just fixed -- a different, not-yet-found
  call to `FUN_0800eb90(3)` is the actual trigger. No display/audio on
  QEMU yet.

## 2026-07-12 (later session, part 2) — breakpoint-based lockstep tracing

Full narrative: `docs/session-2026-07-12-breakpoint-lockstep-tracing.md`.

- New `scripts/checkpoint.py`, `step_init_calls.py`, `watch_loop_flag.py`,
  `watch_write.py`, `lockstep_compare.py`: breakpoint-based (not
  single-step-based -- too slow over real hardware's SWD link)
  QEMU-vs-real-hardware live execution comparison at chosen checkpoints,
  cross-referenced against Ghidra decompilation.
- Confirmed QEMU tracks real hardware **bit-for-bit identically** from
  the real reset entry point through the constructor/init-array
  dispatcher, MPU/cache setup, ~20 subsystem inits, and into the main
  superloop's first 20 passes -- the previously-documented "counter
  never arms" blocker (`[r4+9]` exit flag, `[r4+0x60]` enable field both
  stay 0) is reproduced exactly on QEMU with this new scripted,
  repeatable checkpoint. Real-hardware confirmation at the same exact
  point is the next session's first task.
- Root-caused repeated real-hardware SWD disconnects
  ("OpenOCD lost contact ... CPU likely entered low-power/standby") to
  stock Zelda firmware's own state-6 standby handler triggering during
  the repeated resets this kind of tracing requires -- not a tooling
  bug. Decision: use `gnwmanager`'s patched-out-standby Zelda blob for
  future interactive real-hardware breakpoint-tracing sessions
  specifically (boot-behavior-accuracy work still uses the real stock
  dump). QEMU has no standby/low-power mode modeled yet -- flagged as
  necessary future work, not yet started.
- Found and fixed several real bugs in this session's *own tooling*
  (not firmware bugs): a missing breakpoint step-over before `continue`
  that made QEMU look completely stuck re-executing the same call
  forever (a pure tooling artifact -- see the doc's "Real, load-bearing
  bugs found and fixed in this session's own tooling" section for the
  full list, including a breakpoints-halt-before-not-after off-by-one,
  a Python socket timeout wedge, and stdout buffering hiding live
  progress).
- Reinforced: do not edit the `gnwmanager` package to add capabilities
  (reverted an earlier attempt this session) -- it has independent
  concurrent development happening outside this repo; write pure-
  consumer scripts against its existing public API instead.

## 2026-07-12 (later session) — register snapshot/diff tooling, 3 real reset-default fixes

Full narrative: `docs/session-2026-07-12-register-snapshot-diffing.md`.

- New `scripts/snapshot_registers.py` + `diff_snapshots.py` +
  `triage_diffs.py`: dump every SVD peripheral's register block from
  QEMU or real hardware (via `gnwmanager`'s backend abstraction,
  including its new `--qemu` gdbstub support) and diff/auto-classify
  against the SVD's own documented reset values.
- New `scripts/halt_at_entry.py`: deterministic reset-and-halt-at-the-
  real-entry-point via a real breakpoint (gdb-remote `Z1` for QEMU,
  OpenOCD `bp`/`wait_halt` for real hardware), replacing the racy plain
  `reset_and_halt()`/`reset halt` for cases needing true pre-firmware POR
  state. Built entirely on `gnwmanager`'s existing public backend API, no
  changes to the `gnwmanager` package.
- **Fixed 3 real reset-value bugs**, all the same class (a write-only
  "pulse" register whose write handler had no special case, so the
  generic mask-and-store path leaked the last-written value into
  readback instead of the real always-reads-0 behavior):
  `hw/misc/gnw_h7b0_gpio.c` (`GPIOx_BSRR`), `hw/misc/gnw_h7b0_rtc.c`
  (`RTC_WPR`), `hw/misc/gnw_h7b0_tim1.c` (`TIM1_EGR`).
- `hw/arm/gnw_h7b0_soc.c`: extended `create_unimplemented_device`
  coverage from 2 to 69 peripherals (every SVD peripheral not covered by
  a real device model), so a full-address-space register sweep (or any
  future gnwmanager/firmware probe) logs instead of BusFaulting.
- New `scripts/make_boot_images.py` + `boot_qemu.sh`: standardized,
  correctly-sized (0xFF-padded to real `FLASH_BANK_SIZE`/`EXTFLASH_SIZE`)
  bank1/bank2/extflash boot images and the one launch command going
  forward, replacing ad hoc partial-dump QEMU invocations. Deliberately
  omits `-d guest_errors,unimp`/other unbounded logging flags — that
  combination filled `/tmp` and destabilized the host multiple times
  this session.
- Confirmed (not a bug): RCC's apparent reset-default "anomalies"
  (mirrored `ENR`/`LPENR` register block at a constant `-0x60` offset,
  non-zero trim/backup-domain registers) are real hardware behavior —
  factory calibration trim loaded by hardware itself, backup-domain state
  that legitimately persists across a warm reset by design, and an
  undocumented address-decode aliasing quirk present even at a guaranteed
  pre-firmware halt. None are fixable or worth modeling.

## 2026-07-12 — stock firmware boot investigation, PA0/WKUP1 fix

Full narrative: `docs/session-2026-07-12-stock-firmware-boot-investigation.md`.

- **Fixed `hw/misc/gnw_h7b0_gpio.c`**: stopped forcing PA0/WKUP1 low at
  GPIO reset. It was previously forced low to work around an unrelated
  early-boot write-storm hang from before `RCC_RSR.SFTRSTF` was fixed.
  Real disassembly, cross-checked against `gnwmanager`'s
  `gnwmanager/cli/gnw_patch/{mario,zelda}.py` (real, SHA1-hash-verified
  stock-firmware patch offsets, not a theory), shows this pin gates a
  "state-6 standby" handler (both patch files literally label it that,
  as part of a "warm-boot power-off fix": Mario `0x08005EF4`, Zelda
  `0x0800EA8C`) that firmware expects to read high (button not held) to
  do its real display-init work. With `RCC_RSR.SFTRSTF` already fixed,
  releasing PA0 no longer triggers the old storm — confirmed real forward
  progress on both Mario and Zelda with zero live pokes (genuine
  `SPI2->TXDR` traffic matching the known LCD panel bring-up command
  sequence).
- New sibling decompilation projects: `~/Nerd/git/gnw-mario-decomp` and
  `~/Nerd/git/gnw-zelda-decomp`, same Ghidra script toolkit in both.
- Found but not yet fixed: both games' main superloop only exits a
  timeout-gated wait once a counter (Zelda: `FUN_0800edfc`, threshold 39)
  crosses a threshold, but the counter never starts because its enable
  field (`[r4+0x60]`) is never written by anything — confirmed via a live
  hardware watchpoint over ~12s of real execution. Next session should
  pick up from here.
- Workflow: confirmed QEMU's SD model only requires power-of-2 sizing for
  cards ≤2GiB; above that (the user's real ~7.4GiB `sdcard.img`) only
  512K alignment is required — no qcow2 overlay needed, plain
  `-drive if=sd,format=raw,file=sdcard.img` works directly.

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
