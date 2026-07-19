# 2026-07-13 -- `-icount` migration scoping (planning only, no code changed)

This is a planning document, not an implementation. It follows up on
`docs/session-2026-07-13-part5-retro-go-ltdc-vbr-and-stutter-investigation.md`'s
decision (agreed with the user) to pursue `-icount` as a dedicated effort
to fix SysTick tick-loss under TCG load. See that doc for the root-cause
trail: SysTick's pending-IRQ state is a single bit, and under real
wall-clock-tracking `QEMU_CLOCK_VIRTUAL`, a slow (TCG-interpreted)
`SysTick_Handler` can still be running when the next tick's virtual
deadline fires, silently losing it. `-icount` fixes this by making
`QEMU_CLOCK_VIRTUAL` advance in lockstep with executed instruction count
instead of host wall-clock time, so guest-perceived time is deterministic
regardless of host/TCG speed.

All source citations below are read directly from this v11.0.2-pinned
checkout (`accel/tcg/icount-common.c`, `accel/tcg/tcg-all.c`,
`qemu-options.hx`, `docs/devel/tcg-icount.rst`,
`docs/devel/multi-thread-tcg.rst`, `docs/system/replay.rst`), not from
general QEMU knowledge.

## 1. What `-icount` requires/changes for this machine

### CLI syntax (from `qemu-options.hx`, confirmed in this checkout)

```
-icount [shift=N|auto][,align=on|off][,sleep=on|off][,rr=record|replay,rrfile=<f>[,rrsnapshot=<s>]]
```

- `shift=N`: virtual CPU executes one instruction per 2^N ns of virtual
  time (fixed ratio, `icount_enable_precise()` in `icount-common.c`).
- `shift=auto`: shift is adjusted at runtime to track real time
  (`icount_enable_adaptive()`); starts at shift=3 ("125 MIPS" initial
  guess per a code comment) and self-corrects via `icount_adjust()`.
- `align=on`: tries to keep host and virtual clock synced to the `shift`
  rate; **incompatible with `shift=auto`** and with `sleep=off`
  (`icount_configure()` rejects both combinations with a hard error).
- `sleep=on` (default): virtual time still advances at normal speed while
  the vCPU is idle/sleeping. `sleep=off`: virtual time jumps instantly to
  the next timer deadline when the vCPU sleeps -- more deterministic, but
  cannot combine with `shift=auto` or `align=on`.
- `rr=record|replay,rrfile=...`: full deterministic record/replay; not
  relevant to this project's goal (not chasing determinism-across-runs,
  chasing "match assumed guest clock rate under variable host load") --
  skip for now, revisit only if a "record once on fast host, replay
  identically for debugging" workflow becomes useful later.

**Recommendation for first pass:** `-icount shift=auto,align=off` (the
default alignment behavior when only `shift` is given -- `align` defaults
off already per the option docs). `shift=auto` is the only mode that
self-adjusts to actual host throughput without hand-tuning a shift value
per host machine, which matters since dev machines will vary. A fixed
`shift=N` tuned for one host would silently be wrong (too fast or too
slow) on a faster or slower dev machine -- worth a follow-up pass once
`shift=auto`'s behavior is validated, since a hand-picked fixed shift is
also more "deterministic" in the sense the project actually cares about
(consistent guest-perceived clock rate run-to-run on one machine), but
`auto` is the safer starting point.

### TCG threading requirement -- confirmed, and confirmed low-cost here

`docs/devel/multi-thread-tcg.rst` (line ~39, this checkout) states
single-threaded TCG is required in two cases: forced by
`--accel tcg,thread=single`, **or** simply by enabling `--icount` mode.
Cross-checked directly against `accel/tcg/tcg-all.c`'s
`tcg_init_machine()`: when `mttcg_enabled` is left at its default
`ON_OFF_AUTO_AUTO`, it resolves to `ON_OFF_AUTO_OFF` (single-thread)
automatically whenever `icount_enabled()` is true, regardless of whether
the guest CPU class supports MTTCG. Separately, `accel/tcg/tcg-all.c`
(`mttcg_enabled` setter path) errors outright ("No MTTCG when icount is
enabled") if the user explicitly passes `thread=multi` together with
`-icount`. **So: no `-accel` flag change is needed** -- passing `-icount`
alone is sufficient; QEMU auto-downgrades to single-threaded TCG, and
`scripts/boot_qemu.sh` currently passes no `-accel`/`thread=` flag at all
today, so there's nothing to change there for compatibility.

**Performance cost is effectively zero for this project**, because this
machine only ever has one vCPU (single Cortex-M7 core, confirmed by this
SoC's board file having one CPU). MTTCG's entire value proposition is
running multiple vCPUs on separate host threads
(`target/arm/cpu.c:2332` sets `mttcg_supported = true` generically for
ARM, but that only matters for multi-core boards); with `ms->smp.max_cpus
== 1` there is no second vCPU thread to lose. The "single-threaded TCG"
requirement that trips up multi-core `-icount` migrations elsewhere in
QEMU is a non-issue here.

## 2. Timing-sensitive device model audit

Checked every device listed, by grepping for `qemu_clock_get_ns`/
`timer_mod`/`QEMU_CLOCK_*` usage directly:

- **`hw/display/gnw_h7b0_ltdc.c`** (vblank_timer, line_timer): both
  scheduled purely against `QEMU_CLOCK_VIRTUAL` (`gnw_h7b0_ltdc.c:155,
  158, 172, 256, 836, 913, 915`). Vblank rate is derived from live PLL3R
  frequency and panel timing (`gnw_h7b0_ltdc_recalc_timers()`, ~line 120-150),
  not a wall-clock assumption -- it computes a target Hz from the clock
  tree, then schedules a `QEMU_CLOCK_VIRTUAL` deadline that many ns out.
  Under `-icount`, virtual time advances with instruction count instead
  of real time, so this timer will fire relative to *guest-perceived*
  time correctly -- this should transparently benefit from `-icount`,
  same mechanism as SysTick. No wall-clock-real-time assumption found to
  flag as a risk here beyond the general "did we get vblank_hz right"
  question already tracked as the open coverflow-flicker issue.

- **`hw/misc/gnw_h7b0_dma.c`** (accumulator-scheduled stream timers): also
  purely `QEMU_CLOCK_VIRTUAL`-based (`gnw_h7b0_dma.c:245, 252, 306, 424`).
  Its own top-of-function comment (`gnw_h7b0_dma_schedule_next()`,
  ~line 228-240) explicitly documents *why* it chains from the stream's
  own previously-scheduled deadline rather than `now + delay`: to avoid
  baking in host-scheduling lateness. That "catch-up unless more than one
  period behind" clamp (`if (next < now - delay_ns) next = now;`) is
  itself designed for exactly the kind of scheduling jitter `-icount`
  should reduce, not something `-icount` will break -- but it's worth
  watching this specific clamp under `-icount`, since `-icount`'s notion
  of "now" moves in instruction-count-sized jumps rather than smoothly,
  which could interact with the "more than one period behind" comparison
  in ways not seen under wall-clock time. Flagged as a live-test item
  below.

- **`hw/misc/gnw_h7b0_sai1.c`** (audio) -- **this is the one real risk in
  the audit.** Its top-of-file comment is explicit: sample delivery is
  *not* scheduled off `QEMU_CLOCK_VIRTUAL` at all. It's a pulled model --
  DMA1 transfer-complete snoops feed a `Fifo8` queue, and QEMU's own
  `audio_run()` (host-audio-backend-driven, real wall-clock real-time)
  calls back into `gnw_h7b0_sai1_voice_cb()` to drain it whenever the
  *real* host audio backend's buffer has room. The file's own comment
  explains this replaced an earlier push-model design specifically
  because pacing playback off approximated DMA timing caused audible
  pitch/speed distortion -- i.e. this device already learned once that
  real-time audio pacing and guest-clock pacing don't mix well, and
  deliberately decoupled them. Under `-icount`, guest instruction
  execution (and therefore DMA-driven Fifo8 fill rate) is scaled to
  virtual time, but `audio_run()`'s drain rate stays tied to the real
  host audio clock -- if `-icount`'s effective ratio doesn't track real
  time closely (e.g. mid-adjustment under `shift=auto`, or a heavy frame
  making the guest fall behind even in virtual-time terms because it's
  mid TB re-execution for an MMIO access), the Fifo8 can starve or
  overflow independent of anything `-icount` was meant to fix. This is
  the single biggest live-test unknown in this whole migration (see
  Section 3 and 5).

- **`hw/misc/gnw_h7b0_rcc.c`**: no `qemu_clock_get_ns`/`timer_mod` at all
  found by direct grep -- it's a synchronous register/mux model (PLL
  lock bits mirrored synchronously on write, per the CSI/RDY fix
  documented in STATUS.md), not time-driven. Nothing here to revisit for
  `-icount`. Worth reconfirming `docs/h7b0-clock-tree-findings.md`
  doesn't rely on any timing assumption RCC itself doesn't encode --
  a quick re-read found no `-icount`-relevant clock-tree caveats logged
  there (it's about frequency/formula correctness, not `-icount`-style
  virtual-time pacing).

- **`hw/misc/gnw_h7b0_rtc.c`**: uses `QEMU_CLOCK_VIRTUAL` to track
  elapsed wall time for the RTC's own calendar/counter registers
  (`rtc_base_vclock_ns`, lines 48/89/118). This is a case where
  `-icount`'s decoupling from real time is a real behavior *change*, not
  just a fix: real hardware's RTC genuinely tracks real elapsed
  wall-clock seconds (backed by an external 32kHz crystal, independent of
  CPU clock), so if `-icount` makes `QEMU_CLOCK_VIRTUAL` diverge
  significantly from real time (e.g. running well under 1x due to heavy
  per-frame device load), the emulated RTC's calendar would drift out of
  sync with a real wall clock. This is likely an acceptable tradeoff (a
  drifting on-screen clock face is far less bad than the current
  multi-hundred-ms SysTick freezes), but it's a real, deliberate
  regression to flag and get sign-off on, not an oversight.

- **TIM1 / other TIMx models**: searched
  `hw/misc/gnw_h7b0_tim1.c` and related files -- no dedicated
  `gnw_h7b0_tim1.c` file was found as a distinct device; TIM2 is
  mentioned in STATUS.md as part of the boot-path peripheral set and
  should be located and checked the same way (`qemu_clock_get_ns`/
  `timer_mod` grep) before implementation begins -- not fully audited in
  this pass; flagged as an open item in Section 5.

- **ADC**: no timer-driven ADC conversion model found via grep in this
  pass (STATUS.md describes it as part of the boot-path set, with a
  `GNW_H7B0_ADC_FULL_BATTERY_RAW` register fix landed previously, which
  sounds like a static/synchronous register value rather than a timed
  conversion). Should be explicitly re-checked against `-icount` before
  implementation, same as TIM2 above.

## 3. Project-specific gotchas

- **gdbstub interplay**: no `-icount` references found anywhere in
  `gdbstub/*.c` in this checkout, and QEMU's own icount docs
  (`docs/devel/tcg-icount.rst`) don't mention gdbstub at all. The
  historical `-icount`/gdbstub tension documented upstream is really
  about *replay* mode (`rr=record|replay`) needing gdbstub's stepping to
  stay consistent with the replay log -- not relevant here since this
  project isn't using `rr=`. Plain `-icount shift=auto` (or `shift=N`)
  with no `rr=` should have no special gdbstub interaction beyond the
  general "single-threaded TCG" mode already covered in Section 1. This
  project's actual gdbstub-dependent workflows (`gnwmanager --qemu`,
  `scripts/gdb_tap.py`, live register-snapshot/lockstep tracing scripts)
  use `continue`/breakpoints/memory read-write, not single-instruction
  replay-log stepping, so this looks low-risk -- but it hasn't been
  live-tested, so it's listed as an open unknown in Section 5 anyway,
  since `docs/session-2026-07-12-breakpoint-lockstep-tracing.md`
  documented enough gdbstub-adjacent subtlety (off-by-one register
  timing around breakpoints, a socket-timeout wedge) that a novel
  interaction wouldn't be shocking.

- **Audio backend real-time tension**: no explicit `-icount`-vs-audio
  interaction documented anywhere in this checkout's `docs/` or
  `audio/*.c` (grepped, found nothing). This is a known-in-general
  category of QEMU issue (host-audio-backend-paced devices are
  inherently real-time, `-icount` guest time is not, by design) rather
  than something this specific fork's docs already warn about -- treat
  the SAI1 analysis in Section 2 as the operative finding here, not a
  documented QEMU caveat. `GNW_AUDIODEV=none` (already supported by
  `scripts/boot_qemu.sh`) is the immediate fallback for isolating whether
  a problem is `-icount`-vs-audio-timing specifically vs. something else
  during testing.

- **`scripts/boot_qemu.sh` currently passes no `-icount` and no `-accel`
  flag at all** (confirmed by reading it in full) -- so the change here
  is purely additive (add `-icount shift=auto` to both the `--ephemeral`
  and default exec lines), no existing flag conflicts to resolve.

## 4. Staged rollout / test plan

1. **Smoke test**: boot retro-go (the SD-backed homebrew build, cheapest
   iteration loop per CLAUDE.md's doc map) with
   `-icount shift=auto,align=off` added to `scripts/boot_qemu.sh`'s exec
   line, `-display sdl` (per this user's standing preference), no other
   changes. Confirm it boots to a visible, interactive menu at all before
   anything else -- a hard failure here (e.g. an assert in `icount_configure`
   or an MMIO handler that isn't `-icount`-safe, see `docs/devel/tcg-icount.rst`'s
   "Dealing with MMIO"/`gen_io_start()` section) would be a fast, cheap
   signal.
2. **uwTick rate measurement**: this project already proved out reading
   firmware's live `uwTick` HAL counter via GDB during tonight's session
   (ad-hoc scratch scripts) -- repeat that technique before/after
   `-icount`, sampling `uwTick` delta over a fixed wall-clock interval,
   under: (a) idle menu, (b) the STOP2 sleep/wake scenario that measured
   ~228/sec instead of ~1000/sec tonight, (c) heavy DMA/audio-reinit
   load. **Success criterion**: `uwTick` rate stays close to a stable,
   consistent value across all three scenarios (not necessarily exactly
   1000/sec in wall-clock terms, since `-icount` decouples guest time
   from wall time -- the point is *no collapse* during the heavy
   scenario relative to the idle scenario, unlike tonight's ~4.4x drop).
3. **PC-sampling regression pass**: rerun `scripts/hotloop_sample.py`
   against SMW and the gnuboy-core game that showed stutter tonight,
   confirming the previously-identified hot loops (`smw__dsp_cycle`,
   `apu_snd::render`) are no longer symptomatic of tick-loss-driven
   catch-up bursts (SMW's own 10,000-cycle APU catchup burst in
   `common_rtl.c:673` is a separate, not-in-scope firmware issue per the
   part5 doc -- don't expect `-icount` to remove that specific burst,
   only the *tick-loss* contribution layered on top of it).
4. **Audio-specific check**: with sound enabled (not `GNW_AUDIODEV=none`),
   listen for pitch/speed distortion or stutter during the same
   heavy-load scenarios -- this is the SAI1 risk from Section 2 made
   audible. If present, `GNW_AUDIODEV=none` isolates whether it's
   `-icount`-vs-audio-backend-timing specifically.
5. **`shift=auto` vs fixed `shift=N` comparison**: once `shift=auto`
   is confirmed working, try a few fixed `shift=N` values bracketing
   whatever `shift=auto` settles on (loggable, though this pass didn't
   find an existing way to observe the live auto-adjusted shift value
   short of adding a temporary debug print in `icount_adjust()` -- note
   this as an implementation-time detail) to see whether a fixed value
   gives a more stable `uwTick` rate than the adaptive one, since
   `shift=auto`'s self-correction runs on its own timer cadence
   (`icount_rt_timer`/`icount_vm_timer`, ~1/sec and ~10/sec respectively
   per `icount_configure()`) and could itself introduce a new, different
   kind of jitter under this project's specific heavy-load spikes.
6. **Rollback plan**: `-icount` is a pure CLI-flag addition with no
   device-model code changes required by this scoping pass's findings
   (Section 2 found every timing-sensitive device already uses
   `QEMU_CLOCK_VIRTUAL` correctly, except SAI1's real-time-audio-backend
   coupling, which is a pre-existing design, not new code). If live
   testing surfaces a regression worse than today's tick-loss stutter,
   rollback is simply: don't add the flag (or gate it behind an env var
   in `boot_qemu.sh` the way `--ephemeral`/`GNW_AUDIODEV` already are, so
   it can be toggled per-run without a code revert while still
   validating it side-by-side against the non-`-icount` baseline).

## 5. Open risks / unknowns requiring live testing

1. **SAI1 audio-vs-icount interaction** (Section 2/3) -- the single
   biggest unknown; no way to resolve from source reading alone since it
   depends on real host-audio-backend behavior interacting with
   `-icount`'s time-scaling under real load spikes.
2. **`gnw_h7b0_dma.c`'s "more than one period behind" catch-up clamp**
   under `-icount`'s jump-based (rather than smooth) time advancement --
   untested interaction, flagged in Section 2.
3. **TIM2 and ADC device models were not fully located/audited in this
   pass** (no dedicated `gnw_h7b0_tim1.c`/obviously-named ADC timer file
   was found by the greps run here) -- must be found and checked for
   `QEMU_CLOCK_VIRTUAL` usage before implementation, not just assumed
   clean by extension from the other devices audited.
4. **gdbstub interaction under `-icount`, live-verified** -- source
   reading found no direct conflict, but this project's gdbstub-heavy
   workflows (`gnwmanager --qemu`, `gdb_tap.py`, lockstep tracing) are
   exactly the kind of "novel combination this specific fork exercises
   more than upstream's own test suite does" scenario that has produced
   real surprises before (2026-07-12/13 sessions), so treat "no
   documented conflict" as "unverified," not "safe."
5. **Whether `shift=auto`'s self-adjustment settles fast/stably enough**
   for this project's specific load-spike pattern (STOP2 wake + DMA
   rebind + audio reinit all at once), or whether a fixed `shift=N`
   tuned per dev host ends up being the better default in practice --
   only resolvable by the Section 4 test plan.
6. **Whether `-icount` changes perceived game speed/feel** in a way a
   human playtester would notice even if `uwTick` rate is now stable
   (e.g. `shift=auto`'s target is "keep virtual time within a few seconds
   of real time," not "exactly 1x" at all times) -- needs actual
   hands-on play testing, not just instrumented measurement.
