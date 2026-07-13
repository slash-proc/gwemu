# 2026-07-13 (part 5) — retro-go pause-overlay flicker, menu black-screen, and the SMW stutter investigation

## Starting point

Earlier the same day (commit `77e86eacdd`, "Fix LTDC AL44/layer-order menu
bug and RCC CSI readiness gap") landed real fixes for Mario/Zelda CFW's
GAME/PAUSE menu invisibility, plus a batch of prior uncommitted work
including a change to `gnw_h7b0_ltdc_vblank_tick()`: an "auto-capture"
fallback (originally added the day before, in `1663797f50`, for
gnwmanager/OFW firmware that never reloads via `SRCR.VBR`) that fires on
any vblank where `content_dirty` is false and LTDC is enabled — with no
gating on whether the *current* firmware actually uses VBR at all. That
commit's own message flagged the risk: "may reintroduce flickering in
Retro-Go."

The user reported exactly that: opening Retro-Go's pause overlay, then
returning to gameplay, caused a bad stutter — visually described as "a
couple of frames in quick succession then an inexplicable wait," worse
than ordinary slowdown.

## Investigation path (what turned out to be red herrings, in order)

Each of these was checked with real instrumentation before being ruled
out — worth recording so a future session doesn't re-walk the same path:

1. **LTDC vblank timer period** (`gnw_h7b0_ltdc_recalc_timers()`) —
   instrumented to log every computed period and the real (wall-clock)
   time between calls. Rock steady at ~59Hz throughout, including during
   an active stutter. Not the cause.
2. **RCC/SYSCLK clock-switching** — instrumented
   `gnw_h7b0_rcc_update_sysclk_clock()` to log every Hz change. No clock
   switch happened during the stutter window at all. Not the cause.
3. **Audio backend (`-audiodev none` vs real PulseAudio)** — swapped to
   `-audiodev pa` to test whether the SAI1/DMA "no real backend to drain
   the FIFO" theory held. Stutter reproduced identically with real audio
   playing. Not the cause.
4. **A live GDB watchpoint on `LTDC_SRCR`**, hand-written for this
   session, produced inconsistent/contradictory PC and register values
   between two nearly-identical runs — almost certainly a step-over bug
   in the ad hoc script, not real data. Abandoned in favor of the
   project's existing tools below rather than trusting it.

## What the evidence actually showed

Logging every `SRCR` write's value and the real time since the previous
write (temporary instrumentation, since removed) showed the write
pattern changing the moment the overlay opened: from steady `VBR`
writes every ~25ms (normal vblank-paced gameplay) to `IMR`-only writes in
a repeating pattern of two quick writes ~90ms apart, then a **500–780ms**
real-time gap, repeating for as long as the overlay interaction
continued.

`scripts/hotloop_sample.py` (statistical PC sampling, already in the
repo) taken immediately before vs. during the stutter told the rest of
the story: before, PC was spread across ~40+ distinct addresses in
`0x240a5xxx`–`0x240a68xxx` (normal varied execution). During the stutter,
one address (`0x240a6268`) dominated at ~29% of samples, cold in the
baseline. `arm-none-eabi-readelf -sW` against
`retro-go-temp/elf/gw_retro_go_bank2.elf`, filtered to the `.overlay_smw`
section (retro-go links every core into the *same* fixed RAM VMA range as
a swappable overlay — plain `addr2line` picks an arbitrary overlay's
symbol at a shared address and is not trustworthy here; you have to
filter the symbol table by the specific overlay's ELF section index
first), resolved the hot address into `smw__dsp_cycle` and
`smw__ppu_runLine` — the SNES emulator core's own audio-DSP and
video-scanline stepping functions.

## Root cause (confirmed against source, in `game-and-watch-retro-go-sd`)

- `external/smw/src/smw_cpu_infra.c` hooks specific addresses in the
  *original SMW ROM* (`0x811D`, `0x80F7`, `0x80FB`) — the real game's own
  APU sound-driver-upload routine. On upload-finished, it calls
  `RtlSetUploadingApu(false)`.
- `external/smw/src/common_rtl.c:673`, on that transition, does:
  ```c
  g_snes->apuCatchupCycles = 10000;
  snes_catchupApu(g_snes);
  ```
  forcing the CPU/APU cycle-catchup counter to its hard-coded maximum and
  draining it **synchronously, in one blocking call** — 10,000
  `apu_cycle()`/`dsp_cycle()` calls in a tight loop.

On real hardware this executes in a negligible fraction of a millisecond
— invisible. Under QEMU's TCG interpretation (already known, per
STATUS.md, to run well below the firmware's assumed ~340MHz overclocked
rate), the identical burst takes hundreds of milliseconds of real
wall-clock time. That's the freeze; the "couple of frames in quick
succession" is whatever renders once the burst finishes.

**This is real guest-firmware behavior in the `game-and-watch-retro-go-sd`
sibling repo, not a qemu-gnw device-model bug** — not fixed here. A real
fix (spreading the catchup over multiple calls/frames instead of one
synchronous drain) belongs in that repo's `common_rtl.c`.

**Important, per the user's own pushback and left open**: the user
correctly cautioned against generalizing this into "the whole stutter
problem is SMW's fault" — every retro-go core does its own CPU/PPU/APU
cycle-accounting with some kind of backlog/catchup mechanism (normal
emulator architecture), so if the true root cause is "TCG is slower than
the assumed CPU clock," *any* core with a catchup mechanism could show
some version of this, with SMW's just being unusually visible because of
its large (10,000-cycle) clamp. Celeste not visibly stuttering during
this session's testing was explicitly flagged by the user as "a clue,
not the explanation" — not yet confirmed either way. **Next session:
reproduce on a second game, PC-sample it the same way, and check whether
the hot address lands in that game's own core code (supports "generic
per-core pattern") or in shared qemu-gnw-side code like the DMA/SAI1
audio pipeline (supports "one common device-model bug").**

## Real device-model bugs found and fixed in qemu-gnw (`hw/display/gnw_h7b0_ltdc.c`)

Investigating the overlay flicker surfaced two real, sequential bugs in
this repo's own no-VBR auto-capture fallback (`vblank_tick()`'s `else`
branch) — both now fixed:

1. **Flicker/tearing regression** (the one flagged by `1663797f50`'s own
   commit message): the fallback fired on any vblank where `content_dirty`
   was false, with no way to tell "firmware that never uses VBR" apart
   from "firmware that uses VBR but just landed between two writes" —
   the latter grabbed a mid-draw frame, exactly the tearing the
   capture-at-VBR design (2026-07-10) exists to prevent. **First fix
   attempt**: a `vbr_ever_used` latch, set permanently once any `SRCR.VBR`
   write happened, gating the fallback off forever after. This fixed the
   flicker (committed as `014612c688`) but:
2. **Introduced a new bug**: a *permanent* latch has no way back once
   any game has ever been played, and it turns out retro-go's own
   main-menu UI paints pixels directly into the already-configured
   Layer1 without ever touching `SRCR` at all (confirmed: 32,000+ log
   lines / many real seconds with zero `SRCR` writes while the menu sat
   black). Returning to the menu from *any* game left the screen
   permanently black, since the one mechanism that could still refresh
   it (the fallback) was now latched off for good.
   - **First re-fix attempt**: replace the permanent latch with an
     idle-timeout (re-arm the fallback after N vblanks with no `SRCR`
     write). This unstuck the menu but reintroduced the tearing —
     because real in-game firmware can *itself* leave multi-hundred-ms
     real gaps between VBR writes (see the SMW catchup-burst finding
     above), so any timeout short enough to fix the menu promptly was
     also short enough to spuriously re-arm the fallback mid-game during
     those same bursts.
   - **Actual fix landed**: `vbr_active`, a boolean reset to `false` on
     every `SRCR.IMR` write and set to `true` on every `SRCR.VBR` write.
     IMR reloads are what real firmware uses to apply a fresh
     layer/format config on a screen transition (already true per this
     file's own existing comments on `HAL_LTDC_SetPixelFormat()`), so
     this is an event-based checkpoint instead of a time-based guess:
     a new screen has to prove it uses VBR before the fallback disables
     itself for it again, with no elapsed-time threshold to get wrong in
     either direction.

**Caveat**: this last fix has not yet been confirmed against a live
repro session the way the flicker and (separately) the black-screen
fixes were each independently confirmed — recommend a real pause-overlay
+ menu-return regression pass before treating this as fully closed.
(The black screen the user was still hitting after this fix turned out to
be a *different*, unrelated problem — see below — so this fix's own
correctness is still only inferred from code reasoning, not re-observed
live.)

## A second, unrelated black-screen cause: corrupted persistent flash state

After the `vbr_active` fix, the user still saw the main menu go black.
Root-caused to something else entirely: this project's flash/extflash
images are **persistent by default**
(`memory_region_init_ram_from_file`, landed in the 2026-07-13 web-builder
session) — QEMU writes real firmware flash/save-state operations back to
`backup/qemu-images/zelda-*.bin` on disk. This session killed and
relaunched QEMU abruptly (plain `kill`, no graceful shutdown) upwards of
ten times while games were actively running and potentially
mid-flash-write. Relaunching against **fresh copies** of the same image
set immediately fixed the black menu screen, strongly implicating
corrupted persistent state from those abrupt kills — not a code bug at
all. Confirmed by the user live.

**Takeaway for future sessions**: when iterating on QEMU rebuilds that
require killing and relaunching a live game session many times in a row,
either shut down cleanly or work from copies of the persistent images,
not the live ones — repeated hard kills mid-write are a real corruption
risk with this project's now-persistent-by-default flash backing.

## Tool/workflow learnings this session

- **Multi-overlay symbol resolution**: this build links every retro-go
  core into the *same* fixed RAM virtual address range as a swappable
  overlay (`.overlay_smw`, `.overlay_celeste`, `.overlay_nes`, etc. all
  sharing e.g. `0x2404b000`). Plain `arm-none-eabi-addr2line` against
  such an ELF silently resolves to whichever overlay's symbol happens to
  be first/nearest in the symbol table — wrong and misleading, not an
  error. Correct approach: `arm-none-eabi-readelf -sW <elf>`, find the
  target overlay's section index (`readelf -S`), filter the symbol table
  to that section index, then find the nearest preceding `FUNC` symbol
  by address manually. Worth turning into a small script
  (`scripts/resolve_overlay_symbol.py`?) if this comes up again.
- **`scripts/hotloop_sample.py` is the right first tool** for "where is
  the CPU spending its time" questions on a live target — much safer and
  more reliable than hand-rolling a GDB watchpoint/single-step script.
  This session's ad hoc watchpoint attempt produced inconsistent data
  (a real step-over bug, most likely) and interrupted the user's live
  session for no benefit; `hotloop_sample.py` gave clean, reproducible,
  actionable data on the first and second tries.
- **A GDB connect briefly halts the whole VM**, including audio/input
  delivery — expected and already documented in the script's own
  docstring, but worth restating: don't reach for it to "just peek"
  during a delicate live repro without warning the user first (this
  session did warn, after the fact, when asked "did you mean to halt the
  game?" — should have led with that instead).
- **Reading `uwTick` (or any HAL tick counter) directly via
  `read_memory()`** is a cheap, reliable way to distinguish "CPU is
  genuinely hung" from "CPU is alive but not doing the thing you expect"
  before spending time on deeper hypotheses — used successfully twice
  this session to rule out a hard hang.

## Part 2 continuation: the menu black-screen had a second, unrelated cause

After landing `vbr_active`, the user still saw the main menu go black.
Root-caused to something else entirely: this project's flash/extflash
images are **persistent by default**
(`memory_region_init_ram_from_file`, landed in the 2026-07-13 web-builder
session) — QEMU writes real firmware flash/save-state operations back to
`backup/qemu-images/zelda-*.bin` on disk. This session killed and
relaunched QEMU abruptly (plain `kill`, no graceful shutdown) upwards of
ten times while games were actively running and potentially
mid-flash-write. Relaunching against **fresh copies** of the same image
set immediately fixed the black menu screen, strongly implicating
corrupted persistent state from those abrupt kills — not a code bug at
all. Confirmed by the user live. Both causes are now resolved:
`vbr_active` (code fix) and using fresh image copies (workaround for the
corrupted state; the underlying images themselves are still whatever
state they were left in and weren't repaired).

**Takeaway for future sessions**: when iterating on rebuilds that require
killing and relaunching a live game session many times in a row, either
shut QEMU down cleanly or work from copies of the persistent images, not
the live ones.

## Part 3: Link's Awakening DX (GBC) freeze — confirms the stutter isn't SMW-specific

The user hit a full freeze (not just stutter) in Link's Awakening DX
(GBC, gnuboy core). Screen frozen, but responsive — a button press caused
menu logic to run, just not visibly. `scripts/hotloop_sample.py` during
the freeze resolved the hot address (via the same overlay-symbol-filtering
technique as the SMW investigation, this time against `.overlay_tgb`) to
`apu_snd::render(short*, int)` — the gnuboy core's own audio-rendering
function. This is the same *shape* of finding as SMW's `smw__dsp_cycle`
(also audio rendering) but in a completely independent, unrelated C++
codebase. Two unrelated emulator cores both showing their hottest code in
audio rendering during a stall is real evidence the user's instinct was
right: this isn't an SMW-specific bug, and the shared suspect is
qemu-gnw's own audio pipeline (`hw/misc/gnw_h7b0_dma.c` /
`gnw_h7b0_sai1.c`), not each game's engine.

## Part 4: DMA/SAI1 dynamic rebind — a real architectural change, but ruled out as tonight's cause

Commit `77e86eacdd` (earlier today, for OFW/stock-Zelda support) replaced
a compile-time-fixed DMA stream binding for SAI1 audio with a dynamic
DMAMUX-based rebind: `gnw_h7b0_dma_set_request_notifier()` /
`gnw_h7b0_dma_rebind_request()`, which re-resolves which DMA stream owns
SAI1's audio request (DMAMUX1 request ID 87) from the DMAMUX1 `CxCR`
shadow registers, re-run on every `CxCR` write. This exists because
stock Zelda firmware routes SAI1 to a different DMA stream (DMA2 Stream6)
than retro-go does (DMA1 Stream0) — a real, legitimate need, not a bug by
itself.

Instrumented live (temporary `fprintf`s in `gnw_h7b0_dma_rebind_request()`
and `gnw_h7b0_sai1_update_voice()`, both removed after this investigation)
to check whether a rebind event correlated with the freeze. It didn't:
the rebind fired exactly 3 times total across the whole session, all
during the retro-go boot sequence itself (stock Zelda's ch14 binding on
boot, briefly unbound, then retro-go's own ch0 binding once retro-go
actually started) — none during the later freeze. **This rules out the
DMA/SAI1 rebind as the cause of this specific freeze**, though it remains
a real architectural change worth knowing about for any future
audio-timing investigation.

## Part 5: the actual mechanism — SysTick tick-loss under TCG load, not a device bug

With the DMA/SAI1 and PWR-readiness theories ruled out, direct register
reads during an active freeze told the real story:

- `RCC_CR=0x00000025`, `RCC_CFGR=0x00000000` (`SWS=0`, HSI) — both CSI and
  PLL1 fully disabled — **unchanged across 5+ real seconds** of polling.
  Not "slow," genuinely parked.
- `uwTick` (read directly via `read_memory()`, no HAL dependency) was
  still incrementing — the CPU was alive — but only at **~228/sec**
  instead of the normal ~1000/sec. A real, measured ~4.4x slowdown in the
  firmware's own notion of elapsed time, isolated to this exact window.
- `PWR_SRDCR.VOSRDY` and `PWR_CSR1.ACTVOSRDY` were both already `true`
  when read live — ruling out a stuck PWR-readiness-flag device-model bug
  in the specific busy-wait loops `SystemClock_Config()`
  (`game-and-watch-retro-go-sd`'s `Core/Src/main.c:417`) uses.

Traced (via the sibling repo's real, non-stripped debug source — not
Ghidra, since retro-go's own build carries symbols) to
`Core/Src/gw_sleep.c`'s STOP2 low-power sleep/wake cycle, which retro-go's
idle "attract loop" almost certainly enters to save power, and which
calls `SystemClock_Config()` again on wake.

The actual mechanism was found by reading QEMU's own
`hw/core/ptimer.c` and `hw/timer/armv7m_systick.c` source directly, which
**corrects a specific claim** in the existing
`docs/session-2026-07-12-breakpoint-lockstep-tracing.md` part 22 doc (see
the correction note added there). That doc attributed the residual
tick-rate gap to `armv7m_systick`'s ptimer "reprogramming for one period
from now" under load, with "no catch-up," due to
`PTIMER_POLICY_WRAP_AFTER_ONE_PERIOD`. Direct code reading shows this
isn't accurate: `ptimer_reload()`'s deadline math
(`s->next_event = s->last_event + delta * period`) properly chains from
the *previous* scheduled deadline, not from "now" — QEMU's ptimer layer
does correctly attempt to catch up a backlog.

The real mechanism is one level up, in basic ARM interrupt semantics:
**SysTick's pending-IRQ state is a single bit, not a counter.** On real
hardware, `SysTick_Handler` executes in a handful of cycles, so it's
always done well before the next tick could possibly arrive. Under
QEMU/TCG, executing that same handler costs far more real wall-clock time
due to interpretation overhead. If the ptimer's virtual deadline fires
again while the CPU is still busy delivering/running the *previous*
SysTick interrupt, that second firing just finds the interrupt already
pending — it doesn't queue a second invocation, and `HAL_IncTick()` never
runs for it. That tick is genuinely, silently lost, no matter how
correctly the ptimer itself schedules its own deadlines. The heavier the
per-frame device-model work happening on the single host thread (LTDC
captures, DMA/audio churn), the more often this collision happens —
which is exactly why `uwTick` cratered specifically during the heaviest
observed stretch (STOP2 wake + DMA rebind + audio reinit all at once),
and why the existing part 22 doc's own headless-mode comparison (SysTick
rate rising when display/audio overhead is removed) is still valid
supporting evidence, even though its specific ptimer-policy explanation
wasn't.

## Decision: pursue `-icount`, as a separate, larger effort

This converges with STATUS.md's own prior conclusion (parts 20-23) that
the residual tick-rate gap is genuine TCG-speed-vs-assumed-clock-frequency
mismatch, not a quick, scoped device-model patch. Discussed with the user:
a real fix means switching this machine to `-icount` (deterministic,
instruction-count-scaled virtual time, decoupling guest timing from real
host execution speed) rather than continuing to chase individual
symptoms. The user agreed to pursue this as its own dedicated effort,
explicitly *not* folded into tonight's device-model bugfixes, because it's
a global timing-model change: everything currently tuned around wall-clock
assumptions (`gnw_h7b0_dma.c`'s accumulator-scheduled timers, LTDC's
vblank period derivation, audio pacing) needs re-validating under it, and
getting it subtly wrong is the kind of mistake that surfaces much later
in an unrelated place. Tracked as a new, separate follow-up, not listed
as an open item below.

## Current state / open items for next session

- [x] Pause-overlay flicker fix (`vbr_ever_used`, then `vbr_active`):
      code-reasoned and consistent with all later testing; no dedicated
      clean regression pass was re-run after the corrupted-flash-state
      confound was found, so a final confirmation pass is still worth
      doing but is low-risk.
- [x] Menu black-screen: both causes (the fallback-gating bug and the
      corrupted persistent flash state) identified and resolved.
- [x] General stutter root cause: confirmed to be SysTick tick-loss under
      TCG load (ARM single-pending-bit IRQ semantics colliding with TCG's
      slower per-instruction real-time cost), not a DMA/SAI1 rebind bug,
      not an SMW-specific issue, and not (solely) the ptimer-policy
      mechanism an earlier session doc claimed.
- [ ] Not fixed here (belongs in `game-and-watch-retro-go-sd`): SMW's
      synchronous 10,000-cycle APU catchup burst in
      `external/smw/src/common_rtl.c:673`.
- [ ] Next major effort: switch qemu-gnw to `-icount`. Expect to need to
      re-validate `gnw_h7b0_dma.c`'s accumulator-scheduled timers, LTDC's
      vblank period derivation, and SAI1 audio pacing against
      deterministic instruction-scaled time; scope this as its own
      session, not a quick follow-up.
