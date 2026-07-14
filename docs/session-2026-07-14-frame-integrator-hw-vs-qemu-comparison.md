# 2026-07-14 — definitive root cause of the post-pause stutter: `frame_integrator` positive feedback loop, confirmed hardware-vs-QEMU

## Starting point

After the 2026-07-13 session (see `docs/session-2026-07-13-part5-...md`
and its part-6 addendum) landed the real LTDC `vbr_active` fix and ruled
out several false leads (DMA/SAI1 dynamic rebind, a ptimer-policy
misattribution, `-icount` alone), one open question remained: the user
observed a severe "get stuck, jump forward, repeat" stutter specifically
after closing Retro-Go's pause overlay, across multiple unrelated cores
(SMW, zelda3, gnuboy/tgbdual), and pushed back hard on treating this as
inherent/unfixable TCG slowness: *"This doesn't happen to real hardware.
Why does it only happen to us? ... I'd much rather you compare the two."*

That was the right call. A same-instrumentation, side-by-side comparison
against real hardware (no resets on either side — both targets attached
mid-session, already paused, exactly as the user had them prepared)
gives a definitive, mechanistic answer, not a guess.

## The mechanism traced

Not a per-core bug. It lives in `Core/Src/porting/common.c` in
`game-and-watch-retro-go-sd`, in `common_emu_frame_loop()` and
`open_pause_menu()` — shared by every core (SMW, zelda3, gnuboy/tgbdual,
Celeste, all of them). Key logic:

```c
frame_integrator += (elapsed_10us - frame_time_10us);
if (frame_integrator > frame_time_10us << 1) common_emu_state.skip_frames = 2;
else if (frame_integrator > frame_time_10us) common_emu_state.skip_frames = 1;
else if (frame_integrator < -frame_time_10us) common_emu_state.pause_frames = 1;
```

A leaky integrator tracking "how far behind real time is the emulated
core." When it's built up a large backlog, `skip_frames = 2` makes the
core run twice per loop iteration to catch up. `open_pause_menu()`
resets `startup_frames`/`pause_after_frames`/`clear_frames` on return
from the menu, but not `frame_integrator` itself (the `startup_frames < 3`
early-return in `common_emu_frame_loop` does prevent the first few
post-menu calls' huge stale `elapsed_10us` from polluting the integrator
directly — confirmed *not* the gap here, per the trace below).

## Instrumentation

Two breakpoints, resolved cleanly against the non-overlay (shared,
unambiguous) code section of `retro-go-temp/elf/gw_retro_go_bank2.elf`:

- `0x08106c50` — `open_pause_menu`'s return (the exact instant the
  overlay closes and gameplay resumes)
- `0x081073a6` — the `str r3, [r2, #0]` instruction in
  `common_emu_frame_loop` that writes the updated `frame_integrator`
  value (RAM address `0x20001ff8`)

Traced via this project's own established breakpoint-based (never
single-step-as-the-tracing-mechanism) method, continue-between-
breakpoints with an explicit step-over dance (clear bp at current PC,
single-step once, reinsert, continue) — `scripts/step_init_calls.py`'s
pattern, adapted per-target:

- **QEMU**: raw GDB-remote `Z0`/`z0` (software breakpoints). Note:
  this fork's gdbstub accepts `Z1` (hardware breakpoint) set requests
  with `OK` but they silently never trigger — a real quirk worth
  remembering, `Z0` works correctly.
- **Real hardware**: `OCDBackend["openocd"]()`, raw Tcl `bp <addr> 2 hw`
  / `rbp <addr>`, `resume` + blocking `wait_halt <ms>` (this project's
  established real-hardware pattern from `checkpoint.py`/
  `watch_loop_flag.py`) — **no `reset`/`reset_and_halt` call**, attaching
  directly to the live, already-paused state on both targets.

## Results

**QEMU** (zelda3, same pause-overlay repro):
```
[0] frame_integrator=8675
... drains: 8742, 8409, 8076, 8343, 8010, 7977, 7544, 7111, 6578, 6445 ...
[10] dt=53.8ms  value=6445
[11] dt=73.5ms  value=8112   <- spike: a real-time hiccup re-injects backlog
[12] dt=36.9ms  value=11279  <- still climbing
... continues oscillating with more small spikes, never fully recovering
```

**Real hardware** (identical breakpoints, identical repro):
```
[0] frame_integrator=3129   <- much smaller initial backlog to begin with
... drains cleanly through zero: 3196, 1763, 330, -1003, -2436, -3869 ...
... settles into a STABLE, BOUNDED oscillation around -2500 to -5000
... every single dt stays in a tight 44-72ms range for all 60 samples
... zero spikes, anywhere, in the entire capture
```

## What this proves

The firmware logic is *identical* on both platforms — same source, same
integrator, same catch-up decision. The difference is purely execution
cost:

- On real hardware, running the core 2x (`skip_frames=2`) to pay off a
  backlog costs a negligible fraction of a real frame at native ~340MHz.
  The integrator settles into a clean, bounded steady-state and never
  spikes, because the catch-up work itself is fast enough to never
  register as a new source of lag.
- Under QEMU/TCG, that same "run the core 2x" catch-up work is
  measurably slow in real wall-clock terms. Its own execution cost shows
  up as a large `elapsed_10us` on the *next* loop iteration, which feeds
  right back into `frame_integrator`, demanding *more* catch-up — a
  genuine positive feedback loop that is structurally impossible on real
  hardware (because there the cost is too small to ever close the loop)
  but forms naturally under TCG interpretation.

This is not a QEMU device-model bug, not a firmware bug in the
conventional sense (the logic is correct and battle-tested on real
hardware for years), and not something `-icount` alone fixes (confirmed
separately, see the 2026-07-13 doc) — it's an emergent interaction
between a real-time-feedback control loop and a host that can't always
execute its "make up for lost time" work fast enough, which is exactly
the condition under which any leaky integrator with unbounded gain can
run away.

## Candidate real fixes (not yet implemented — for discussion)

1. **Clamp `frame_integrator`'s growth** (in
   `game-and-watch-retro-go-sd`'s `common.c`, now explicitly in-scope
   per the user's direction: *"this is our code, I determine what is
   out of scope"*). Bounding it to something like `2-3x frame_time_10us`
   directly caps how large a single catch-up burst can ever be,
   independent of platform — real hardware would never notice (it never
   gets near that bound anyway), and QEMU would be structurally
   prevented from spiraling, since there's a hard ceiling on how much
   "extra work" any single iteration can be asked to do. Lowest-risk,
   most targeted option; doesn't fix TCG's raw speed, just prevents the
   feedback loop from compounding.
2. **`-icount`**: makes guest-perceived time track executed instructions
   instead of wall-clock, which in principle prevents slow catch-up work
   from ever inflating `elapsed_10us` in guest-time terms at all (the
   loop can't close if the "how far behind" measurement itself can't be
   distorted by host slowness). Bigger, riskier, structural change
   (global timing-model swap, needs re-validating every wall-clock-paced
   device timer in this repo) — already scoped separately in the
   2026-07-13 `-icount` migration doc, and testing so far found it
   doesn't fully resolve visible stutter on its own, though it may not
   have been tested specifically against *this* mechanism in isolation.

## Tooling notes for future sessions

- `Z1` (hardware breakpoint) silently doesn't trigger against this
  fork's gdbstub despite acking `OK` on set — use `Z0` (software
  breakpoint) for any future GDB-remote scripting against this project's
  QEMU, not `Z1`.
- For real-hardware OpenOCD scripting: `OCDBackend["openocd"]().open()`
  in default `attach` mode does **not** reset the target — safe to use
  against a live, already-running/paused device. Confirmed this session
  by attaching to the user's live paused repro without disturbing it.
- `GDBBackend.read_register()`/similar convenience methods call
  `halt()`/`resume()` internally when the target is running — mixing
  those with manual raw `_send_command()` breakpoint/continue sequences
  in the same script can produce confusing, hard-to-debug state (this
  session hit exactly this: a version of the trace script using
  `read_register("pc")` before the main loop caused every subsequent
  breakpoint wait to silently time out). Prefer either the convenience
  API *or* raw commands throughout a single script, not a mix.
