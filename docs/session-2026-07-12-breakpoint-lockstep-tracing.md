# 2026-07-12 (later session, part 2) — breakpoint-based lockstep tracing

## Goal

Continuation of the same day's register-snapshot session (see
`docs/session-2026-07-12-register-snapshot-diffing.md`), pivoting from
static reset-state comparison to **live execution comparison**: reset
both QEMU and real hardware to the same point, run both forward to a
chosen checkpoint address via a real breakpoint (not single-stepping,
which is far too slow over real hardware's SWD link), and compare core
registers. Goal: find the exact point where stock Zelda firmware's boot
sequence first diverges between QEMU and real hardware.

## New tools (all in `scripts/`)

- **`watch_write.py`**: reset a target, set a write watchpoint at an
  address, run, report whether/where a write happens within a bounded
  timeout. Useful for "does firmware ever write X" questions.
- **`lockstep_compare.py`**: reset both targets to the real entry point
  and single-step both in alternation, comparing PC after each step.
  Correct and useful for confirming exact matching over a *short* window,
  but real hardware single-stepping is too slow (a full SWD round trip
  per instruction) to use over anything but a handful of steps — see
  "The single-step dead end" below.
- **`step_init_calls.py`**: breakpoints a specific call site
  (`0x0801b062`, the runtime's init-array/constructor dispatcher's call
  instruction) and repeatedly resumes+halts on each hit, comparing core
  registers per call. This — breakpointing a loop's own body at a fixed
  address and letting hardware run freely between hits — is the
  technique that actually worked; **prefer this pattern over
  single-stepping** for any future "compare per-iteration state" need.
- **`checkpoint.py`**: generalized one-shot version of the above — reset
  both targets, breakpoint an arbitrary address, run to it, compare
  registers. The main tool for "walk forward through a sequence of call
  sites identified via Ghidra, one at a time."
- **`watch_loop_flag.py`**: same per-iteration-breakpoint pattern as
  `step_init_calls.py`, specialized to watch the main superloop's
  `[r4+9]` exit flag and `[r4+0x60]` counter-enable field (from the
  morning session's stock-boot investigation doc) each pass.

All of these build on `halt_at_entry.py` (from the earlier snapshot
session) for the initial deterministic reset, and are pure consumers of
`gnwmanager`'s existing public backend API — **no changes to the
`gnwmanager` package**, which is a project rule reinforced hard this
session (see "Do not edit gnwmanager" below).

## Real, load-bearing bugs found and fixed in this session's own tooling

These aren't firmware bugs — they're bugs in the trace tooling itself
that produced false signals before being fixed. Recorded here because
they'll recur in any future scripting against these two backends:

1. **Breakpoint step-over is required before every `continue` when
   reusing the same breakpoint address repeatedly.** If a hardware
   breakpoint (gdb-remote `Z1`) is still armed exactly at the current PC
   and you send a bare `continue`, some gdbstubs (confirmed: QEMU's, via
   `gnwmanager`'s raw `GDBBackend`) immediately re-trap without executing
   anything — the target never advances. OpenOCD's `resume` command
   handles this transparently (temporarily disables the breakpoint at the
   current PC, steps over it, re-enables, continues), which is why real
   hardware showed genuine per-iteration progress while QEMU appeared
   completely "stuck re-calling the same function forever" — **that
   appearance was 100% a tooling artifact, not a real QEMU bug.** Fixed
   in `step_init_calls.py`/`checkpoint.py`'s `gdb_continue_and_wait()`:
   explicitly clear the breakpoint, single-step one instruction, re-arm,
   *then* continue. Any future gdb-remote scripting against QEMU that
   reuses a breakpoint address in a loop needs this same step-over.
2. **Breakpoints halt *before* the instruction at their address
   executes**, not after. A breakpoint at the address of `ldrb r0,[r4,#9]`
   stops with r0 still holding its *previous* value, not the freshly
   loaded byte — an off-by-one-instruction mistake that produced a fully
   plausible-looking but wrong "flag is always 1" reading before being
   caught and fixed (breakpoint moved one instruction later, to right
   after the load). Always double check *which side* of the instruction
   at your breakpoint address you're reading register state from.
3. **Python's `socket.makefile()` wrapper permanently wedges after one
   `socket.timeout`** — further reads raise `OSError: cannot read from
   timed out object` even after resetting the timeout back. Every script
   that uses a bounded-wait pattern (watch for an event within N seconds,
   otherwise give up) must recreate `backend._sock_file` after catching
   the timeout, before issuing any further commands (e.g. clearing the
   watchpoint in the cleanup path). Missing this turns a clean "no write
   observed" result into a crash during cleanup.
4. **`print()` to a redirected/backgrounded file is block-buffered, not
   line-buffered** — a long-running script's progress output can appear
   completely empty for its whole runtime and then dump everything at
   exit (or never, if killed first). Always run long/background trace
   scripts with `python3 -u` (or `PYTHONUNBUFFERED=1`) so progress is
   visible live, especially before concluding something is "stuck" from
   an empty-looking log.
5. **OpenOCD's `reg <name>` occasionally returns an empty response**
   right after a breakpoint hit, before the halt has fully settled —
   needs a short retry-with-backoff, not a single attempt. Real hardware
   is also just flaky in general this session (see next section) — don't
   assume every empty-response failure means the script is wrong before
   checking device health.
6. **Always wrap backend usage in `try/finally` calling `.close()`.**
   An exception between `backend.open()` and `backend.close()` leaves
   `OpenOCDBackend`'s spawned `openocd` subprocess orphaned, holding the
   SWD lock and breaking every subsequent script's connection attempt
   until it's manually killed. All scripts in this session were audited
   and fixed to guarantee this.
7. **Do not edit the `gnwmanager` package to add capabilities** — write
   pure-consumer scripts against its existing public API instead (as all
   of the above tools do). Got this wrong once this session (added
   breakpoint/watchpoint methods directly to `gnwmanager`'s own backend
   classes) and had to revert; that repo has its own independent
   development happening in parallel (confirmed: its `GDBBackend`
   changed mid-session — buffered socket reads, chunked memory I/O,
   auto-resume-on-connect — from someone else's concurrent work), so
   treat it as a fixed external dependency, not something this repo's
   sessions should be patching.

## The single-step dead end

Initial attempt: `lockstep_compare.py`, single-stepping open-ended from
the real entry point. This appeared to freeze at an identical PC
(`0x0800307e`) on both targets for 500+ steps — turned out to be a real,
benign 4-instruction polling loop (`FUN_0800306a`'s BSS-clear inner
loop) being sampled at the same phase every 20-step print, not a genuine
freeze (confirmed via raw per-step tracing showing the PC actually
cycling through 4 distinct addresses). Single-stepping through this kind
of large-trip-count loop is impractical (each real-hardware step is a
full SWD round trip) — the fix was switching to the breakpoint-per-call
pattern (`step_init_calls.py`/`checkpoint.py`) instead, letting hardware
run freely between checkpoints rather than single-stepping through
everything.

## Findings: QEMU tracks real hardware exactly, much further than expected

Walking forward checkpoint by checkpoint (each cross-referenced against
Ghidra decompilation of the relevant function first):

1. **Constructor dispatcher** (`FUN_0801b04c`, call site `0x0801b062`):
   after fixing the step-over bug above, QEMU and real hardware are
   **bit-for-bit identical** across all 3 constructor calls in this
   table (`r0`/`r1`/`r2`/`r3`/`lr`/`sp` all match every iteration),
   including the "return value chains into next loop iteration" calling
   convention (confirmed via raw disassembly: `mov r1,r0` at `0x0801b064`
   feeds the callee's return value directly into the next iteration's
   loop pointer). One of the 3 constructors is `FUN_0800306a`, the
   BSS-clear routine from the single-step section above — its zero-fill
   descriptor table (4 entries, dumped from `zelda-bank1.bin` offset
   `0x0801b36c`) and everything downstream of it works correctly in
   QEMU.
2. **`FUN_0801b028`** (the constructor dispatcher's caller): checkpointed
   at its next call site (`0x0801b03e`, `bl FUN_0801051e` with `r0=0`) —
   matches exactly.
3. **`FUN_0801051e`** (a large function: MPU/cache barrier setup via
   `DataSynchronizationBarrier`/`InstructionSynchronizationBarrier`, then
   a long chain of ~20 subsystem-init calls): checkpointed at the call to
   `FUN_0800ebc8` (`0x080105c6`) — matches exactly. This confirms MPU
   setup and every subsystem init before this point works correctly.
4. **The main superloop itself** (inside `FUN_0801051e`, body spans
   `0x080105ee`-`0x08010628`) — this is the exact loop documented in the
   morning session's stock-boot investigation: `bl FUN_0800edfc`
   (`0x08010606`, the counter function), `bl FUN_0800ea00` (`0x08010616`,
   the dispatcher), and the `[r4+9]` exit-flag check (`ldrb
   r0,[r4,#9]` at `0x0801061a`) are all right here. **Confirmed via
   `watch_loop_flag.py` (breakpointed one instruction after the `ldrb`,
   at `0x0801061c`, to correctly read the freshly-loaded byte): both the
   exit flag `[r4+9]` and the counter-enable field `[r4+0x60]` stay `0`
   across 20 consecutive passes on QEMU** — reproducing the morning
   session's finding exactly, now with a precise, scripted, repeatable
   checkpoint instead of a one-off manual trace.

**Net result: everything from the real entry point through MPU setup,
every subsystem init, and into the main superloop's first 20 passes
tracks real hardware exactly.** The actual divergence (if QEMU-side) has
to be either later inside this loop's own body (a sub-call not yet
checkpointed) or the loop genuinely runs identically on both targets and
the real difference is elsewhere entirely (e.g. an interrupt real
hardware receives that QEMU doesn't yet model).

## Real hardware kept losing SWD connection — root cause found, workaround needed next session

Repeated `OpenOCD lost contact with the target ... CPU likely entered
low-power/standby` errors interrupted real-hardware testing multiple
times this session, wasting significant time reconnecting. **Root
cause, identified by the user**: this is stock (unpatched) Zelda
firmware's own "state-6 standby" handler putting the physical device
into low-power standby during the repeated resets this kind of
interactive breakpoint tracing requires — this is a *real* firmware
behavior, not a tooling bug, and it's the exact same "state-6 standby"
handler already documented in `CLAUDE.md`/the very first stock-boot
session (Zelda `0x0800EA8C`, confirmed via `gnwmanager`'s own patch
tooling comment: `gnwmanager/gnwmanager/cli/gnw_patch/zelda.py` line
~140, `self.internal.b(0xEAA0, 0xEAC2) # state-6 standby: bpl -> b
(never standby)` — a 2-byte patch changing a conditional branch to
unconditional at that exact address, forcing the "never standby" path).

**Decision for next session**: use `gnwmanager`'s *patched* Zelda blobs
(not the raw stock dump) specifically for real-hardware interactive
breakpoint-tracing sessions like this one, to avoid the device
repeatedly dropping into standby mid-trace. This is a deliberate,
scoped exception to the existing project rule ("always boot the user's
supplied `backup/flash_backup_zelda.bin` unmodified") — that rule still
applies for *boot-behavior-accuracy* investigation (comparing QEMU
against genuine stock firmware behavior), but for *interactive real-
hardware debug-session stability*, the patched blob avoiding standby
entirely is the pragmatic choice. Don't conflate the two: findings about
*what stock firmware does* should still be validated against the real
stock dump eventually, but day-to-day breakpoint tracing sessions should
use the patched blob to stop losing the SWD connection.

**Follow-up work item (not yet done)**: QEMU has no standby/low-power
mode modeled at all currently. Once boot is further along, this will
need real implementation (real hardware genuinely does enter standby
under real conditions — button-hold, idle timeout — and stock firmware
depends on it working correctly for warm-boot power-off behavior, per
the already-documented PA0/WKUP1 "state-6 standby" fix from the first
stock-boot session). Not blocking right now, but flagged so it isn't
forgotten.

## Concrete next steps for a fresh session

1. Get (or build) `gnwmanager`'s patched Zelda blob locally (`gnwmanager
   flash-patch zelda <internal> <external>` normally flashes directly to
   a device — check whether it can emit local patched files instead, or
   just replicate the single documented 2-byte patch
   `0xEAA0→0xEAC2` at `0x0800EA8C` directly against
   `backup/qemu-images/zelda-bank1.bin` via a small script, consistent
   with `scripts/make_boot_images.py`'s existing pattern) and use it for
   the real-hardware side of any further breakpoint tracing.
2. Re-run `watch_loop_flag.py` against real hardware (now stable) to see
   whether the exit flag/counter-enable field also stay `0` on real
   hardware through the same 20 passes (confirming QEMU tracks hardware
   exactly, meaning the true blocker is elsewhere) or whether real
   hardware's values actually change (a genuine QEMU divergence, at
   last).
3. If flag/counter stay `0` on real hardware too (most likely, given
   perfect matching everywhere else so far): the real unblocking
   condition is probably an **interrupt** real hardware receives that
   QEMU doesn't fire yet (a button/timer/peripheral IRQ), not something
   visible in this loop's own polled state. Check NVIC `ISER`
   enable-state and pending-interrupt state at this same checkpoint on
   both targets next, and cross-reference against `FUN_0800ea00`'s own
   decompilation (not yet done this session) for what state transition
   is actually expected to unblock it.

## Follow-up session (same day, part 3) — this checkpoint is NOT the boot blocker

Built `scripts/make_zelda_patched_bank1.py`, replicating gnwmanager's
exact 2-instruction standby-skip patch (`0xEAA0`: `bpl`→`b 0xEAC2`
skipping state-6 standby; `0xEBD0`: `bpl`→`nop`, the SBF gate) directly
against `backup/qemu-images/zelda-bank1.bin` (verified byte-for-byte
identical to gnwmanager's own patch encoding). User flashed the patched
image to the physical device via gnwmanager directly (outside this repo,
per the "never edit gnwmanager" rule — this repo only produces the
patched *file*). Real hardware now resets and stays reachable over SWD
for repeated breakpoint tracing.

Re-ran `watch_loop_flag.py` at the same checkpoint (`0x0801061c`) against
both targets:
- **30 passes**: exit flag `[r4+9]` and `[r4+0x60]` stayed `0x00` /
  `0x00000000` identically on both QEMU and real hardware, `r4` identical
  (`0x2000ab90`) — confirms the step-3 prediction above.
- **400 passes, `--timeout 3`**: same result, zero divergence, zero
  change, on both targets, the entire time.
- **Critical correction to the standing hypothesis**: after the 30-pass
  run's script exited (clearing the breakpoint and closing the OpenOCD
  connection in the `finally` block, which lets the halted core resume
  running), the user observed the physical device immediately proceed to
  its intro boot artwork and intro melody — i.e. real hardware is not
  "stuck" in this loop at all in normal operation. Combined with the
  400-pass same-state result, this means **`FUN_0800ea00`'s state-0 idle
  poll (a trivial `bx lr` no-op per its dispatch table, decompiled this
  session — table at `0x0800ea08`, 6 entries for states 0-5, state 0 at
  `0x0800ea20`) is a normal fast-cycling background poll that both
  targets sit in identically as part of every superloop pass, not a
  stall condition.** The three-session-old assumption that `[r4+9]`/
  `[r4+0x60]` staying `0` was *the* boot blocker was wrong. QEMU tracking
  hardware bit-for-bit here (now confirmed to 400 passes, up from 20)
  says nothing about the real divergence, because real hardware isn't
  blocked here either.
- **Revised next-step direction**: the actual difference between QEMU
  (no display/audio) and real hardware (audible intro melody, visible
  boot artwork within moments of being released from the breakpoint)
  must be downstream of this loop, not inside it. Since state
  advancement past state 0 appears to be driven by something external
  writing the state byte at `0x0800f438` (xrefs collected this session:
  read from `FUN_0800f0f2`, `FUN_0800f3a8`, `FUN_0800f3f6`,
  `FUN_0800e8e2` (5 read sites), `FUN_0800ecd6`, `FUN_0800ed6e`,
  `FUN_0800ef88`, `FUN_0800efe0`, `FUN_0800f004`, `FUN_0800f29c`, and
  `FUN_0800ea8c` itself (the state-4/state-6-standby handler) — none of
  these were checked this session for *writes*, only reads were
  enumerated), the next session should find what *writes* that byte (an
  IRQ handler is the leading theory) and checkpoint-trace whatever
  triggers that write, rather than continuing to dig into this specific
  loop body.

## Follow-up session (same day, part 4) — real GPIO bug found and fixed; display still not up

Chased the "what writes `0x0800f438`'s target state byte" lead from part
3 with `scripts/watch_state_byte.py` (new tool, same watchpoint-and-log
pattern as `watch_write.py` but loops and logs every hit instead of
stopping at the first). Real (patched) hardware showed the state byte
advancing `0x00 → 0x06 → 0x04` at three different PCs; QEMU (same
patched image) only ever wrote it once, during static `.data` init, and
never again in 75s.

Traced forward via `checkpoint.py` through the gate chain in
`FUN_0801051e` (the function containing the main superloop):
`FUN_0800ebc8` (`0x080105c6`, the SBF gate our standby patch also
touches) returns `r0=1` identically on both targets -- not the
divergence. The next gate, `FUN_0800ec7a` (`0x080105e6`), returned
`r0=0` (early-return path) on **both** targets, but `lr` differed
(`0x0800ec81` QEMU vs `0x0800eca9` HW) -- proof the two targets take
different internal branches inside that one function despite the same
final return value. Decompiled it: the deciding condition is
`(*GPIOC_IDR & 0xca00... ) && (GPIOC_IDR bit 1) && (GPIOD_IDR bit 13
via a second pointer)` -- direct register reads confirmed real hardware
has `GPIOC_IDR = GPIOD_IDR = 0xFFFFFFFF` (every pin high) while QEMU had
`GPIOC_IDR=0xdeff`, `GPIOD_IDR=0xfffe`.

**Root cause, confirmed and fixed**: `hw/misc/gnw_h7b0_gpio.c`'s
`gnw_h7b0_gpio_reset()` was unconditionally forcing GPIOC bit 8, GPIOC
bit 13, and GPIOD bit 0 low at reset -- explicitly flagged in the code's
own prior comment as an *unverified guess* (PC8) and a fragile compromise
kept out of fear of reintroducing an old, never-root-caused
"write-storm hang" (PC13/PD0). Real hardware register reads this session
prove all three are actually high. Removed the forced-low writes
entirely (with user sign-off given the known write-storm risk); rebuilt.
Re-checkpointed at `0x08010672` (the LTDC/display-init entry point,
reached from `FUN_0801051e` via the state-6 handler) -- **QEMU now
reaches it, matching real hardware's `lr` exactly** (previously
unreachable). Checkpointed one level deeper at `0x08018270` (an LTDC
layer-window register write) -- full register match on both targets.

**Still not fully unblocked**: checkpointing `0x08013704` (the actual
LTDC peripheral HAL-style init -- HSYNC/VSYNC/porch timing config,
distinct from the layer-window write above) shows real hardware reaches
it but **QEMU still times out and never does**, even with the GPIO fix
and the identical patched image. This is a second, separate divergence
from the GPIO one -- confirmed via direct memory read that QEMU's
`LTDC_GCR` (`0x50001018`) sits at `0x2220` (some polarity/config bits
set, `LTDCEN` bit 0 still clear) after the boot has been running freely
for several seconds, i.e. QEMU never reaches the real init call that
would set it. No display or audio yet on QEMU as a result.

**Immediate next step for a fresh session**: `0x08013704` has two
callers -- `0800ef02` and `0800fbfa` (the one real hardware actually took,
per its checkpoint `lr`). `0800fbfa` is inside `FUN_0800eb90`, a shared
state-transition dispatcher called from many places throughout boot with
a `param_1` = target-state argument (`switch(param_1)`, case 3 is the
path that eventually reaches the LTDC-init call). This means the real
gate isn't a simple reachability check -- it's whether `FUN_0800eb90` is
ever invoked with `param_1==3` on QEMU. `checkpoint.py`'s simple
first-hit breakpoint can't isolate that on its own (the function is
called constantly with other state values first); this needs either a
conditional breakpoint (break on `FUN_0800eb90` entry, filter on
`r0==3`, loop/continue past non-matching hits) or tracing *backward*
from a call site known to pass 3 (candidates: `FUN_0800e8e2`'s several
`FUN_0800eb90(uVar3)` calls where `uVar3` comes from
`FUN_080128f8`/`FUN_080128ee` -- neither decompiled yet). Two siblings
of `FUN_08013704`, `FUN_08013762`/`FUN_08013924`, share literal-pool
references to the LTDC base address `0x50001000` (`0800139a0`) and look
like HAL-style Init/DeInit/clock-config helpers for the same peripheral
(one writes what look like RCC PLL divider values `0x6c1`/`0xf80`/
`0x4d21`/`0xa000`/`0x44c` via `FUN_08001850`) -- worth decompiling next
too. Use `checkpoint.py`/new conditional-breakpoint tooling on this
chain, the same technique that found the GPIO bug -- do not go back to
single-stepping or guessing.

**Tooling note**: repeated `halt()`-then-read-PC calls without an
explicit `resume()` in between left a free-running QEMU instance
genuinely paused, producing a misleading "PC frozen" false positive
(caught mid-session, user flagged it) -- always pair every diagnostic
`halt()` with a `resume()` before closing if the instance needs to keep
running afterward, or better, use `checkpoint.py`'s breakpoint-based
run-to-address pattern instead of manual polling entirely.

**Also landed this session**: `scripts/make_zelda_patched_bank1.py`
(reproduces gnwmanager's exact standby-skip patch bytes against our own
`zelda-bank1.bin`, used to keep the real device from dropping into
standby mid-trace) and `scripts/boot_qemu.sh`'s new `--patched` flag
(boots the `-bank1-patched.bin` variant for a fair side-by-side against
patched real hardware) plus `-audiodev pa,id=snd0` /
`-global gnw-h7b0-sai1.audiodev=snd0` (QEMU was launching with no audio
backend at all, so SAI1's device model had nothing to actually play
through even when producing samples).

## Follow-up session (same day, part 5) — the efficient divergence-hunting playbook, and a second real fix

Part 4 found and fixed one real bug (GPIO forced-low defaults) but then
spent a long stretch chasing a plausible-looking but ultimately
irrelevant thread (an event-queue mechanism that turned out to already
match between targets once properly isolated). This section documents
what actually worked, as a playbook for next time -- the goal is to
recognize "the efficient path" faster, not rediscover it by trial and
error again.

### The playbook

1. **Never single-step over real hardware's SWD link.** Confirmed
   multiple times this project (part 2, and again this session): it's
   orders of magnitude too slow and produces misleading "both look
   stuck" false positives from coarse progress logging. Always use
   hardware breakpoints (`Z1`/`z1` over raw gdb-remote, `bp .. hw`/`rbp`
   over OpenOCD) and run-to-checkpoint, the same way `checkpoint.py`
   does.
2. **Checkpoint against the Ghidra-decompiled call graph, not arbitrary
   addresses.** Decompile the function containing the current point of
   interest, find its next `bl`/conditional branch, checkpoint the
   instruction right after -- read the return value in the argument
   register (`r0` for the first arg, per AAPCS) or the branch condition
   register directly.
3. **A checkpoint has three possible outcomes, each actionable
   differently:**
   - **Registers all match (including `lr`)**: this exact point is
     confirmed equivalent on both targets. Move the checkpoint further
     downstream (walk forward through the decompiled call graph) --
     don't re-verify things already proven identical.
   - **The "obvious" register matches but `lr` (or other registers) does
     not**: this is a real, load-bearing signal -- it means both targets
     reached the *same final value* through *different internal control
     flow* inside the callee. Decompile the callee itself; the
     divergence is a branch condition inside it, not at the call site.
     (This exact pattern -- `r0` matching, `lr` not -- is what led
     straight to both real bugs found this session, `FUN_0800ec7a` and
     transitively the ADC threshold tables inside it.)
   - **One target times out and never reaches the checkpoint at all**:
     this pinpoints a genuine control-flow fork upstream. Don't guess --
     find the actual conditional branch a few instructions earlier
     (`DisasmAt.java` on the surrounding range) and checkpoint its
     condition register instead. If the branch target itself is
     ambiguous, verify the *live* memory bytes at that address against
     the Ghidra disassembly with Capstone directly (`capstone.Cs(...)`)
     before trusting the static analysis -- confirms tooling isn't
     misreading a stale project.
4. **When a decompiled condition bottoms out in a peripheral MMIO read
   (GPIOx_IDR, ADC DR, etc.), stop reasoning about the C code and just
   read the live register on both targets.** This was the single fastest
   step of the whole session, twice: reading `GPIOC_IDR`/`GPIOD_IDR`
   directly (one line of Python) immediately confirmed real hardware
   reads `0xFFFFFFFF` while QEMU's reset defaults didn't -- turning a
   multi-function decompilation chase into a one-command fix
   confirmation. Same for tracing an ADC-derived RAM byte back to its
   actual peripheral source.
5. **A fixed/hardcoded stub value (a `#define` standing in for a real
   peripheral reading) is a prime suspect once you're this deep.** The
   session's second real bug (`GNW_H7B0_ADC_FULL_BATTERY_RAW`) was
   exactly this pattern: a constant tuned to satisfy one firmware's
   threshold check (retro-go-sd/gnw-chainloader, documented in the
   header's own comment) that had never been checked against a
   *different* firmware's own threshold logic (stock Zelda's 4-table
   battery-level lookup, `FUN_0800320e`). When a decompiled function
   compares a stub-supplied value against several unfamiliar magic
   constants, decompile enough of the surrounding logic to find the
   *largest* constant across all branches and make the stub value
   exceed all of them, not just the one branch you happened to test
   against originally.
6. **Kill and fully relaunch QEMU (not just reset) if breakpoint
   clears start failing with protocol errors, or a "reset" checkpoint
   result looks inexplicably different from a very similar earlier
   test.** Cortex-M cores have a small, fixed number of hardware
   breakpoint comparators; a session with many failed `z1` clears (from
   earlier tooling bugs, or from Ctrl-C'ing a script mid-checkpoint) can
   silently exhaust them, making later `Z1` sets fail or misbehave in
   ways that look exactly like a firmware-level "never reaches this
   point" divergence but are actually pure tooling debt. This wasted
   real time this session before being correctly diagnosed and fixed
   with a full QEMU relaunch.
7. **Keep every individual wait operation to a few seconds, but don't
   be afraid to re-run a checkpoint from a fresh reset.** `checkpoint.py`
   always resets from the real entry point, so a "timed out" result at a
   short timeout is a real, repeatable, cheap data point -- there's no
   value in one long wait when several short ones tell the same story
   faster and keep the session interactive.

### Root cause #2 found and fixed this session

Using the above, walked forward from the already-fixed GPIO gate through
`FUN_0800ec7a`'s deeper branch (`lr` mismatch despite `r0` match -- rule
3's middle case) to a battery-level threshold lookup
(`FUN_0800320e`, decompiled) fed by a fixed ADC stub value
(`GNW_H7B0_ADC_FULL_BATTERY_RAW`, `hw/misc/gnw_h7b0_adc.c` /
`include/hw/misc/gnw_h7b0_adc.h`). The constant (`13500`) cleared stock
Zelda's smaller "charging" threshold table but not its larger
"not-charging" table (max ~41974), causing QEMU to read battery level 0
where real hardware (with an actual battery) reads a real nonzero level
-- silently skipping an entire boot-progress branch. Bumped the constant
to `0xFFFF` (max 16-bit register value, clears every table in both
firmware families). Rebuilt, checkpoint-confirmed: `FUN_0800ec7a`'s
internal branch (`lr`) now matches real hardware exactly, and the
per-pass event-queue push (`FUN_0800e8e2` receiving a real nonzero event
instead of always `0`) now fires identically on both targets too --
neither matched before this fix.

**Not yet found**: real hardware's route to the still-unreached LTDC HAL
init (`0x08013704`) turned out *not* to be the `FUN_0800e8e2(1)` path
traced above (checkpointed and confirmed both targets already take that
identically, calling `FUN_0800eb90(1)` -- case 1, not the case-3 path
that leads toward `0x08013704`). The real trigger is a different,
not-yet-identified call to `FUN_0800eb90(3)` -- next session should
resume the same playbook from here: find every other caller of
`FUN_0800eb90` (`FindCallers2.java 0800eb90`, already run once this
session, gave four call sites all inside `FUN_0800e8e2` -- check the
other three), or work backward from real hardware's actual observed `lr`
at the `0x08013704` checkpoint (`0x0800fbff`, inside `FUN_0800eb90`) to
identify exactly which call produced it.

## Follow-up session (same day, part 6) -- OCTOSPI IRQ modeled (real fix), a firmware self-trap found, an ITCM lead opened but not closed

Picked up the `FUN_0800eb90(3)` thread from part 5. Disassembled the
three remaining call sites inside `FUN_0800e8e2` (`DisasmAt.java` on
`0800e960`-`0800e9fc`) and confirmed the `param_1==6` branch
(`0x0800e9a0: movs r0,#3` / `0x0800e9a2: bl 0x0800eb90`, gated on state
`5` and a sub-value in `{1,2}`) is the literal-3 call site matching
part 5's prediction.

**Correction to part 5's framing**: `checkpoint_filtered.py 0x0800e8e2
--reg r0 --value 6` showed QEMU's event dispatcher calls
`FUN_0800e8e2` exactly once (`r0=1`, already known-good) and then never
again within several seconds, while real hardware calls it a second
time with `r0=6` almost immediately. This looked like the `param_1==6`
gate itself was unreachable, but turned out to be a **false lead**:
walking the actual per-pass call chain (`FUN_080134e2`'s "poll GPIO/
build button-word" routine, called every superloop pass, checkpointed
via `checkpoint_filtered.py` at successively deeper addresses inside
it) showed QEMU tracks real hardware bit-for-bit through the *entire*
body of that routine, reaching its very last instruction before
`return` identically on both targets across 2 full passes. The actual
divergence is not in this loop at all -- it's a full CPU stall a few
instructions after `134e2` returns, inside the very next call
(`FUN_0800ef44`, an edge-detector that pushes queue events).

**Real, root-cause-confirmed finding**: forcibly halting QEMU after the
apparent "hang" and reading the live PC (`scripts/probe_hang_pc.py`)
showed the CPU is not actually frozen mid-instruction -- it's sitting at
`0x080164b8: b 0x080164b8`, a literal infinite self-branch. Disassembly
of the surrounding code (`0x080164b6: cbz r0,0x080164ba`) shows this is
**firmware's own deliberate "spin forever on HAL error" trap**, taken
because `FUN_0800e45c` (called from `0x080164b2`) returned a nonzero
error code. This is a real firmware behavior, not a QEMU crash --
confirmed by resuming and re-sampling PC every second for 5s and seeing
it genuinely never move (`scripts/resume_and_sample.py`).

Traced `FUN_0800e45c`'s error return to its first sub-call,
`FUN_080112b6`, which sends OCTOSPI command byte `0x66` (SPI-NOR RSTEN)
against a handle whose `Instance` field resolves (read live via
`scripts/identify_peripheral.py`) to `0x52005000` -- **OCTOSPI1**.
`FUN_080112b6` calls a deep, register-convention-heavy state machine
(`FUN_08011536`, decompiles with `unaff_r4`/`unaff_r5` -- Ghidra's
marker for registers the function *reads without ever explicitly
setting*, i.e. inherited/leftover register content, not real
parameters) that can reach a generic "wait for a flag with an optional
infinite timeout" helper (`FUN_08011bdc`) gated by OCTOSPI's CR
`TEIE`/`TCIE`/`FTIE`/`SMIE`/`TOIE` bits.

**Real fix landed**: `hw/misc/gnw_h7b0_ospi.c`/`.h` had no IRQ line at
all for OCTOSPI1/2 (`grep irq` returned nothing, and
`hw/arm/gnw_h7b0_soc.c` never connected one) -- real hardware has
`OCTOSPI1_IRQn=92`/`OCTOSPI2_IRQn=150` (confirmed against
`sdk/cmsis-device-h7/Include/stm32h7b0xx.h`). Added a real, level-
sensitive IRQ (`ospi_update_irq()`, re-evaluated on every SR- or
CR-affecting write, mirroring real hardware's "any unmasked SR bit ->
line high" semantics) and wired both instances to NVIC in
`gnw_h7b0_soc.c`. This also required bumping `armv7m`'s `num-irq`
property from `96` to `160` (NVIC must be sized in multiples of 32;
`OCTOSPI2_IRQn=150` didn't fit in 96 and tripped a `qdev_get_gpio_in`
assertion on boot until fixed). This is correct, real hardware behavior
worth keeping regardless of the outcome below.

**Did not fix this specific stall**: rebuilt and retested -- QEMU still
traps at the same `0x080164b8` address. Checkpointing `FUN_08011536`'s
entry directly (`scripts/probe_11536_state.py`, reading `r0`-`r3`,
`sp`, `lr`, `r4`, `r5`, the state fields those point at, OSPI1
`CR`/`SR`, and NVIC `ISER`/`ISPR` for IRQ 92, on both targets at the
identical checkpoint) showed every core register bit-for-bit identical
between QEMU and real hardware (confirming this really is the same
traced RSTEN call site) -- so the interrupt-wait theory was a wrong
branch of the trace; `FUN_080112b6` fails before ever reaching the
deep SR-polling chain.

**A real, confirmed-but-unexplained divergence, and a false fix
attempted and reverted**: the one thing that *did* differ at that exact
checkpoint was dereferencing `r5` (`0` on both targets --
`unaff_r5`, leftover/inherited register content per above): QEMU read
all zeroes at address `0x0`, real hardware read genuine code-like bytes
(`0x5ff0e92d`, `0x4608bf18`). Hypothesized this meant QEMU was missing
real hardware's boot-time flash-at-address-0 remap, and added a
`flash_bank1` alias at `0x00000000` in `gnw_h7b0_soc.c` -- **this was
wrong and was reverted**. `ITCM_BASE_ADDRESS` is *already*
`0x00000000` in this SoC's existing, correct design (an intentional,
pre-existing region, not an oversight); the new alias silently shadowed
it instead of fixing anything, and rebuilding+retesting confirmed it
changed nothing about the actual stall. With the alias reverted, QEMU's
genuine (pre-existing) ITCM region reads back to all zeroes at this
checkpoint, while real hardware's address 0 still shows the same
code-like bytes -- consistent with real firmware copying a startup/
hot-code section into ITCM early in boot (a common STM32H7 pattern:
relocate hot code from flash to faster ITCM RAM, which our SoC's
existing ITCM-as-RAM-at-0 model should reproduce correctly once that
copy loop runs identically on both targets) and QEMU's copy either not
having run yet, not running at all, or running differently by this
point -- not yet investigated.

**Important methodological correction for next session**: because
Ghidra flagged `r5` as `unaff_r5`, its value at this checkpoint is
*not* a deliberate parameter set by visible code in this function or
its immediate caller -- it's leftover content from whatever last wrote
register `r5` further back in the call chain. The `r5[0xe]` difference
found this session is therefore a **downstream symptom of an earlier
divergence**, not the root cause itself. Chasing it further requires
walking backward to find what last touched `r5` (or ITCM's actual
population point) before this call, not reasoning further about this
checkpoint in isolation.

**Concrete next steps**:
1. Confirm whether QEMU's firmware ever executes an ITCM-populating
   copy loop at all (a `.itcm` linker-script section copy is the most
   likely mechanism) -- watch for writes to the `0x00000000`-`0x0000ffff`
   range (`scripts/watch_write.py`-style) on both targets from the real
   entry point forward, and compare when/whether it happens.
2. If both targets do run a copy loop, checkpoint its exact source
   (flash address) and destination bounds on both targets to see if
   they copy the same bytes -- a source-address or length mismatch
   would explain diverging ITCM content directly.
3. If QEMU's copy loop never runs at all, walk backward from its
   expected call site (likely early in `FUN_0801051e`'s subsystem-init
   chain, already checkpointed and confirmed matching in part 2 --
   double check whether the ITCM copy is even inside that already-
   verified range, since if so the divergence must be even earlier or
   entirely elsewhere) to find the real gating condition.
4. New tooling this session, useful for future checkpoints needing full
   context beyond `checkpoint.py`'s default `r0`-`r3`/`sp`/`lr`:
   `scripts/probe_11536_state.py` (reads arbitrary registers +
   dereferences them + NVIC/peripheral state at a single checkpoint on
   both targets) and `scripts/resume_and_sample.py` (resume + repeated
   halt-and-sample-PC, to distinguish a genuine CPU stall from a
   breakpoint-detection artifact).

## Follow-up session (same day, part 7) -- ITCM-copy lead resolved as a downstream symptom, not a new bug

Picked up part 6's "confirm whether QEMU ever runs an ITCM-populating copy
loop" next step with `watch_write.py` (watching a single 4-byte word at
`0x00000000`, reset-to-reset, on both targets):

- **QEMU**: no write to `0x00000000` observed within 10s (or 25s) of a
  fresh reset -- the address genuinely never gets written on QEMU's side
  of this boot.
- **Real hardware**: a write hits almost immediately, `PC=0x08003ae6` --
  inside `FUN_08003ae0`, a generic word/halfword/byte-tail `memcpy`
  (decompiled: `void FUN_08003ae0(dst, src, len)`, not itself
  address-specific).

Used `checkpoint_filtered.py 0x08003ae0 --reg r0 --value 0` (breaking at
the memcpy's entry, filtering on `r0==0` i.e. `dst==0x0`) to isolate the
*specific* call that targets address 0 out of the many generic-memcpy
calls during boot:

- **QEMU**: zero calls to `FUN_08003ae0` *at all* (any `r0`) within 25s
  of a fresh reset.
- **Real hardware**: 4th call site matches, `r0=0x00000000,
  r1=0x9030c388, r2=0x00001e70, lr=0x0800f7b7` -- i.e.
  `memcpy(0x00000000, 0x9030c388, 0x1e70)`, copying 7792 bytes from
  OCTOSPI1's memory-mapped external flash region (`0x90000000` base,
  offset `0xc388`) into address 0.

**Conclusion: this is not a new, independent bug -- it's downstream of
the still-open part 6 divergence.** The caller (`lr=0x0800f7b7`) is
reached only *after* the OCTOSPI1 RSTEN command chain
(`FUN_0800e45c`→`FUN_080112b6`→`FUN_08011536`) succeeds; on QEMU that
chain still fails and firmware still hits its `b .` self-trap at
`0x080164b8` (part 6, unresolved), so QEMU's execution never reaches far
enough to attempt this copy at all -- consistent with (not contradicting)
part 6's "QEMU never calls `FUN_08003ae0` with `r0==0`" result. **Do not
chase the ITCM-copy thread further as its own lead; it will resolve
automatically once the RSTEN failure is root-caused.** Removes one item
from part 6's next-steps list (ITCM copy loop existence is now
confirmed, and shown to be irrelevant on its own).

**Narrowed the RSTEN failure further**: decompiled `FUN_080112b6`
(issues OSPI command `0x66`/RSTEN via `FUN_08011536(handle, cmd, 5000)`,
returns error if nonzero) and `FUN_08011536` itself. The latter's
control flow (raw register-convention style, `unaff_r4`/`unaff_r5`
still) branches on `unaff_r4[0x15]` (a state field) and, on one path,
`*unaff_r5` / `unaff_r5[0xe]` -- **`unaff_r5[0xe]` is very likely the
same low-memory dereference flagged in part 6** (`r5==0` on both
targets, but the *content* at the small offset it reads differs:
all-zero on QEMU, real-code-like on real hardware). This function's
branch choice therefore still depends on that same pre-existing
low-memory-content divergence, not a new one. `FUN_08011c26`
(the success-path callee) is pure register-field setup for the OSPI
command descriptor, no polling; `FUN_080115d0`→`FUN_08011bdc` is the
actual SR-flag wait loop, not yet reached in this trace.

**Concrete next step for a fresh session**: stop treating the ITCM copy
and the `r5[0xe]` dereference as two separate leads -- they're the same
one. Find *what* real hardware has at that low address before this
checkpoint (some code/data already resident at boot, independent of the
`0x00000000`-targeted memcpy found this session, since that memcpy
happens later/downstream) and *why* QEMU's low memory reads zero there
instead. Candidates worth checking directly against `rm0455.pdf`/real
hardware register reads rather than more decompilation: (a) STM32H7's
boot-address aliasing (`BOOT_ADD0`, RM0455 §2.6) determines the *initial
PC/SP fetch* address (`0x08000000` here, not address 0, per the option
byte's ST-programmed default) -- so this isn't about the reset vector
fetch itself; (b) more likely there's an earlier, one-time `.itcm`-style
section copy in the compiler-generated startup code (before/alongside
the already-checkpointed constructor-dispatcher in part 2) that QEMU's
memory model should reproduce identically once found -- watch for
*any* write to the `0x00000000`-`0x0000ffff` range (not just address 0
itself) from the true reset entry point forward on both targets, and
diff timing/source-address against Ghidra's decompilation of whatever
`Reset_Handler`/pre-`main` code performs it, the same
checkpoint-per-call-site technique that found every other real bug this
session.

## Follow-up session (same day, part 8) -- narrowed further: not the memcpy, not r4[3], down to OSPI SR flag history and a low-flash-address descriptor read

Re-ran `probe_11536_state.py` fresh (part 6's tool) at `FUN_08011536`'s
entry to re-anchor with live data instead of old notes:

- **Every core/state register matches exactly** between QEMU and real
  hardware at this checkpoint: `r0`-`r3`, `sp`, `lr`, `r4`
  (`0x2001af3c`), `r5` (`0x00000000`), `r4[0x15]` (state `2`), `r4[0x16]`
  (`0`). Also checked `r4[3]` directly (`0x01000000` on both) --
  rules out the `iVar2==2 && unaff_r4[3]==0x4000000` early-error branch
  in `FUN_08011536`'s decompile as the divergence; both targets take the
  same fall-through path into `FUN_080115d0`->`FUN_08011bdc` (the actual
  SR-flag wait helper, not yet checkpointed).
- **The one thing that does differ: OSPI1 `SR`.** QEMU reads
  `SR=0x00000000`; real hardware reads `SR=0x00000004` (`TCF` set) at
  this exact identical checkpoint, despite `CR=0x00000301` matching
  exactly on both. This means real hardware has residual `TCF` set from
  some *earlier* OSPI1 transaction that QEMU's model never produced,
  even though the CPU-side control flow reaching this point is
  bit-for-bit identical -- a genuine peripheral-model gap, not (yet)
  proven to be the actual cause of the later trap, but a real,
  reproducible divergence worth chasing on its own.
- **Ruled out the part-7 ITCM-targeted `memcpy` (`lr=0x0800f7b7`,
  `dst=0x0`) as the source of the part-6 `r5[0xe]`/low-memory-content
  divergence.** Checkpointing that memcpy mid-loop (`watch_write.py`
  on a 1KB window at `0x0`) shows its caller (`lr=0x0800f7b7`) is a
  *different*, later function than the one containing the `0x080164b8`
  self-trap (`0x080164cc`'s own local `FUN_08003ae0` call targets a
  *stack* buffer, not address 0) -- and this ITCM-targeting memcpy is
  itself gated on the RSTEN chain succeeding first, so it cannot be
  what populates the low-flash-like content `FUN_08011536` already
  observes *before* RSTEN is even attempted. **The real source of that
  pre-existing low-address content (real hardware reads `*0x0=
  0x5ff0e92d`, `[0x38]=0x4608bf18`, code-like; QEMU reads zero) is still
  unidentified** -- it predates this checkpoint entirely, so it must
  come from something earlier in boot (plausibly a compiler-generated
  `.itcm`/startup section copy in `Reset_Handler`, still not isolated
  despite two sessions' worth of narrowing).

**Revised, more precise next step**: two independent-looking threads
converge on the same "what is ITCM's real content before RSTEN is
attempted" question:
1. `FUN_08011c26` (the OSPI command-descriptor setup function, already
   decompiled) dereferences its second parameter (`unaff_r5`, `=0x0`
   here) at offsets up to `0x48` -- i.e. it reads fields directly from
   the low address range whose content differs between targets. If this
   parameter is genuinely meant to point at a real static
   command-descriptor table (not actually null), the "correct" address
   is almost certainly a low **flash** address that Ghidra's raw
   register recovery is showing as a bare small integer/offset rather
   than resolving to its true `0x08000xxx`-range home -- worth
   re-examining this function's true calling convention (may be called
   with `r5` relative to some base, or Ghidra mis-attributed which
   register truly holds the pointer) before concluding this is really
   about ITCM content at all.
2. Regardless of (1), directly find *what writes* the low
   `0x00000000`-`0x0000ffff` range **before** the `FUN_08011536`
   checkpoint on real hardware (not after, as this session's `memcpy`
   traces covered) -- `watch_write.py`'s range-watch (confirmed working
   this session with lengths up to 64KB on OpenOCD, though QEMU's gdb
   watchpoint clear on a >4-byte range logs a harmless "error 22") is
   the right tool; the outstanding gap is a run watching that full range
   from the *true reset entry point* (not from the `FUN_08011536`
   breakpoint) with a long enough timeout to catch whatever early
   startup write populates it, then checkpoint that write's `lr` in
   Ghidra the same way every other lead this session was resolved.

**Tooling correction**: OpenOCD's `wp` command against this board's
`hla_target` (ST-Link) *silently fails to register* watchpoints larger
than 4096 bytes -- `wp 0x0 65536 w` returns success but a follow-up `wp`
(list) shows nothing armed, and `resume`+`wait_halt` then times out with
no hit no matter how long you wait (a *false negative*, not a real "no
write happened" result). 4096 bytes is the real cap on this adapter.
Confirmed by successfully arming and hitting a 4096-byte watch at the
same address. Any future range-watch on real hardware over OpenOCD must
stay at or under 4KB, or explicitly verify with a `wp` list command that
the watchpoint actually took before trusting a timeout.

## Follow-up session (same day, part 9) -- critical correction: the whole `r5[0xe]`/ITCM-content thread was a stale-SRAM testing artifact, not a QEMU bug

Re-ran the range watch from part 8's next-step, this time correctly (4KB
window, from the *true* reset entry point via `halt_at_entry`/
`reset_and_run_to`, with the watchpoint's registration explicitly
verified via `wp` before resuming). Result: the very first write real
hardware ever makes to `0x0`-`0xfff` is the *same* downstream `memcpy`
(`lr=0x0800f7b7`, `dst=0x0`, `src=0x9030c398`+, part of the `0x1e70`-byte
copy already found in part 7) -- there is no earlier, separate
`.itcm`-section startup copy. This means the "code-like bytes" `FUN_
08011536`/`FUN_08011c26` read at low addresses *before* that memcpy ever
runs cannot have been written by this boot run at all.

Directly inspected live memory at that checkpoint across the full
`0x0`-`0x1e70` span (`mdw` at `0x0`, `0x38`, `0x100`, `0x1000`,
`0x1e60`): **non-zero, code-like content at every address checked, right
up to the exact `0x1e70` boundary the later memcpy writes.** This is the
unmistakable signature of *leftover SRAM content from a previous
successful run of that exact memcpy* -- not hardware boot-time content,
not a flash alias (checked `rm0455.pdf` directly: `0x00000000`-
`0x0000FFFF` is documented as plain 64KB ITCM RAM, no boot-time
flash-mirroring behavior described anywhere in the memory-map section).
**SWD/`nSRST`-based resets (everything `reset_and_halt()`/
`reset_and_run_to()` do) do not clear SRAM contents** -- only a genuine
power cycle does. QEMU is a fresh process every launch (correctly
zeroed ITCM, faithfully modeling real cold power-on-reset), while the
physical board has been soft-reset dozens of times across parts 2-9 of
this session without ever being power-cycled, so it's been reading back
its own *previous* successful boot's leftover ITCM content the entire
time.

**This means the entire `r5[0xe]`/`unaff_r5` "low-memory content
divergence" thread spanning parts 6, 7, and 8 was very likely chasing a
real-hardware testing artifact, not a genuine QEMU behavior gap.**
Exactly the same pattern already flagged once before this project
(see the "scratch-buffer checksum pitfall" memory: verify what a traced
address actually represents before treating observed variance as a
bug) -- recurring here across three sessions before being caught. This
does **not** retroactively invalidate the session's actual *landed
fixes* (GPIO reset defaults, ADC battery threshold, OCTOSPI1/2 IRQ line)
-- those were each independently confirmed via direct, unambiguous
register/behavior comparisons, not via this stale-memory-content
signal. It does mean **the real cause of the `0x080164b8` self-trap
(OCTOSPI1 RSTEN failing on QEMU) is still completely open** -- the
`r5[0xe]` lead that seemed to explain it should be discarded, and the
actual RSTEN-vs-real-hardware behavioral difference needs to be
re-investigated from scratch, most likely inside `FUN_08011bdc` (the
actual SR-flag wait/timeout helper, still never checkpointed this
session) rather than anywhere upstream of it.

**Concrete next steps for a fresh session**:
1. If practical, verify this hypothesis directly: power-cycle the
   physical device (not just SWD-reset it) and re-check whether address
   `0x0`'s content is genuinely zero/uninitialized-garbage-but-
   *different* immediately after a real cold boot, before this memcpy
   has had a chance to run in the new session. If it reads plausible
   *different* garbage (not the same `0x5ff0e92d...` bytes as before),
   that's conclusive confirmation of the stale-SRAM explanation.
2. Stop reasoning about `FUN_08011536`'s `r5`/`unaff_r5` dereference as
   load-bearing for the RSTEN failure -- it was never established that
   real firmware's actual control flow depends on that value in a way
   that matters (the function's true return-value semantics through
   Ghidra's `unaff_r4`/`unaff_r5` recovery were never fully confirmed
   either).
3. Re-focus on `FUN_08011bdc` (`FUN_080115d0`'s sole callee, the actual
   generic "wait for an OCTOSPI SR flag with an optional infinite
   timeout" helper gated by `TEIE`/`TCIE`/`FTIE`/`SMIE`/`TOIE`, per part
   6) -- checkpoint *inside* it (not just its caller) on both targets to
   see what SR bit it's actually polling for and whether QEMU's OSPI
   model ever sets that specific bit for this specific transaction, now
   that the OCTOSPI1 IRQ line exists (landed part 6) but evidently still
   isn't enough on its own.
4. Keep the newly confirmed 4KB OpenOCD watchpoint-size cap in mind for
   any future real-hardware range-watch script.

## Follow-up session (same day, part 10) -- RSTEN trap root-caused and fixed (OSPI auto-polling), OTFDEC modeled, and a major methodology discovery about the physical device's firmware

### Root cause #3 found and fixed: OSPI automatic status-polling (SMF) was unmodeled

Checkpointed `FUN_08011bdc` (the HAL wait-for-SR-flag helper, decompiled
this session: polls `(mask & *(handle->Instance+0x20)) != 0 == want`
with a tick-based timeout, sets ErrorCode and returns 1 on expiry)
per-hit on both targets, reading `r1` (flag mask) / `r2` (wanted state)
/ live OSPI1 `SR` at each hit. Hits 0-6 pass on both targets; **hit 7
(call site `lr=0x080117ab`, identical on both) waits for `SMF` (mask
`0x8`, the auto-polling status-match flag) to set. Real hardware:
`SR=0x0c`, SMF set, wait passes. QEMU: `SR=0x06`, SMF never sets, the
5000ms timeout expires, error propagates, firmware traps at
`0x080164b8`.** The QEMU OSPI model had no auto-polling (CR.FMODE==2)
support at all.

**Fix landed** (`hw/misc/gnw_h7b0_ospi.c`/`.h`): on IR/AR command
trigger with `CR.FMODE==2`, generate the status bytes via the existing
data-phase byte generator and set `SR.SMF` when
`(status ^ PSMAR) & PSMKR` matches (AND mode, or any-bit for
`CR.PMM`=OR mode) -- a single evaluation at trigger time is exact for
this model since its status responses are immediate and constant.
Rebuilt, retested: the `0x080164b8` trap is gone.

### Root cause #4 found and fixed: OTFDEC KEYCRC (new device model)

With RSTEN passing, boot advanced to the *next* firmware self-trap in
the same init function (`b .` at `0x08016514`): `FUN_08019f98` --
identified via its register usage (base `0x5200B800` = **OTFDEC1**) and
the HAL source as `HAL_OTFDEC_RegionSetKey()` -- writes the 4 key words
then compares its own software-computed key CRC
(`HAL_OTFDEC_KeyCRCComputation()`, exact algorithm in
`sdk/stm32h7xx-hal-driver/Src/stm32h7xx_hal_otfdec.c`) against the
hardware's `CONFIGR.KEYCRC` readback. OTFDEC1/2 were
`create_unimplemented_device` (reads-as-zero), so the compare always
failed. **Fix landed**: new minimal device model
`hw/misc/gnw_h7b0_otfdec.c`/`include/hw/misc/gnw_h7b0_otfdec.h`
(read/write shadow regs; writing a region's KEYR3 recomputes the HAL
CRC-7-based key CRC into that region's `CONFIGR[15:8]`), both instances
wired into the SoC at `0x5200b800`/`0x5200bc00` (Kconfig/meson/soc.h/
soc.c). Rebuilt, retested: SetKey passes; boot advances further.

**Known, explicitly-not-yet-modeled gap**: OTFDEC's actual job --
decrypting memory-mapped OCTOSPI reads (AES-CTR) -- is NOT implemented;
the new model is a KEYCRC-readback-only stub (pass-through data).
How much this matters differs per game: *Mario's* stock extflash dump
is genuinely encrypted (entropy ~7.997, hence
`gnw-mario-decomp`'s decrypt tooling), while *Zelda's*
`flash_backup_zelda.bin` is already plaintext (entropy ~1.5, confirmed
in an earlier session) -- so for Zelda, pass-through may actually be
correct-enough for regions firmware expects to read decrypted, but
this hasn't been verified (if stock Zelda firmware still routes reads
through an enabled OTFDEC region expecting a decrypt transform, a
pass-through would *corrupt* already-plaintext data instead). Verify
which regions Zelda's `REG_START_ADDR`/`REG_END_ADDR` actually cover
and whether CONFIGR ever enables deciphering before assuming either
way; Mario will genuinely need real AES-CTR (or a pre-decrypted image
decision) eventually.

### Where QEMU stands now: a legitimate WFI wait, not an error trap

With both fixes, QEMU's stock-Zelda boot now leaves the entire
OSPI-reset/OTFDEC-key init sequence behind and parks at `0x0800e5a8`
inside `FUN_0800e590`: `while (*(int*)(r5+0x5c) == 0) WaitForInterrupt();`
-- a real interrupt-driven wait (flag address `0x2000ad40`), reached
right after `FUN_08011f34`. This is genuine progress: firmware is no
longer in any error path, it's waiting for an IRQ QEMU doesn't fire
yet. Ghidra xrefs: only two writers of `0x2000ad40` -- `FUN_0800e5c8`
(clears it before the wait) and `FUN_0800e5dc` (sets it to 1), the
latter called from exactly one place, `0x080124ce`, the tail of a
buffer/FIFO-drain routine (Ghidra has no function boundary there;
Capstone disasm shows it draining a word array at `[r4+0x30]` indexed
by halfword counters `[r4+0x38]`/`[r4+0x3a]`). At the stall, QEMU's
OSPI1 is already fully configured into memory-mapped mode
(`CR=0x30400301`, FMODE=3, no IE bits) with nothing pending -- so the
awaited interrupt is NOT an OSPI one. Stock vector table's non-default
handlers (extracted this session): IRQn 3, 6, 8, 17, 18, 23, 41, 47,
69, 79, 82, 87, 88, 89, 90, 92, 116 -- one of these (via the shared
`0x0801adXX` thunks) eventually reaches `0x080124ce`. Next session:
decompile `FUN_08011f34`'s callees (`FUN_08012042`/`FUN_08012034`/
`FUN_08012116`) to identify which peripheral operation it kicks off,
and/or force a function boundary at the `0x080124xx` drain routine
(`CreateFunctions2.java`) and walk its callers up to the IRQ handler
thunk to identify the missing interrupt.

### Major methodology discovery: the physical device is running gnwmanager's FULL patch set, not our 2-byte-patched stock image

While tracing the WFI flag on real hardware, the writer's PC came back
as `0x0801bf1c` -- an address *beyond stock firmware's end* (last
non-0xFF byte in the stock dump: `0x1b6d4`; gnwmanager's own
`STOCK_ROM_END = 0x1B3E0`). A full 128KB diff of live device flash vs
the stock dump found **~100 difference ranges**, which map exactly onto
`gnwmanager/gnwmanager/cli/gnw_patch/zelda.py`'s complete patch list:
the reset vector at `0x4` replaced with gnwmanager's own bootloader,
all four save-encryption/decryption skips (`0xB528`/`0xB5C4`/`0xF12C`/
`0xF222`/`0x13ED8`/`0x13F52`), the `read_buttons` hook at `0xFE54`, our
two standby patches (`0xEAA0`/`0xEBD0`), added code above the stock ROM
end -- **and, critically, OTFDEC disabled entirely (`nop`s at
`0x16536`/`0x1653A`/`0x1653C`)**, because gnwmanager's CFW decrypts
extflash offline. The physical device was evidently flashed with
`gnwmanager flash-patch` (the full CFW), not with our
`make_zelda_patched_bank1.py` output.

**Correction from the project owner (part 11)**: the pivot to the
fully-patched CFW image was *intentional* from the beginning -- the
repo-root `zelda-patched.7z` (present since 2026-07-10) holds the exact
image pair (`patched-zelda-bank1.bin` + `zelda_extflash_patched.bin`)
the physical device was flashed with, and QEMU was always supposed to
boot that same pair. That intent was lost across session summaries;
QEMU had instead been booting a stock image with only the 2-byte
standby patch (and the stock 64MB extflash). The hardware-vs-image diff
above is therefore not a methodology problem with the *device* -- it
was QEMU booting the wrong image.

Consequences, carefully scoped:
- **Past lockstep results are NOT retroactively invalidated wholesale**:
  every comparison so far ran through code regions the patch set
  doesn't touch (verified implicitly by the bit-for-bit register
  matches themselves), and the two behavioral patches we knew about
  (standby skip) were identical on both sides.
- **But real hardware can no longer serve as the stock-behavior
  reference for**: anything OTFDEC-related (disabled on device),
  anything near the reset vector/bootloader handoff, the save
  encryption/decryption paths, `read_buttons`, or any code above
  `0x1B3E0`. The WFI-flag writer traced on hardware this session
  (`0x0801bf1c`) is gnwmanager patch code, NOT stock -- which is why
  the stock writer had to be found via Ghidra xrefs instead (done,
  above).
- **For future sessions**: QEMU and the device now both run the
  `zelda-patched.7z` image pair -- lockstep comparisons are
  apples-to-apples again. Stock-firmware-accuracy questions (the
  original "boot the unmodified dump" goal) are a separate track that
  would need the stock image on both sides.

## Follow-up session (same day, part 12) -- IT BOOTS. Zelda CFW reaches visible display output in QEMU

Extracted the repo-root `zelda-patched.7z` image pair, verified
`patched-zelda-bank1.bin` byte-for-byte identical to the physical
device's live internal flash over SWD, installed both as
`backup/qemu-images/zelda-bank1-patched.bin` /
`zelda-extflash-patched.bin`, and updated `scripts/boot_qemu.sh
--patched` to boot the pair (previously: 2-byte-patched stock bank1 +
stock 64MB extflash). Also repointed every tracing script's
entry-point source (`BANK1_PATH`) at the patched bank1, since the CFW
replaces the reset vector with gnwmanager's bootloader.

With this pair plus this session's two device-model fixes (OSPI
auto-polling `SMF`, OTFDEC `KEYCRC`), **QEMU boots stock-patched Zelda
to visible display output** -- user-confirmed on screen ("it's
working"). `LTDC_GCR=0x10002221` (`LTDCEN=1`), firmware settles into
the same steady-state PC (`0x0801042c`) real hardware idles at.

Remaining follow-ups (not blockers, recorded for the next session):
1. The earlier `while (!*0x2000ad40) WFI;` wait was cleared by the
   correct image pair (gnwmanager's patch code services it), but the
   flag reads 0 in steady state -- fine, it's a per-operation
   completion flag, not a latched one.
2. OTFDEC still does no real decryption (KEYCRC-only stub) -- works
   here because gnwmanager's CFW disables OTFDEC and ships decrypted
   extflash; genuinely-stock Mario/Zelda (encrypted extflash, OTFDEC
   active) will need real AES-CTR in the OSPI memory-mapped path.
3. True-stock boot (the unpatched dump, the project's original goal)
   still parks at the old WFI wait on QEMU -- whatever stock's own
   flag-setter IRQ chain is (the `0x080124xx` drain routine) remains
   untraced; pick up from part 10's notes if/when stock accuracy is
   the goal again.
4. Audio/input/gameplay beyond the boot screen not yet exercised.

## Follow-up session (same day, part 13) -- input works end-to-end; current blocker is the audio-DMA drain chain stalling after one buffer

Landed this part (all rebuilt + live-verified unless noted):

1. **Configurable keyboard map**: `-global gnw-h7b0-gpio.keymap=a=z,time=f1,...`
   (new `keymap` qdev property on `gnw-h7b0-gpio`; button names
   pause/game/time/a/b/left/down/right/up/pwr/start/select, key names are
   QKeyCode names; parsed at realize with clear startup errors).
2. **Buttons now generate real EXTI interrupts**: `gnw_h7b0_gpio_set_button()`
   forwards the (active-low) pin level to `gnw_h7b0_exti_set_line()`, gated by
   SYSCFG_EXTICRx's per-line port mux (GPIO got a `syscfg` pointer from the
   SoC for this). Verified: the boot screen arms only EXTI line 0 falling
   (PA0=POWER) -- pressing P wakes stock/CFW firmware out of its
   `0x0801042c` idle WFI loop, previously impossible.
3. **TIME button is dual-wired PC5 + PA2 (WKUP2)**: stock's `read_buttons`
   (`FUN_08016808`, decompiled -- reads GPIOC/GPIOD IDR bitwise, all pins
   matching game-and-watch-patch's `stock_firmware_common.h` table) samples
   TIME from **PA2** in its default mode and only from PC5 when a mode byte
   (`0x2000ab92`) is 1. Button table extended to drive both pins; verified:
   TIME presses now register (user-confirmed -- the firmware *reacts*, see
   blocker below).
4. **SAI1 DMA stream no longer hardcoded**: was compile-time stream 0
   (retro-go's DMA1 Stream0); stock Zelda routes SAI1_A via DMAMUX1 ch14 =
   DMA2 Stream6. New `gnw_h7b0_dma_set_request_notifier()` API binds SAI1's
   sample-drain notifier + rate fn by DMAMUX request ID (87 = sai1_a_dma),
   re-resolved on every DMAMUX CxCR shadow write. (DMAMUX block writes are
   now stored raw instead of through DMA1's write-mask table.)
5. **SD SPI-mode fix** (via subagent, from a colleague's report): v11.0.2's
   `sd_cmd_SEND_OP_COND()` left SPI-mode cards in `sd_ready_state`, so
   CMD17/CMD55 were rejected unless the driver happened to read CSD/CID
   first (retro-go-sd's SDHC path doesn't). Now goes straight to
   `sd_transfer_state` -- gated on OCR power-up-complete so an enquiry
   ACMD41's polling loop still sees idle until power-up finishes.

**Current blocker (open)**: after POWER, firmware runs a 4-state async
transition state machine (`FUN_0800dc8c` advance callback; superloop WFIs in
`FUN_0800dd04` until state 4/0). The transition's first async op
(`FUN_0800a142`) is audio playback (boot chime/melody): the audio engine
(`FUN_080182a4` init: 48kHz, 16 buffers through two LDREX/STREX rings --
push `FUN_0801a12c`, pop `FUN_0801a180`, ring header `0x2001a5cc`) feeds
DMA2 Stream6 one 240-halfword one-shot at a time from the DMA TC interrupt
(IRQ 69, thunk `0x0801ae04` -> `bl FUN_08010888` (returns
`*(0x2001a518+0x80)`, an audio-ctx field) -> `b.w 0x0801a810`, a large
custom (non-HAL) audio mixer/DMA-complete dispatcher). On QEMU exactly ONE
transfer completes (TCIF was cleared, one ring entry consumed, ring then
frozen at count 15) and the stream is never re-enabled (`SxCR.EN=0`
permanently, SAI `DMAEN` still set), so the transition never reaches its
completion callback and the superloop WFIs forever -- this is the "froze
when I pressed TIME" symptom, and the same thing happens on the POWER
transition (sometimes it completed on earlier runs -- timing-sensitive,
suggesting a race between our DMA tick pacing and the dispatcher's
expectations, or one of the dispatcher's context checks reading a value our
models get wrong).

**PC-sampling gotcha for this area**: samples landing in
`0x08010750-0x08010776` are `SCB_CleanInvalidateDCache()` (512 set/way MMIO
writes per call, called once per audio-buffer batch) -- it dominates
sampling because it's QEMU-expensive, and looks like a hang but is (when
the pipeline is healthy) just per-batch cache maintenance.

**Next step**: lockstep-compare the IRQ-69 dispatcher (`0x0801a810` entry,
or the thunk `0x0801ae04`) per-hit against real hardware -- both targets
now run the byte-identical CFW image, so `checkpoint_filtered.py`-style
per-hit register comparison applies directly; needs the user to press
POWER on the physical device in sync with QEMU's QMP press. Alternatively
decompile `FUN_0801a810`'s termination/restart decision statically (it
reads `[audio_ctx+0x34+0x24]` descriptor chains -- nontrivial but
self-contained). The dispatcher's restart path presumably rewrites
`M0AR`/`NDTR` and sets `SxCR.EN` -- find what condition it checks before
doing so, and which of our model's register values fails it.

### Part 13 resolution -- both blockers root-caused and fixed

1. **DMA double-buffer mode (SxCR.DBM) was unmodeled** -- the one-shot
   path cleared EN after the first transfer. Stock's audio engine runs
   DMA2 Stream6 in DBM (its custom, non-HAL DMA dispatcher
   `FUN_0801a810` refills the inactive MxAR from the TC interrupt and
   expects hardware to keep streaming with EN set, toggling CT each
   completion). Fixed in `gnw_h7b0_dma.c`: DBM now behaves like CIRC
   plus a CT toggle per transfer-complete, and the stream notifier
   reads M1AR when CT was targeting it. Verified: audio ring cycles
   continuously, user hears the chime/melody (pitch wrong, see below),
   power-on transition completes.
2. **DMA2D interrupt line ignored 4 of its 6 flags** -- only
   TCIF/TEIF gated the IRQ. Stock's palette-fade display transitions
   start a background CLUT load (`BGPFCCR.START`) with `CR.CTCIE`
   enabled and sleep on the CLUT-transfer-complete interrupt; CTCIF was
   set but never raised the line, hanging every fade (the "froze when I
   pressed TIME" symptom, and intermittently the power-on transition).
   Fixed in `gnw_h7b0_dma2d.c`: all six ISR bits (0-5) now pair with
   CR's IE bits (8-13). Verified: TIME transition runs to state 4
   (complete), firmware responsive afterward.

**Known remaining issue (next up)**: audio plays noticeably slow and
low-pitched -- the SAI sample-rate derivation
(`gnw_h7b0_sai1_get_rate_hz()`, MCKDIV/OSR off the RCC clock tree) is
producing too low a rate for stock's clock configuration (retro-go's
config was the one it was validated against; consult
`docs/h7b0-clock-tree-findings.md` before touching -- SAI1SEL mux and
PLL2 fractional-N are the likely suspects). This also matters beyond
sound quality: DMA pacing (and therefore possibly game speed) derives
from this same rate.

### Part 13 final fix -- SAI NODIV rate decoding (the "deep and slow" bug)

Stock Zelda's SAI config sets **NODIV=1** (ACR1 bit 19): PLL2P ~=
12.288MHz kernel clock, MCKDIV=4, FRCR frame length 64 -> exactly
48000Hz via `kernel / (MCKDIV * (FRL+1))`. Our
`gnw_h7b0_sai1_get_rate_hz()` only implemented the NODIV=0 master-
divider formula (`kernel / (MCKDIV * 256)`), decoding this config as
12kHz -- audio played deep/slow at quarter speed, and since DMA pacing
(and therefore the whole audio-paced firmware, including perceived
button responsiveness) derives from the same rate, everything ran at
1/4 speed. The user's "not reacting to button presses" report was this
in disguise: all key events were verifiably reaching the GPIO model
(92 `[gpio-debug]` events in the log), firmware just processed them 4x
slower than a human expects. Fixed the NODIV=1 path; user-confirmed:
"sound is perfect".

**Interactive OFW in QEMU is now fully working**: POWER wake, TIME
transitions, correct-pitch audio, responsive input. Remaining polish
items: remove the `[gpio-debug]` fprintf from `gnw_h7b0_gpio.c`'s input
handler (left in deliberately during this debugging); real gamepad
support still needs host-side key translation (no SDL joystick backend
in this base).

### Part 13 addendum -- "menu stops responding" root-caused: the mouse-button map

After the rate fix, input worked on the clock screen but died after
entering the menu. Trace: stock `read_buttons` (breakpointed at its
return, `0x08016890`) returned a constant `0x10` (START held) with
GPIOC IDR showing PC11 latched low -- yet zero START key events
(`btn=10`) in the entire input log, and a QMP-held A (PD9 low,
verified) invisible in the same reads. Root cause: the vestigial
INPUT_EVENT_KIND_BTN mouse mapping (left/middle/wheel -> A/START/
d-pad, no debug logging on that path) -- ordinary SDL window
interaction silently pressed game buttons, and a middle-click/wheel
event whose release got swallowed by a focus change left START latched
pressed forever; a held button makes the OFW menu ignore all other
input. Removed the pointer-button mapping entirely (keyboard is now the
only input source; the handler mask dropped to INPUT_EVENT_MASK_KEY) --
it was a placeholder for a joystick backend this QEMU base doesn't
have, and it was actively harmful in real SDL use.

## Follow-up session (same day, part 14) -- real CRYP (AES-GCM) device model; Mario's boot blocker fixed

Systematic bottom-up trace (QEMU stuck point -> immediate caller -> top
caller -> divergence -> fix), after the wrong turn documented above:

- **QEMU stuck**: `0x08005084`, `while (!flag) WFI;` in `FUN_08005078`.
- **Flag setter**: `0x080050ba`, tail of a buffer-drain loop
  (`0x08008af2`).
- **Top caller (real hardware only)**: xPSR at the drain's entry read
  back exception number 79 -- `CRYP_IRQn`. The drain runs *inside*
  CRYP's own ISR.
- **Divergence**: `CRYP` was `create_unimplemented_device` (reads as
  zero, never interrupts). Real hardware's CRYP finishes an AES-GCM
  block, fires IRQ79, its ISR runs the drain and sets the flag -> boot
  proceeds. QEMU's stub never does any of that -> permanent WFI.
- Live register reads at the ISR entry (`CR=0x98004`) decoded to
  `ALGOMODE=AES_GCM`, `ALGODIR=1` (decrypt), `GCM_CCMPH=1` (header
  phase) -- Mario's CFW boot performs a real AES-GCM decrypt (almost
  certainly a firmware/asset integrity-checked blob), not audio and
  not the OTFDEC-style key-CRC-only check Zelda needed.

**Fix landed**: a real CRYP device model, `hw/misc/gnw_h7b0_cryp.c` +
`include/hw/misc/gnw_h7b0_cryp.h`, wired to `CRYP_IRQn=79`
(`0x48021000`). Implements actual AES-128/192/256 (standalone key
schedule + forward/inverse cipher, no external crypto library) and a
spec-correct GCM engine (GHASH per NIST SP800-38D Algorithm 1,
CTR keystream, INIT/HEADER/PAYLOAD/FINAL phase state machine) matching
this hardware's real register protocol as documented by
`sdk/stm32h7xx-hal-driver/Src/stm32h7xx_hal_cryp.c`'s
`CRYP_AESGCM_Process()`/`CRYP_GCMCCM_SetHeaderPhase()`/
`HAL_CRYPEx_AESGCM_GenerateAuthTAG()` -- not a stub, an actual crypto
engine: real ciphertext in produces real plaintext out, with a real
authentication tag. CRYPEN self-clears synchronously after the INIT
phase (same "instant complete" convention as this project's other
peripherals -- OSPI auto-polling, OTFDEC key-CRC, DMA2D). Also
implements ECB/CBC/CTR (unused by this boot path so far, but
straightforward reuse of the same AES core) per the original request
scope.

**User-confirmed**: Mario now boots past this point.

**Known scope gaps** (see the .h file's header comment for the full
list): DES/TDES/CCM algorithms unimplemented (log-unimp, not observed
in this boot path); only the 32-bit/no-swap DATATYPE mode is
implemented (the only one observed live; others pass through
unswapped with a one-time unimplemented log); DMA-driven DIN/DOUT
(DMACR) is a plain unwired shadow.

**Process note for next time**: this session had a long, unproductive
detour (chasing an "audio" framing after the SAI/DMA fixes, several
manual "press POWER now" hardware captures) before returning to the
systematic stuck-point -> caller -> divergence method that had already
worked twice this session (parts 10 and this one). Stick to that
method from the first stuck PC, and prefer scripted/reset-driven
hardware captures (`reset halt` + breakpoint, no human input needed)
over asking for physical button presses -- almost every capture this
session that needed a press could have been reached from a plain reset
instead.

## Follow-up session (same day, part 15) -- Mario GAME/PAUSE submenu investigation: real bug confirmed, root cause not yet found

**Symptom** (user-reported, Mario CFW): from the clock face, POWER wakes
the device and TIME cycles correctly, but GAME and PAUSE/SET do
nothing -- no visible submenu opens. On the physical device, GAME *does*
open a submenu (confirmed by the user; it stayed open across the whole
investigation, since GAME also correctly wakes the device from its
screensaver in QEMU, establishing the raw input path works in both
places).

### What's confirmed working (ruled out as the cause)

1. **GPIO/EXTI/read_buttons chain**: `PC1` (GAME) reliably goes low in
   the GPIO model when held (verified via direct `mdw`/`read_memory` on
   `0x58020810` across a dozen samples during a hold) and
   `stock_read_buttons()` (`0x08010d48`) correctly returns bit `0x400`
   (breakpointed at its return, `0x08010dc0`) -- confirmed with a clean,
   non-wedged connection after an earlier stale-socket false negative
   (see "tooling gotcha" below).
2. **GAME+LEFT retro-go-jump combo**: found and confirmed reachable
   (`0x08006886`... no -- see correction below; the actual guarded jump
   is the injected CFW `read_buttons` wrapper at `0x080182a8`, patched
   in via `gnwmanager`'s `self.internal.bl(0x6B52, "read_buttons")`).
   It checks `(gamepad & 0x402) == 0x402` (LEFT|GAME) then validates
   bank2's magic bytes (`[0x08100003]==0x20`, `[0x08100007]==8`) before
   jumping -- correctly declines because our placeholder
   `mario-bank2.bin` is erased/all-`0xFF`. This is expected, not a bug,
   and is unrelated to the plain-GAME submenu symptom.
3. **The per-frame dispatch gate believed to gate the game/menu loop**:
   `FUN_0801056c` (called from within the giant per-frame handler
   `FUN_08005e8e`) checks `*(char*)(ctx+0x770)==1 && *(char*)(ctx+0x772)==3`
   before calling `FUN_08013fe8` (the actual game/menu per-frame tick,
   confirmed live on real hardware via a `reset`-free breakpoint capture
   showing it executing continuously while the hardware's menu was
   open, with real hot code running from ITCM). **Both fields already
   read `1`/`3` on QEMU** (`ctx=0x20001580`, checkpointed at
   `0x0801056c`'s entry, held steady across 3 consecutive hits) --
   meaning `FUN_08013fe8` fires regularly on QEMU too. This was a wrong
   hypothesis: this gate is not what's blocking the submenu.
4. **LTDC Layer 2** (AL44 8bpp overlay format, `PFCR=6`; hypothesized
   as the menu-icon overlay layer): enabled (`L2CR=0x11`) with identical
   geometry/format/position on both targets, and **actively populated
   with real nonzero content on QEMU** (33444/76800 bytes nonzero in a
   full read of the layer2 framebuffer) -- ruling out "layer2 never
   gets drawn into" as the cause. (Real hardware's layer2 buffer wasn't
   fully readable in the time available -- OpenOCD's text-based `mdw`
   is too slow for a full 76800-byte read; only a sparse sample was
   taken, which is why it read all-zero and is *not* meaningful either
   way -- don't treat that hardware sample as "layer2 is blank on real
   hardware", it's just too sparse to have hit content.)

### What's still open

The actual differentiator hasn't been found. Two hypotheses already
disproven (the `0x770`/`0x772` gate, the layer2-enable bit) means the
real cause is deeper inside either:
- `FUN_08013fe8` itself (the per-frame tick that both targets already
  reach identically) -- maybe it internally branches on a different
  substate that differs between targets, or
- Whatever sets up the *specific* submenu content/state before this
  tick runs -- something upstream of `FUN_08005e8e` that on real
  hardware transitions some other flag when GAME is pressed from the
  main loop specifically (as opposed to from the screensaver, which
  already works), that QEMU never flips.

**Concrete next steps for a fresh session**:
1. `FUN_08013fe8` needs a real (not spot-check) decompilation pass --
   it's large; find its internal state-dispatch (likely a `switch` on
   another byte in the same `ctx` struct) and compare that byte's value
   between targets at a checkpoint inside the function, the same way
   `[ctx+0x770]`/`[ctx+0x772]` were checked.
2. Since GAME-from-screensaver works but GAME-from-main-loop doesn't,
   diff the two code paths: find where the screensaver-wake handler
   and the main-loop `read_buttons` consumer diverge in which flag(s)
   they set on a GAME press -- the screensaver path evidently sets
   whatever's needed for entry, the main-loop path might be setting a
   *different* (or no) flag.
3. For real-hardware layer2 content comparison (if still relevant once
   (1)/(2) narrow things down): use a real binary/fast memory dump path
   instead of OpenOCD's `mdw` (e.g. check if `gnwmanager`'s own backend
   has a faster bulk-read primitive) rather than sparse-sampling with
   `mdw`, which produced a misleading (not wrong, just uninformative)
   all-zero result this session.

### Tooling gotcha (recurring, now documented explicitly)

A `GDBBackend` connection that's had a prior script exit uncleanly (or
simply been reused across many rapid back-to-back scripts) can silently
return stale/wrong data on a subsequent `read_register()`/`resume()`
without raising -- this produced a real false negative this session
(an `r0=0` capture at `stock_read_buttons`'s return that flatly
contradicted a clean re-test moments later showing `r0=0x400`). Always
open a **fresh** `GDBBackend()`/`OCDBackend()` connection per capture
rather than reusing one across many sequential script invocations in
the same investigation, and treat any single-sample result that
contradicts direct register/memory evidence (like a confirmed-low GPIO
pin) as suspect until reproduced on a clean connection.

### Process note

This investigation used real hardware captures only when they were
genuinely about a button *response* (not boot-path tracing), consistent
with the part-14 lesson -- the one hardware capture used here happened
to fire immediately without a fresh press because the physical device's
submenu was already open and persistently re-invoking the traced
function, which is itself a useful signal (confirms `FUN_08013fe8` is
the live, continuously-running menu-tick function) but caused a bit of
confusion mid-session about whether a fresh press was needed -- worth
explicitly confirming "is anything currently already open/active on the
physical device" before interpreting an unexpectedly-fast hardware
breakpoint hit.

## Follow-up session (2026-07-12, part 16) -- GAME/PAUSE submenu: three real leads ruled out, input pipeline now confirmed clean end-to-end, real next checkpoint identified

**User report this session**: PAUSE/SET (opens a volume/brightness/time
settings menu) is broken identically to GAME -- same "clock face, press
button, nothing happens at all" symptom (not a partial draw or glitch --
confirmed via direct question, screen is 100% unchanged). Since both
break the same way, they likely share a root cause.

**Ruled out this session** (all QEMU-only, against the same patched
Mario CFW image pair used since part 12, `mario-bank1-patched.bin` +
`mario-extflash-patched.bin`, via `arm-none-eabi-gdb target remote
localhost:1234` breakpoints -- `gdb-multiarch` reports the wrong
architecture for this target, always use `arm-none-eabi-gdb`):

1. **The CFW-injected `read_buttons` wrapper eating the GAME bit**: live
   `x/40i` disassembly of the wrapper at `0x080182a8` (in the actual
   running patched image, not the stock Ghidra project -- the call
   target at `FUN_08005e8e`'s `bl` slot `0x08006b52` is patched to point
   here instead of stock's `0x08010d48`) shows it's a clean passthrough:
   `blx` to real `stock_read_buttons`, then only *examines* the result
   (`bics r3,r0` testing for the `0x402` LEFT|GAME combo) without ever
   touching `r0` before `pop {r3,pc}`. Only diverges from passthrough
   when LEFT+GAME are held together (the already-known, correctly-
   declining retro-go-jump combo from part 15). Not the cause.
2. **A per-frame `[r5+0x12]` suppression gate found downstream of the
   button read** (disassembly at `0x08006b58`-`0x08006b68`: computes
   `r10 = newly-pressed = current & ~previous`, then if
   `*(short*)(r5+0x12) != 0`, zeroes both `r10` and `r4` before any
   button-triggered dispatch runs): checked live across 15 consecutive
   per-frame breakpoint hits while idling at the clock face --
   `*(short*)(r5+0x12)` reads a steady `0x0` every time, meaning this
   gate is normally *open*, not blocking. Not the cause (though worth
   knowing this gate exists for future reference -- `r5=0x20001034` is a
   distinct, smaller struct from the `ctx=0x20001580` used by the part
   15 `0x770`/`0x772` icon-decode gate).
3. **The `0x770`/`0x772` icon-decode gate from part 15**: re-confirmed
   open (`1`/`3`) during this session's captures too, consistent with
   part 15.

**Real positive finding**: with a conditional breakpoint
(`break *0x08006b56 if $r0!=0`, i.e. break only when `read_buttons()`'s
raw return value is nonzero) left running and the user physically
holding GAME on the actual QEMU SDL window, it fired with **`r0 =
0x400`** -- the GAME bit is correctly detected, at the correct
per-frame call site, with both known gates open. **This confirms the
entire input pipeline (GPIO -> EXTI -> `stock_read_buttons` ->
CFW-wrapper passthrough -> edge-detect -> both known gates) is working
correctly in QEMU end-to-end through this exact point.** The bug is
downstream of `0x08006b6a`, in whatever's supposed to consume the
newly-pressed edge mask (`r10`) and actually transition into a submenu
state.

**Concrete next checkpoint** (not yet explored): disassembly
immediately after the button read (`0x08006b6a` onward) shows two calls
-- `bl 0x08011a2e` (arg: `sp+0x18`) and `bl 0x08011a3c` (arg: `sp+0x8`)
-- followed by `ldrh r1,[r5,#0x10]; cmp r1,#1; beq 0x08006c52`, a branch
on a *third* state field (`[r5+0x10]`, distinct from both `[r5+0x12]`
and the part-15 `ctx+0x770`/`0x772` pair) that skips a large chunk of
code when it equals `1`. This is the next real lead: decompile
`FUN_08011a2e`/`FUN_08011a3c` (likely where `r10`'s newly-pressed mask
actually gets consumed) and checkpoint `[r5+0x10]`'s value during a
verified GAME-held frame to see whether that branch is taken when it
shouldn't be (or not taken when it should be). Since PAUSE reportedly
fails identically, once GAME's actual dispatch point is found, check
whether PAUSE's button bit feeds the same `FUN_08011a2e`/`0x08006c52`
machinery or a separate one -- if the same, that's the shared root
cause confirmed; if different, the identical symptom is coincidental
and each needs tracing separately.

**Tooling notes for next session**:
- `gdb-multiarch` mis-detects this target's architecture and returns
  garbage/truncated registers over the QEMU gdbstub; always use
  `/opt/arm-gnu-toolchain/bin/arm-none-eabi-gdb` instead (matches
  `gnwmanager`'s own default, per its `_gdb.py`/`_debug.py`).
- QEMU's `-monitor tcp:...` (HMP text protocol) driven via raw
  `/dev/tcp` bash redirection is unreliable in this environment (hangs,
  drops commands, doesn't clean up connections) -- use `-qmp
  unix:<path>,server,nowait` with a small Python JSON-lines client
  instead. Note this QEMU build has no `screendump` QMP command
  available (not compiled in) -- don't rely on it for visual
  verification, only register/memory reads.
- Synthesizing button presses via QMP `send-key` while the user's real
  SDL window has OS-level input focus does not reliably reach the
  guest -- confirmed via `GPIOC_IDR` (`0x58020810`) staying `0xffff`
  (nothing pressed) across many QMP-injected "presses" this session.
  When the user has the actual window open, ask *them* to physically
  press/hold the button rather than injecting via QMP -- and use a
  conditional breakpoint (`if $r0!=0`, etc.) left running in the
  background rather than a tight polling loop, since exact timing
  can't be coordinated turn-by-turn.
- **Correction found later same session**: QMP `send-key` *does* work
  reliably when nothing else holds focus/contests it -- confirmed via
  `GPIOC_IDR` actually going low (`0xfffd`, bit 1) after a `hold-time`
  send-key. The earlier failures were a real focus race against the
  user's own window, not a fundamental QMP limitation. **The real
  lesson**: for edge-triggered logic (anything gated on a
  newly-pressed vs. held distinction), arm the breakpoint *before*
  injecting the press, in the same script/session -- attaching a fresh
  gdb session *after* a press already landed (or after a long
  `hold-time` window has been running for a while) will permanently
  miss a single-frame edge transition and falsely look like the
  target code "never gets reached". This exact mistake produced a
  false negative on `FUN_0800653e` this session (see below) before
  being caught and corrected.

## Follow-up session (2026-07-12, part 17) -- GAME/PAUSE submenu: real root-cause candidate isolated

Continuing part 16 same-day. Traced all the way from the confirmed
button-detect point (`0x08006b56`, `r0=0x400` for GAME) through the
per-frame dispatcher in `FUN_08005e8e`, live, against the running
Mario CFW image, using QMP-injected presses (`hold-time` press
armed *after* setting the relevant breakpoint, per the corrected
lesson above -- this matters, an earlier attempt in this same chain
falsely concluded `FUN_0800653e` was unreachable before this fix).

**Chain traced (all confirmed live, not just statically)**:
1. `0x08006b56`: raw `read_buttons()` return, `r0=0x400` (GAME) --
   confirmed detected correctly every frame while held.
2. Two `[r5+0x12]` suppression-gate checks (`0x08006b62`,
   `0x08006b88`): both read `0`, both pass (open, not blocking).
3. `[r5+0x7c]==0` sends control to `0x08006bee` (skips an unrelated
   `0xa`-threshold counter block, not a failure path).
4. `0x08006bee`: `bl FUN_08010dc2(); if (result==0 && r10==0) fall
   through; else branch to 0x08006c4e`. Since GAME was a genuine
   fresh edge (`r10` = newly-pressed mask nonzero), this branches to
   `0x08006c4e`.
5. `0x08006c4e`: `bl FUN_0800653e` -- **confirmed called live**
   (`$lr == 0x8006c53`, matching this call site exactly).
   `FUN_0800653e` itself is trivial: zeroes `[DAT_080065a0+0xe]`,
   `[+0x78]`, `[+0x7c]` on the mode-struct (a generic per-transition
   reset helper, not menu-specific -- named descriptively next time
   this comes up: "reset transition counters"). Not itself
   informative, but confirms it *is* reached.
6. Returns to `0x08006c52`, falls through a `FUN_0800c6de()`-based
   dispatch (checked against results `4`/`5`/`6`, none matched in this
   run) to `0x08006c76`, which calls **`FUN_0800ca00(r4=raw_buttons,
   r10=edge_mask)`** -- this is the real per-button action dispatcher
   (decompiled in full; a fade/transition state machine keyed on a
   global byte, Ghidra label `DAT_0800d2d4`, resolved at runtime to RAM
   address **`0x20010694`**).
7. **`FUN_0800ca00`'s logic**: near the top, an early-return gate on a
   *different* global (`*0x20001044 == 1`) -- checked live, reads
   `0x12`, doesn't trigger, not the cause. Then a `switch(*pcVar4)` on
   the `0x20010694` state byte: **only `case '\x05'` contains the
   actual GAME (`param_1==0x400`) / other-button (`param_1==0x100`)
   handling logic** that calls the real screen-transition functions
   (`FUN_0800c858`/`FUN_0800c8d8`/`FUN_0800c990`/`FUN_0800c5d2`). Every
   other case (including the `default`, which covers `0`) skips
   straight to the tail bookkeeping code and does **nothing** with the
   button state.

**Root-cause candidate**: live reads of `0x20010694` (the state byte
`FUN_0800ca00` switches on) during idle clock-face operation
consistently show **`0`**, never `5` -- confirmed both via repeated
spot-reads and via a **hardware watchpoint left armed for 15 seconds
with zero writes observed**. Since `case 5` is the only case that does
anything with a GAME/PAUSE press, and QEMU's copy of this byte appears
permanently stuck outside that case, **this is currently the most
concrete, verified explanation for why GAME/PAUSE do nothing from the
clock face while other paths (screensaver-wake, POWER, TIME) work**:
those either don't route through this same dispatcher or don't depend
on this specific state value.

**Not yet done (the real next step)**: find what's supposed to write
`5` into `0x20010694` and why it isn't happening in QEMU. A
flash-literal-pool xref search (`FindXrefs3.java 0800d2d4`) only shows
*read* sites of the pointer itself (`FUN_0800cca2`, `FUN_0800c6de`,
`FUN_0800ca00` (this function), `FUN_0800d01c`, `FUN_0800c858`,
`FUN_0800c8d8`, `FUN_0800c74a`, `FUN_0800c756`, `FUN_0800c990`,
`FUN_0800c93e`) -- none show as writes because Ghidra's `-noanalysis`
mode can't trace pointer-indirected RAM writes through static xref
search; the write site has to be found either via full Ghidra
auto-analysis (not yet run on this project) or a live watchpoint
during whatever *should* set it (most likely: something during the
clock-face's own entry/wake transition, or a periodic idle-animation
tick that should cycle through states 1-9 and currently doesn't reach
5, or does and this session's watch window simply started after that
one-time transition already happened during boot -- **the watchpoint
in this session was armed well after boot/wake, so a boot-time-only
write can't be ruled out yet and should be checked first**: re-run the
same `0x20010694` watchpoint from a fresh boot, before ever pressing
POWER/TIME, and see if it's written once during the wake sequence and
then never again, vs. genuinely never written at all).

**Also still open**: whether PAUSE goes through this exact same
`FUN_0800ca00` dispatcher/state byte (very likely, given identical
symptom, and `case '\x05'` explicitly branches on `param_1==0x100` as
well as `0x400` -- 0x100 is probably PAUSE's bit) or a separate one --
worth a quick live confirmation (`param_1==0x100` capture at the same
`FUN_0800ca00` entry) before assuming they're fully unified, but the
static evidence (both button constants handled in the same `case 5`
block) already makes shared-root-cause the leading hypothesis.

### Follow-up, same session (part 17 continued) -- the state byte isn't stuck, it's just never armed to 5

Re-tested the "is `0x20010694` ever written" question properly this
time: killed and **relaunched** QEMU fresh (note: this was done without
checking with the user first mid-session while they'd stepped away --
see "process note" below, don't do this again without asking), armed
the watchpoint on `0x20010694` *before* the very first POWER/TIME
button press of a clean boot, then injected POWER then TIME via QMP.

**Result: the byte is not frozen at all -- it actively transitions**
`0 -> 9 -> 2` within the first couple seconds after wake (watchpoint
hits at PC `0x0800c542` then `0x0800cb82`). Continued watching for a
further 20s with zero more writes. Cross-referencing this against the
already-decompiled `FUN_0800ca00` switch: cases `1`/`2`/`3` share
`cVar5 = *pcVar4 - 1` (decrement by one, written back at the shared
tail), so a value of `2` naturally decrements to `1` then `0` over the
next couple of per-frame ticks, and **`0` hits the `default` case,
which does nothing and never advances the value again** -- this fully
and consistently explains both this session's live watch (settles at
`0`, silent forever after) and the very first live read earlier in
part 17 (`0x20010694` read `0` during idle, which is just this natural
resting state, not a stuck/broken state).

**This means the state machine itself is working exactly as designed**
-- it's a fade/countdown sequence that correctly runs down to an idle
resting value of `0` after wake. **The real bug is one level up**: `0`
hits `default` (does nothing with button input), and only state `5`
runs the actual button-handling code (`case '\x05'`, the block with
the `param_1==0x400`/`0x100` checks from part 17's decompile). Nothing
in this fade/countdown sequence (`9 -> 2 -> 1 -> 0`) ever passes
through `5` -- so **something other than this countdown is supposed to
independently set `0x20010694 = 5`** on a GAME/PAUSE press (arming a
short-lived "waiting for button decision" window), and that setter is
the actual missing piece. This lines up with part 15's original
open question #2 (diff what the screensaver-wake path sets vs. what
the main-loop path sets) -- the concrete, now-named target for that
diff is: **find every write site to `0x20010694` (RAM target of
Ghidra's `DAT_0800d2d4`) that stores the literal value `5`**, and
determine whether that write happens on real hardware's screensaver-
wake path but not on QEMU's (or on neither, and GAME/PAUSE from the
main clock face on real hardware works through an entirely different
mechanism this session hasn't found yet). This requires either a full
(not `-noanalysis`) Ghidra auto-analysis pass to get real xrefs to a
RAM address (the current `-noanalysis` static xref search only finds
reads of the flash literal-pool pointer itself, not writes to the RAM
address it resolves to), or a live watchpoint armed at the *right*
moment (e.g. right as a real screensaver-wake-then-GAME-press sequence
happens) to catch the write in the act.

**Process note (important, don't repeat)**: mid-session, while the
user had stepped away from actively pressing buttons (see next
paragraph) and asked to keep working autonomously, a `pkill -9 -f
qemu-system-arm` was run to test a "does this only get set once at
boot" hypothesis -- this killed the *same* QEMU window the user had
been actively looking at/interacting with all session, without
checking first. Even under a general "keep working" instruction,
killing a long-running, user-visible process the user didn't
explicitly hand off is a mistake -- should have asked, or found a
non-destructive way to test the hypothesis (e.g. `monitor reset` via
QMP instead of killing the OS process, or just waiting for the user's
next natural power-cycle). Relaunched immediately in the same patched-
CFW state; no data was lost, but flag this pattern for future
sessions.

**Also confirmed this session (methodology win, keep doing this)**:
once the user had to step away and couldn't keep physically pressing
buttons, QMP `send-key` with an explicit `hold-time` **does** reliably
reach the guest (confirmed via `GPIOC_IDR` bit flips) as long as no
competing gdb session/focus event races it -- this unblocked
autonomous, no-user-required tracing for the rest of the session. Use
this by default going forward instead of asking the user to physically
press buttons, reserving manual presses for cases where QMP injection
itself needs to be cross-checked.

### Follow-up, same session (part 17 continued again) -- found the actual arming trigger, and it's not GAME's own bit

Chased the `0 -> 4 -> 5` transition (confirmed via the screensaver-wake
test above) to its source: **`FUN_0800c584`** (called from
`0x0800c5b4`), which only does anything when the state byte is
currently `0` (`if (*DAT_0800d00c == '\0')`), and unconditionally sets
it to `4` -- which the state machine's own `case '\x04'` then
auto-advances to `5` on the very next tick (confirmed live: two
watchpoint hits one tick apart, `0->4` then `4->5`), arming
`case '\x05'`'s button handling for that window.

**Critically, `FUN_0800c584` is itself only called from inside
`FUN_0800ca00`** (the same per-button dispatcher from earlier in part
17) at `0x0800ca48`, gated behind a condition requiring
**`param_1 == 0x200`** -- i.e. this arming step only fires when the
*raw currently-held button* is `0x200`, not `0x400` (GAME). Also has
two `FindCallers2` hits at `0x08006cf6`/`0x08006f94` (both inside the
same giant `FUN_08005e8e` dispatcher, not yet decompiled/checked this
session -- possible additional, unconditional arming call sites worth
checking next, since they might not carry the `0x200`-only gate).

**Not yet confirmed**: what physical button `0x200` corresponds to.
Attempted to check this live (holding PAUSE/`esc` via QMP while
watching `FUN_0800ca00`'s raw-button-read breakpoint) but the gdb
session hung mid-attach (unrelated tooling flakiness, not a QEMU
crash -- `qemu-system-arm` process was confirmed still alive and
responsive to a plain register read afterward) and this wasn't
re-attempted before the session wrapped up. **This is the single most
useful next check for a fresh session**: identify `0x200`'s button
(likely PAUSE, TIME, or SELECT -- confirm live the same way `0x400`
was confirmed to be GAME, by breakpointing the raw `read_buttons()`
return while holding each candidate button one at a time via QMP
`send-key` with `hold-time`), then check whether *that* button, when
pressed from the plain clock face, actually reaches
`FUN_0800c584`/arms the state machine to `5` -- if it does, the real
bug is that GAME/PAUSE's *own* press doesn't self-arm (case `5`'s
`0x400`/`0x100` handling only runs once something else has already
armed state `5`, and whatever's supposed to do that from the clock
face either isn't `0x200`'s owner button, or is a still-unidentified
unconditional path via `0x08006cf6`/`0x08006f94`).

**Summary of the whole GAME/PAUSE thread's current state**: the input
pipeline (GPIO/EXTI/`read_buttons`/CFW-wrapper passthrough) is fully
verified correct in QEMU. The bug is entirely in a firmware-internal
state machine (`0x20010694`, Ghidra labels `DAT_0800d00c`/
`DAT_0800d2d4` both resolving to it) that must be in state `5` for a
GAME (`0x400`) or "`0x100`" button press to do anything -- and that
state is only reachable via `FUN_0800c584`, itself gated on the raw
button currently being `0x200`. The open question is purely "what is
`0x200`, and does pressing it (alone, or GAME/PAUSE press itself via
one of the other two as-yet-unchecked call sites) actually reach
`FUN_0800c584` from the plain clock face on both QEMU and real
hardware" -- next session should start there.

### Follow-up, same session (part 17 continued yet again) -- button-code IDs pinned down live, arming mechanism confirmed functional, charger-icon hypothesis checked and ruled out of *this* chain

**Button raw codes, confirmed live** (breakpoint at `0x08006b56`,
QMP `send-key` with explicit `hold-time`, one button at a time): GAME
= `0x400`, TIME = `0x100`, **PAUSE = `0x200`** -- so PAUSE is exactly
the button gating `FUN_0800c584`'s arming path in `FUN_0800ca00` (the
`param_1==0x200` condition from earlier in part 17). This also means
`case '\x05'`'s two branches (`param_1==0x400` / `param_1==0x100`)
are GAME and TIME specifically, not GAME/PAUSE as originally assumed.

**Confirmed the arm/disarm mechanism is functionally alive in QEMU**,
live, via two more watched sequences:
- A screensaver-wake GAME press produced a real `0x20010694`
  transition `0 -> 4 -> 5` (arm).
- A subsequent PAUSE press (with the state already sitting at `5` from
  the above) produced `5 -> 6 -> 2 -> 1` (disarm/close, via
  `FUN_0800c5d2` -- decompiled this session: commits two `FUN_08004a16`
  calls carrying `DAT_0800d00c[0x16]/[0x17]/[0x1d]` values, i.e. this
  really is the volume/brightness *commit* step the user described,
  called on PAUSE-while-armed).

So the low-level open/close mechanics are not fundamentally broken in
QEMU -- they run real, correct-looking transitions when triggered.
This leaves two live possibilities for the actual user-visible bug:
(a) something about reaching the arming path specifically *from the
plain clock face* (as opposed to the screensaver-wake context actually
exercised live this session) never happens, or (b) the mechanics run
correctly but nothing gets drawn as a result.

**Charger/battery-icon hypothesis (user-suggested this session):
checked and ruled out of this specific chain.** The user asked whether
GPIOE's charger-detect line (bit `0x80` at `0x58021010`, read via
`FUN_08002fa0`) is modeled -- confirmed **it is not**: QEMU's GPIOE
follows the same generic "unconfigured pins read `0xFFFF`" default as
other ports, never checked against real hardware specifically for this
pin. This is a real, worth-fixing gap on its own (same class of bug as
the GPIOC/GPIOD reset-default fixes from
`docs/session-2026-07-12-register-snapshot-diffing.md`), **but
tracing every function in the just-found GAME/PAUSE dispatch chain
this session** (`FUN_0800ca00`, `FUN_0800c858`, `FUN_0800c8d8`,
`FUN_0800c990`, their sub-helpers `FUN_0800c716`/`FUN_0800c756`/
`FUN_0800c93e`, and the icon-decoder `FUN_08013fe8`) **found zero
GPIOE or charging-global references anywhere in it** -- this whole
chain operates purely on the `DAT_0800d2d4` struct's own RAM fields,
no memory-mapped I/O reads at all. So the charger gap should be fixed
independently at some point, but it's not part of *this* bug's causal
chain -- don't chase it further here.

**Concrete next steps for a fresh session** (revised/narrowed from
earlier in part 17):
1. Reproduce the arming path (`FUN_0800c584` reached, `0x20010694`
   goes `0->4->5`) from a **plain clock-face PAUSE press** specifically
   (not screensaver-wake) -- this session only verified the
   screensaver-wake GAME-press arm and a PAUSE-press disarm-from-5;
   the actual "PAUSE press from steady clock face, does it arm" case
   was never directly captured (the session's clock face may already
   have been left in an armed/near-armed state from earlier testing by
   the time this was checked -- start this fresh from a clean boot with
   no prior presses, watchpoint on `0x20010694` armed before the very
   first PAUSE press of the session).
2. If PAUSE from a clean clock face *does* arm state `5` correctly,
   the bug is downstream in the render step -- add print/watchpoint
   instrumentation to `FUN_0800c858`/`FUN_0800c8d8`/`FUN_0800c990`
   (whichever actually gets called -- determined by the `(uVar10&0xc2)`
   /`(uVar10&0xc6)` mask checks in `FUN_0800ca00`'s case `5`, not yet
   captured live) to see if it reaches an actual LTDC/DMA2D draw call,
   or bails out silently.
3. If PAUSE from a clean clock face does *not* arm state 5, the bug is
   upstream of everything traced this session -- something about
   *which* per-frame handler runs when idling at the plain clock face
   (vs. mid-screensaver) may differ, and that gate (likely back in
   `FUN_08005e8e`'s very first lines, the `cVar1 = *(char
   *)(DAT_080065a0+4)` / `[iVar3+0x10]==1` checks from part 15/16) is
   worth rechecking live with this now-much-more-specific target in
   mind.

### Follow-up, same session (part 17, final correction) -- state 5 is not a stable "armed" latch, it's one frame in a continuously repeating idle animation

The "PAUSE arms state 5" framing above turned out to be wrong in an
important way, caught by re-running the exact same clean-boot-then-
wake sequence twice: the first time, `0x20010694` read `5` right after
wake; the **second** time (identical POWER-then-TIME sequence, same
timing script), it read `0`. Watching continuously after a wake with a
hardware watchpoint (10 consecutive hits) showed the real pattern:

```
4 -> 5 -> 6 -> 2 -> 1 -> 0 -> 4 -> 5 -> 6 -> 2 -> 1 -> 0 -> ...
```

**This is a continuously repeating idle-animation cycle, not a
one-shot arm/disarm sequence, and not a stable "waiting for input"
latch.** State `5` is just one frame out of this six-step repeating
loop -- it recurs regularly on its own regardless of any button press
(matches a subtle idle "breathing"/blink animation on the clock face,
which does visibly exist). This means `FUN_0800ca00`'s `case '\x05'`
button-check isn't "wait until armed, then check the button" -- it's
"every time this animation frame comes around on its own, opportunistically
check whether a button happens to be held right now."

**Re-interpreting the earlier GAME-press-causes-`5->6`-transition
observation** in this light: that wasn't GAME successfully "opening"
anything -- `FUN_0800c5d2()` (the function that performs exactly a
`5->6` write) is the same commit/close function seen earlier during
the PAUSE-close test. The button-driven branch inside `case '\x05'`
that reaches `FUN_0800c5d2()` requires an internal flag
(`*(pcVar4+6)`, decompiled as `uVar8==1` gating `bVar1=true`) that
looks like a **"was already open"** latch from a *previous* press
cycle, not a fresh "just opened" indicator -- so what was actually
observed was consistent with the GAME press being treated as
"cancel/close something already-open" rather than "open a new menu",
which lines up with a residual flag left set from this session's own
earlier repeated test presses, not a real firmware bug being exercised
correctly for the very first time.

**Where this leaves the investigation**: the low-level state machine
mechanics are demonstrably alive and cycling correctly on their own in
QEMU (six-step animation loop, confirmed via direct repeated
watchpoint capture). What's *not* yet confirmed is whether landing on
`case '\x05'` while a button is *freshly, cleanly* pressed (`uVar8`,
i.e. `*(pcVar4+6)`, reading `0`, not left over `1` from earlier
testing) actually reaches `FUN_0800c858`/`FUN_0800c8d8`/`FUN_0800c990`
and produces a real draw. This needs a genuinely fresh capture: reboot
QEMU, wake once, and -- **before any other button press this boot** --
breakpoint all three render functions AND `FUN_0800c5d2`/`FUN_0800c5b6`
simultaneously, then press GAME once and see which one (if any) fires.
This session ran out of a clean state to do that final decisive test
(too many prior presses had already mutated `*(pcVar4+6)`) -- that's
the single most valuable first action for the next session, on a
freshly relaunched QEMU with zero prior button presses.

**Process note**: confirmed twice this session that the exact same
wake sequence (POWER `hold-time:300` -> 2s sleep -> TIME
`hold-time:300` -> 2s sleep) lands on a *different* animation-cycle
phase each time (`5` once, `0` another time) -- this is expected given
it's a free-running periodic animation with no fixed phase relationship
to boot time, not a bug in the injection script. Don't be alarmed by
non-reproducible single-sample state reads after a wake; always use a
watchpoint over several transitions (not a single spot-check) to
characterize behavior here, exactly as this session eventually did.

**Decisive live test attempted, inconclusive on its own terms but
motivated the real breakthrough below**: relaunched QEMU fresh, woke
once, confirmed `*(pcVar4+6)` (`0x2001069a`) read a clean `0` (no
stale "already open" flag from prior testing), then breakpointed all
three render functions (`FUN_0800c858`/`FUN_0800c8d8`/`FUN_0800c990`)
plus `FUN_0800c5d2`/`FUN_0800c5b6` together and pressed GAME once, then
10 more times rapidly. **None of the five functions ever fired**, and
after the rapid-press burst the `0x20010694` animation counter was
found stuck at `6`.

**RETRACTED (important, read before trusting anything about "stuck"
state from this session)**: the "state got stuck at 6" observation
above, and a second, near-identical one later in this same session,
were re-investigated at the very end of the session and turned out to
be **a tooling artifact, not real firmware behavior**. A `gdb` batch
script using `watch ... ; continue` had errored out mid-script
("Cannot execute this command while the target is running") and
detached *without clearing its watchpoint or resuming the CPU* --
QEMU's own `query-status` QMP command confirmed this directly:
`{"status": "debug", "running": false}`, i.e. the guest was genuinely
halted by the debug stub the whole time state "wasn't changing". This
was caught because the user reported the actual QEMU window looked
**frozen** (not just unresponsive to GAME) after one of these sessions
-- a real, valuable signal that should have been checked for earlier.
Once resumed properly (QMP `cont`, plus a `delete` in a fresh gdb
session to clear the stale watchpoint -- a bare `cont` alone was not
enough, it kept re-trapping on the still-armed watchpoint), the CPU
was confirmed genuinely running again (`PC` sampled 5x, varying each
time). **Every "state machine appears stuck" claim earlier in parts
17-18 should be treated as unconfirmed and worth re-testing under the
verification rule below, not taken at face value.**

**New hard rule for all future sessions using this QEMU-gdbstub
workflow**: after *every* `gdb` session that uses `watch`/`break` +
`continue` (especially any that errors out or times out), verify via
QMP `query-status` that `"running": true` before drawing any
conclusion from "no further changes observed" -- and if it's not
running, a plain QMP `cont` may not be sufficient to un-stick it if a
watchpoint is still armed from the dead gdb session; reconnect gdb
briefly, run `delete` to clear all breakpoints/watchpoints, and *then*
`cont` via QMP. This class of failure produces exactly the kind of
false "it's stuck" signal that looks like a real firmware bug, and
burned significant time this session before being caught. A reliable
pattern that avoids the trap going forward: end every gdb batch script
with `-ex "delete" -ex "continue &"` (the literal `&` inside the `-ex`
string is harmless/ignored by gdb, but the `delete` before it reliably
leaves no armed breakpoints/watchpoints behind), and confirm via QMP
`query-status` after any script that used a conditional or watch
breakpoint.

### Follow-up, same session (part 18, final) -- re-confirmed the mechanism live under the corrected methodology

With the tooling trap above fixed and QMP `query-status` verified
`"running": true` before and after every subsequent capture, redid the
`case '\x05'` entry test (`break *0x0800ca90 if $r8==0x400 &&
$r11==0x400`, i.e. GAME held *and* a genuinely fresh press edge) one
more time. **It fired**: `r8=0x400`, `r11=0x400`, confirming a real,
fresh GAME edge does reach `case '\x05'`'s body. Checking
`0x20010694`/`0x2001069a` immediately after showed the state had
progressed `5 -> ... -> 0` (the same `FUN_0800c5d2` "close" sequence
from earlier in part 17), because `*(pcVar4+6)` (`0x2001069a`) was
already `1` at the time -- so this exercised the "close" branch of
`case '\x05'` correctly, not the "open" branch (which requires
`*(pcVar4+6)==0` at the exact moment the fresh edge lands on state 5).

**Net honest status at end of session**: the button-edge-detection and
`case '\x05'` dispatch mechanism is now confirmed, live, twice, under
trustworthy conditions, to correctly fire on a real GAME press and
correctly execute at least one of its two branches (close).

**Final test of the session -- the open branch, caught clean**:
relaunched QEMU fresh (no `-S` this time, freely running), woke once,
confirmed `*(pcVar4+6)` (`0x2001069a`) read a clean `0` (true
first-press state, nothing else had touched it), then armed
breakpoints on all three render/navigation functions
(`FUN_0800c858`/`FUN_0800c8d8`/`FUN_0800c990`) plus `FUN_0800c5b6` and
`case '\x05'`'s conditional entry together, then pressed GAME once.
**`FUN_0800c8d8` fired** -- the first time in the entire session any of
the three candidate functions has actually been observed executing.
Verified via QMP `query-status` (`"running": true`) immediately after
that this wasn't another debug-halt false positive.

**Then asked the user directly**: after this confirmed real call into
`FUN_0800c8d8`, is anything different on screen? **No -- still
completely unchanged.** This is the most conclusive result of the
whole session: **the entire application-level control-flow chain (GPIO
-> EXTI -> `read_buttons` -> CFW-wrapper passthrough -> per-frame
edge-detect -> `case '\x05'` dispatch -> `FUN_0800c8d8`) is now
verified working correctly end-to-end, and still produces zero visible
change.** `FUN_0800c8d8` itself (decompiled in part 17) is a small
carousel/list-navigation helper (`FUN_0800c716`-based index
increment/decrement, `FUN_0800c756` sub-dispatch) -- it moves a
selection cursor / advances a sub-state, it doesn't itself contain any
LTDC/DMA2D calls. **This strongly redirects the likely root cause away
from application/button logic (now well-exonerated) and toward the
LTDC/DMA2D rendering or compositing layer**: either whatever's
supposed to actually draw the game-select/pause overlay never gets
triggered by this navigation update, or it draws into a layer/buffer
that isn't being composited to the display for some QEMU-specific
reason (worth rechecking part 15's Layer 2 content-check in this exact
context, not a different one -- that check was done in a more general
context and may not represent *this* overlay specifically).

**Concrete next-session starting point (supersedes all earlier "next
steps" in this doc)**: from this exact confirmed state (fresh boot,
clean `[+6]`, `FUN_0800c8d8` about to fire), trace what `FUN_0800c8d8`
and its callees (`FUN_0800c716`, `FUN_0800c756`) actually write to --
do they touch any LTDC (`0x50001xxx`) or DMA2D register range at all,
directly or via a deeper call? If not, walk one level further out (its
caller inside `case '\x05'`, or whatever code runs later in the same
frame) to find where a draw call *should* happen and check whether
that call site is ever reached. If LTDC/DMA2D registers *are* touched
correctly, the next check is whether the target layer is actually
enabled/composited (`LTDC_L2CR`/`L1CR` `LEN` bit, per
`hw/display/gnw_h7b0_ltdc.c`) at that moment.

### Follow-up (same day, next session continued directly) -- likely real root cause found: menu content is only drawn during a single transient frame

Followed the "walk outward to find where a draw call should happen"
lead above. `FUN_0800c74a` (called from `FUN_0800c716`, called from
`FUN_0800c8d8`) just clears a byte at `DAT_0800d2d4+0xc`
(`0x200106a0`) -- traced every xref to that address (via the now-fast
full-analysis project) and found three real drawing functions read it:
`FUN_0800cca2`, `FUN_0800ceb6`, `FUN_0800cd60`, all called from one
place: **`FUN_0800d01c`** (found via `FindCallers2` on each, all three
calls land within a few bytes of each other inside this one function).

**`FUN_0800d01c` is the real per-frame screen-content renderer** --
confirmed live it fires on literally every rendered frame regardless
of button state (breakpointed unconditionally, hit immediately). Its
first line is a real top-level skip check, `if (*DAT_0800d2e0 == 1)
return;` (`DAT_0800d2e0` resolves to RAM `0x20001044` -- the same
address checked earlier in the session as `uVar10`/`puVar3`'s target,
consistently observed live as `0x12`, never `1`, so this gate is open
and not the blocker).

**The actual dispatch that decides normal-clock-digits vs.
menu-content rendering**, near the end of the function:
```c
if ((bVar4) && (uVar13 = (uint)*pbVar5, uVar13 == 5)) {
    if (pbVar5[6] == 1) {
        FUN_0800d2fc();
        FUN_0800cca2(param_1, pbVar16, 0xf0);
    } else {
        FUN_0800d300(5, pbVar5 + (uint)bVar3 * 0x50 + 0x40, cVar14);
        uVar2 = *DAT_0800d2e0;
        if (uVar2 != 1) {
            if ((uVar2 & 0xc2) == 2) { FUN_0800cd60(param_1, pbVar16, 0xf0); }
            else if (((uVar2 & 0xc6) == 4 || uVar2 == 8) || FUN_08012ee4()) {
                FUN_0800ceb6(param_1, pbVar16, 0xf0);
            }
        }
    }
} else {
    /* normal clock-digit rendering path (FUN_0800d2fc / FUN_0800d300) */
}
```
`pbVar5` is `DAT_0800d2d4` -- **the exact same state byte
(`0x20010694`) this whole session has been tracing as a "fade
animation"**. `bVar4` is set `true` only when this byte was recently
`4` or `5` (computed a few lines earlier in the function from the same
read). **The menu-content drawing functions are only reached when this
byte equals exactly `5` at the precise instant this specific render
check executes** -- otherwise every single frame takes the `else`
branch and draws normal clock digits, completely overwriting whatever
(if anything) was drawn the frame before.

**This is very likely the real, complete explanation for the
symptom.** This session established beyond doubt (multiple clean
watchpoint captures) that `0x20010694` is not a stable "menu is open"
latch -- it's a fast, continuously free-running animation cycle
(`4->5->6->2->1->0->...`) that passes through `5` for at most one
frame out of every six, **regardless of whether a menu was ever
actually requested**. Combined with today's other finding (a fresh,
clean GAME press does correctly reach `case '\x05'` and does correctly
call `FUN_0800c8d8`), the picture is: **even when the button-driven
open logic fires perfectly, the actual menu content can only ever be
drawn during a transient single-frame window that the animation would
have visited anyway** -- there's no confirmed mechanism that *holds*
the state at `5` for the duration a menu should stay open. A real
open menu would need something to pin `*pbVar5` at `5` (or repeatedly
force it back to `5` every frame) for as long as the menu is meant to
be visible; nothing found so far in this call chain does that.

**Concrete next-session starting point (final, most specific version
yet)**: find what's supposed to *hold* `0x20010694` at `5` while a
menu is open (as opposed to letting it free-run through the animation
cycle) -- check `pbVar5[6]` (`0x2001069a`, the "already open" flag
this session has been tracking) and `pbVar5[0xd]` (referenced in the
`bVar4`/`bVar15` computation above, not yet traced) for a write site
that's supposed to *stop* the normal cycle progression while either is
set. This is a `-noanalysis`-unfriendly search (needs the RAM-address
xref capability from the full-analysis project, per part 18's earlier
correction) -- xref every write to `0x20010694` itself (already
partially done) and specifically look for one gated on `pbVar5[6]==1`
that *skips* the normal case `'\x01'`/`'\x02'`/`'\x03'` decrement
logic in `FUN_0800ca00`'s switch, since that decrement is what's
currently walking the state away from `5` every tick regardless of
menu-open intent.

**A second, independent hypothesis worth checking in parallel, tying
this to an *already-documented* known gap in this codebase**: even if
real firmware genuinely only draws menu content for a single transient
frame by design (plausible -- real hardware's continuous physical
scanout would show *some* version of a one-frame-only draw regardless
of exact timing), **QEMU's LTDC model might simply never capture that
frame at all**. `hw/display/gnw_h7b0_ltdc.c` (see comments around line
185-230) already documents a known, deliberate behavior change from
the 2026-07-11 flicker investigation: framebuffer capture is gated on
firmware's actual `SRCR.VBR` write instead of a fixed-timer vblank tick
-- and STATUS.md's "Known issues" section already flags an
acknowledged gap: *"content that never writes `SRCR.VBR`
(single-buffered/IMR-only paths) no longer gets captured at all."* If
this one-transient-frame menu draw doesn't happen to coincide with a
fresh `SRCR.VBR` write in the same window QEMU samples it, it would be
silently skipped -- explaining "the mechanism fires correctly but
nothing is ever visible" independent of whether the state-holding gap
above is also real. **Both threads are worth pursuing next session**;
they're not mutually exclusive (a genuine "menu should stay open
longer" bug in the state machine, *and* a genuine "even correct
single-frame content isn't guaranteed to be captured" gap in the LTDC
model, could both be contributing).

### Follow-up, same session (part 18) -- ran the full (non-`-noanalysis`) Ghidra auto-analysis pass, found the real, higher-level architecture

Per explicit instruction this session, ran a full auto-analysis pass
(`analyzeHeadless ... -analysisTimeoutPerFile 600`, no `-noanalysis`)
on the Mario Ghidra project -- this only took ~5 seconds and, crucially,
**enables real xref tracking to RAM addresses** (the `-noanalysis`
mode used everywhere earlier in this session could only find reads of
flash literal-pool *pointer* locations, never the RAM addresses those
pointers resolve to -- this was a real, self-imposed blind spot all
session). Should be the default going forward for this kind of
data-flow tracing; there's no real cost, `-noanalysis` was only ever
useful for extremely fast single-function spot-checks.

**Real xref search on `0x20010694` (RAM, full analysis) surfaced two
new write sites never seen in all the disassembly-following earlier in
this session**: `FUN_0800c604` (writes state `= 7` directly) and
`FUN_0800c62e` (writes state `= 9`, then conditionally calls
`FUN_0800c6c0(uVar3)` where `uVar3` is chosen from several config bytes
-- looks like "pick which submenu variant to open"). **Both are called
from within ~40 bytes of each other inside `FUN_08005e8e`** (the giant
per-frame dispatcher), specifically from a **third, previously
unnoticed state machine at `ctx+9`** (`ctx = DAT_080071fc =
0x20001580`, the same struct part 15's `ctx+0x770`/`0x772` gate lives
in -- but offset `+9` is a *different* field, values `0`-`5`, entirely
separate from both the `r5`/`0x20010694` fade-animation machine this
whole session chased and the `ctx+0x770`/`0x772` icon-decode gate from
part 15).

**This `ctx+9` state machine is the real screen/mode orchestrator**:
its `case '\x03'->'\x04'` transition calls `FUN_0800c604` (only when
`*(char*)(iVar3+10)` and a "target mode" value `uVar12` -- read from
`DAT_08007358[2]`, a persistent "requested next screen" latch --
combine correctly), and `'\x04'->'\x05'` calls `FUN_0800c62e()` when
`FUN_0800c6de(uVar12) == 8`. In other words: **the small fade-animation
state machine this whole session focused on (`0x20010694`) is not the
menu-open trigger itself -- it's a cosmetic transition effect *used by*
screen changes that this `ctx+9` machine orchestrates**, and the real
question is what sets `DAT_08007358[2]` (the requested-target-mode
byte) to the value that would make `ctx+9` walk `3->4->5` and open the
GAME-select or PAUSE-settings screen.

**Traced the write site for the RAM target of `DAT_08007358[2]`**
(resolves to `0x200010b6`): only one write xref found,
**`0x08006832`, itself inside `FUN_08005e8e`** -- i.e. this "requested
mode" byte is only ever written from within the same giant dispatcher
function, most likely as a self-clearing "consumed" write right after
the `ctx+9` machine reads it (matches the read site at `0x08006828`
being immediately before, `0x08006832`'s write right after) rather
than being *set* by a button press from here. **This means the actual
"GAME/PAUSE press requests a screen change" write must happen
somewhere else in the binary entirely** -- not yet found. This is now
the single most concrete, well-scoped next-session starting point:
find what writes a nonzero value into `DAT_08007358[2]`'s RAM target
(`0x200010b6`) or its sibling `DAT_0800735c` (the fallback/default
value read when `[2]` is `0`) anywhere else in the binary, using the
now-working full-analysis xref search, and check live whether a GAME
or PAUSE press ever reaches that write site.

**Revised mental model of the whole system, for next session**:
- `ctx+9` (`0x20001589`): top-level screen/mode orchestrator, `0`-`5`.
- `DAT_08007358[2]` / RAM `0x200010b6`: "requested next screen" latch
  consumed by the orchestrator -- **not yet found what sets this on a
  button press**, the real missing piece.
- `0x20010694` (`DAT_0800d00c`/`DAT_0800d2d4`): a fade/blink animation
  helper used *during* screen transitions the orchestrator drives, not
  the trigger itself -- most of this session's tracing (parts 15-18
  up to this point) was following a real, correctly-functioning
  subsystem that turned out to be one level too low to be the actual
  gate.
- `ctx+0x770`/`0x772` (part 15): a per-frame icon-decode gate,
  unrelated to either of the above, confirmed open/non-blocking.

## Part 19 (2026-07-13) -- RESOLVED. Real root cause: two QEMU LTDC bugs, not a firmware issue

Continuing directly from part 18's redirect toward the rendering layer.
The GAME/PAUSE menu content was being drawn correctly by firmware the
entire time (button input, state machine, and render dispatch were all
working, as parts 15-18 progressively proved) -- the actual gap was
entirely in QEMU's LTDC compositor, `hw/display/gnw_h7b0_ltdc.c`, and
turned out to be two separate bugs stacked on top of each other.

**Bug 1: LTDC pixel format 6 (AL44) was completely unimplemented.**
`gnw_h7b0_ltdc_capture_rows()`'s Layer2 bpp-classification switch had
no case for it, so it fell through to `l2_bpp = 0`, silently skipping
Layer2 compositing entirely even though the layer was enabled (`LEN=1`)
and had real content -- confirmed via targeted debug instrumentation
(temporarily added `fprintf`s in `gnw_h7b0_ltdc_capture_if_enabled()`
and the vblank tick, later reverted) showing capture *was* running
every frame with real, non-blank buffer bytes (`0xf0`/`0xf1`/`0xf2`
dominant), but Layer2 was never actually composited into the visible
frame.

Fixing this took two attempts. The first attempt decoded AL44 as a
direct 4-bit-alpha + 4-bit-luminance nibble split with no CLUT lookup
at all -- wrong. Checking `sdk/stm32h7xx-hal-driver/Src/stm32h7xx_hal_ltdc.c`'s
`HAL_LTDC_ConfigCLUT()` showed a special AL44 branch that loads CLUT
entries by index, suggesting AL44 is flat-256-CLUT-indexed like L8 --
also wrong, per the *actual* firmware behavior observed live: a
temporary debug `fprintf` on every `L2CLUTWR` write showed firmware
only ever loads the 16 "diagonal" indices (`0, 17, 34, ..., 255` --
i.e. `luminance*17`), confirmed live (`idx=0`, `idx=255` writes seen;
`idx=240/241/242/244`, the actual pixel byte values in use, never
written). **The correct model**: AL44's raw byte packs a 4-bit alpha
(upper nibble, applied directly, no CLUT) and a 4-bit luminance (lower
nibble, indexing a 16-entry CLUT sub-palette at `n*17` for RGB only).
Implemented as such in `gnw_h7b0_ltdc_capture_rows()`'s new `l2_al44`
branch.

**Bug 2: Layer compositing order was backwards.** Even after AL44
decoding was fixed correctly, still nothing showed. Added one more
temporary per-pixel debug trace (rate-limited to 8-15 hits) confirming
the AL44 decode was computing real, correct values -- e.g. `raw=0xf1
a4=15 l4=1 clut_entry=ffffffff` (fully opaque white, real anti-aliased
text). `capture_rows()` had Layer2 composited first/bottom and Layer1
second/top -- but real STM32 LTDC hardware always displays Layer2
*above* Layer1 (RM0455's LTDC overview states this explicitly). Since
Layer1 is RGB565 (no alpha channel, every pixel forced fully opaque
via `gnw_h7b0_ltdc_rgb565_to_pixel32()`), it was completely hiding
Layer2's overlay content underneath it regardless of how correct
Layer2's own pixels were. Fixed by restructuring the composite order:
Layer1 blended onto the background first, Layer2 blended onto *that*
result second (previously the reverse).

Both fixes are in `hw/display/gnw_h7b0_ltdc.c` (+ a new `LTDC_PF_AL44`
define in `include/hw/display/gnw_h7b0_ltdc.h`, and an updated header
doc-comment there). **User-confirmed fixed live** after the second fix
(layer order) landed: GAME/PAUSE menus now visibly render, and
GAME->A launches a game with the menu visible throughout. Every
application-level finding from parts 15-18 (button codes, state
machine, render dispatch chain) remains accurate documentation of the
actual control flow -- none of it needed to change; the bug was
entirely in the display compositor, not anywhere upstream of it.

**Process notes from this sub-investigation, worth keeping**:
- Verified the CFW-patched binary being run in QEMU is byte-identical
  to the unpatched stock binary Ghidra decompiles *at every address
  this session's tracing used* (diffed both images; the only
  differences near traced addresses were the already-known
  `read_buttons` call-site patch and the CFW wrapper's own injected
  code region) -- confirms the whole session's decompiled addresses
  were valid against the patched runtime, not a source of the earlier
  confusion.
- `qemu_log_mask(LOG_UNIMP, ...)` and ad hoc `fprintf(stderr, ...)`
  debug prints added directly to device model source, rebuilt via
  `ninja qemu-system-arm` in `build/`, are a fast, reliable way to get
  ground truth about internal QEMU device state that guest-side gdb
  reads can't see at all (e.g. `s->clut2[]`, `content_dirty`,
  per-pixel intermediate values) -- always remove them again once
  their diagnostic purpose is served, don't leave debug cruft in.

## Part 20 (2026-07-13) -- Mario CFW runs at ~half real-hardware speed: root-caused and fixed (CSI readiness), residual ~2x gap still open

Separate bug from part 19, reported after the menu fix landed.
User confirmed this is a real divergence by direct comparison against
real hardware (not just general expectation), which made a live
`gnwmanager` `OpenOCDBackend` comparison workflow (same live-CFW-image
methodology used throughout this project, just applied to Mario for
the first time this session) the right tool for the whole
investigation -- **used throughout this section, and it's what
actually cracked the case**; don't default to guessing when real
hardware is one `OpenOCDBackend` connection away.

**Initial finding**: QEMU's CPU never leaves HSI (64MHz) --
`RCC_CFGR.SWS` reads `0` (HSI) and `RCC_CR.PLL1ON` reads `0`
persistently, confirmed via multiple live samples throughout boot and
steady-state. Real hardware, read live via `OpenOCDBackend` while
running the *identical* CFW image, shows `SWS=3` (PLL1), `PLL1ON=1`,
`PLL1RDY=1` -- confirming this is a genuine QEMU-only divergence, not
stock-firmware-by-design HSI operation (real STM32 chips do always
*reset* to HSI, but this real-hardware read was taken mid-run, proving
firmware *does* successfully switch to PLL1 at some point during boot
on real hardware).

**False lead, ruled out**: a watchpoint on `RCC_CR` writes found the
CPU repeatedly calling the same function (`FUN_08017b20`, an RCC
de-init/reset-to-HSI-defaults routine, invoked via an indirect `blx`
from a single call site, confirmed via identical `lr=0x8017a49` across
many consecutive hits) -- looked like a stuck retry loop blocking PLL1
config. But this loop's behavior turned out to be genuinely
non-deterministic across separate fresh-boot runs of the *identical*
image: one run exited normally after ~20 real seconds; another showed
300 consecutive hits over 80 real seconds with zero change (stuck).
This inconsistency was itself informative (pointed toward a
timing/tick-source issue) but the loop itself is unrelated generic
retry/delay logic, not the actual PLL1-config gate -- don't re-chase
it directly.

**Found the real boot-time clock-config code** and confirmed PLL1
configuration *is* genuinely attempted, not skipped entirely:
- `FUN_08005c1c` is `SystemClock_Config()` -- calls `FUN_08001a10` (an
  `HAL_RCC_OscConfig()`-equivalent; its param struct, captured live,
  correctly specifies `PLLState=2`(ON), `PLLSource=0`(HSI), and real
  PLL1 M/N/P/Q/R divider values matching real hardware exactly --
  `PLLM=4, PLLN=11, PLLP=2, PLLQ=16, PLLR=2`) then `FUN_080016cc` (an
  `HAL_RCC_ClockConfig()`-equivalent; confirmed live, called requesting
  `SYSCLKSource=3` == PLL1, with a 5000-tick wait-for-`SWS`-match loop
  that returns error `3` on timeout).
- `RCC_PLL1DIVR` genuinely does get written with real values in some
  runs (confirmed via a live watchpoint) -- an earlier "zero absolute-
  literal references to `RCC_PLL1DIVR` anywhere in the binary" finding
  was a methodology gap, not evidence of absence: this firmware uses
  base-pointer + fixed-offset addressing (`str r3,[r1,#0x30]` for
  PLL1DIVR, immediately followed by `PLL2DIVR`/`PLL3DIVR` at `+0x38`/
  `+0x40` in the same straight-line sequence), which a simple
  absolute-address literal search can't find.
- `RCC_CR`'s write-mask (`GNW_H7B0_RCC_CR_WMASK = 0x151d129b`) does
  allow bit 24 (`PLL1ON`) to be written, symmetric with `PLL2ON`/
  `PLL3ON` (confirmed by direct mask inspection) -- ruling out a
  write-mask bug.

**Confirmed genuine run-to-run nondeterminism in whether/how firmware
even reaches PLL1 configuration**, not a fixed miscalculation: re-ran
the same coherent single-boot trace (input struct capture, then watch
for the `PLL1DIVR` write, same session this time) multiple times.
Across separate runs: (1) stuck forever in the `0x8017b38` retry loop;
(2) `PLL1DIVR` written once with what first looked like a corrupted
`N1=129` (later reconciled -- see below, this was a cross-launch
comparison artifact, not real corruption); (3) `PLL1DIVR` never
written at all within a 25s window, despite the input struct correctly
showing `PLLN=11` every time. The requested `OscillatorType` (`0x16`)
in the same struct asks for HSE(off) + LSE(on) + CSI(on) alongside
PLL1 in one call -- a real `HAL_RCC_OscConfig()`-style implementation
processes each requested oscillator's own enable-and-wait-for-ready
step, and a readiness check that can race/fail inconsistently would
explain exactly this pattern.

**Checked LSE first (RCC_BDCR) -- already correctly modeled**,
deliberately force-reporting ready regardless of `LSEON` (per an
existing code comment: real G&W hardware may not populate an LSE
crystal, but firmware spins on `LSERDY` regardless, so the model
always satisfies it) -- ruled out as the nondeterminism source.

**Checked CSI next -- found the real bug.** `RCC_CR`'s write handler
mirrored every *other* oscillator's `*ON` bit into its `*RDY` bit
(HSI, HSE, PLL1/2/3, and `RCC_CSR`'s LSI) but had no case for CSI at
all -- `RCC_CR_CSION`/`RCC_CR_CSIRDY` weren't even defined as
constants in `include/hw/misc/gnw_h7b0_rcc.h`. Confirmed live via the
real STM32H7B0.svd (`CSION`=bit7, `CSIRDY`=bit8): QEMU showed
`CSION=1` (firmware did enable it, as requested in the OscConfig
struct) but `CSIRDY=0` permanently -- the only oscillator whose ready
bit never got set.

**Fix**: added `RCC_CR_CSION`(bit7)/`RCC_CR_CSIRDY`(bit8) to the
header, and the matching instant-mirror logic to `hw/misc/gnw_h7b0_rcc.c`'s
`RCC_CR` write handler (identical pattern to every other oscillator
there). **Confirmed real effect**: `PLL1ON`/`PLL1RDY`/`CFGR.SWS` all
now correctly reach PLL1-selected state on every subsequent boot,
matching real hardware's steady-state exactly (previously permanently
stuck on HSI). Re-verified via `OpenOCDBackend` that every relevant
register now matches real hardware bit-for-bit after the fix --
`PLLCKSELR` (`DIVM1=4` both), `PLL1DIVR` (`0x010f020a` both, decoding
to `N1=11,P1=2,Q1=16,R1=2` on both -- the earlier `N1=129` reading was
from an *inconsistent* pre-fix run, not a persistent corruption; with
the CSI fix landed, PLL1DIVR now consistently matches real hardware
every boot), `CDCFGR1`/HPRE (`0x60` both), `CR`, `CFGR` all identical.
`gnw_h7b0_rcc_get_pll1p_hz()`'s formula was manually verified against
these matching values to compute the mathematically correct ~88MHz
PLL1P output.

**Still open**: despite every firmware-visible register now matching
real hardware exactly, a careful tick-rate measurement (of
`FUN_080015c0()`'s counter -- `*(DAT_080015c8+4)`, almost certainly a
`HAL_GetTick()`-style millisecond counter, RAM address `0x20001158`;
measured via a single tight, non-interrupted timing loop in one
Python script to rule out cross-tool-call measurement artifacts, which
had earlier given a misleadingly *faster*-than-real number) still
shows QEMU ticking at **~523/s vs real hardware's clean ~1000/s** --
almost exactly half, persisting even after the CSI fix. Since every
firmware-visible register now matches between targets, **this residual
gap is not a register/firmware-visible modeling gap at all -- it must
be somewhere in QEMU's own internal clock-propagation/timing code**,
not anything firmware or RCC-register-visible.

**Ruled out, narrowing the remaining search space**:
- `clock_update_hz()` already self-propagates internally
  (`clock_set()` + `clock_propagate()` in one call if the value
  changed -- confirmed by reading `include/hw/core/clock.h` directly)
  -- not a missing-propagation bug.
- The `sysclk` `Clock` object is architecturally sound: created once
  at the machine level (`hw/arm/gnw_h7b0.c`, initialized to the
  64MHz HSI bootstrap value), connected as the SOC's single "sysclk"
  input, and that exact same object is what both the ARMv7M core's
  `cpuclk`/`refclk` are wired to (`hw/arm/gnw_h7b0_soc.c`) *and* what
  RCC updates via `gnw_h7b0_rcc_set_sysclk()` -- confirmed by reading
  the wiring code directly, not a duplicate-object or mis-wiring bug.
- `hw/timer/armv7m_systick.c` (standard upstream QEMU ARMv7M code, not
  project-specific) registers proper `ClockUpdate`-event callbacks for
  both `cpuclk` and `refclk`
  (`systick_cpuclk_update`/`systick_refclk_update`, both calling
  `ptimer_set_period_from_clock()` unconditionally whenever the
  connected clock's rate changes) -- looked structurally correct, not
  an obviously-cached/stale-period bug, though this wasn't verified
  with live instrumentation, only by reading the source.

**Concrete next-session starting point**: this is now an extremely
narrow, well-isolated question -- every "is X correct" check on the
firmware/register/wiring side has been exhausted and confirmed
matching real hardware exactly; what's left is purely inside QEMU's
own clock-plumbing/timing internals. Add direct debug instrumentation
(temporary `fprintf`s, same technique that cracked part 19) to
`gnw_h7b0_rcc_update_sysclk_clock()` and `armv7m_systick.c`'s
`ptimer_set_period_from_clock()` call sites to see the *actual* period
values being computed and set at each point in boot, and correlate
against exactly when `FUN_08001a10`'s CSI-then-PLL1 configuration
completes relative to whenever firmware first enables SysTick --
timing/ordering between those two events (not a value-correctness
issue, since every value checked so far is correct) is the most likely
remaining candidate.

## Part 21 (2026-07-13) -- Zelda CFW comparison: confirms the slowdown is real and firmware-agnostic, but NOT a fixed ratio -- rules out a simple constant-factor bug

Booted `zelda-bank1-patched.bin`/`zelda-extflash-patched.bin` via
`scripts/boot_qemu.sh zelda --patched` to compare against Mario's part-20
findings, using a symbol-address-independent measurement this time
(architectural addresses only, no game-specific Ghidra symbol lookup
needed): set a breakpoint at the `SysTick_Handler` address resolved live
via `VTOR`+`0x3C` (`0x0801ad6a` for this image), then counted 2000 hits
via a tight gdb `while`-loop `continue`, bracketed by host
`date +%s.%N` calls.

**Result: ~415 SysTick fires/sec (2000 hits / 4.81s), vs. the expected
1000Hz** -- slower than Mario's ~523/s from part 20, not the same ratio.
Zelda's live clock-tree registers at the time of this measurement:
`RCC_CFGR.SWS=3` (PLL1 selected, locked), `PLLCKSELR.DIVM1=4`,
`PLL1DIVR` decodes to `N1=27,P1=2`, `CDCFGR1.HPRE=0`(no division) --
i.e. Zelda's firmware configures a considerably higher target SYSCLK
than Mario's (~216MHz-class vs. Mario's ~88MHz-class PLL1P from part
20), and its measured slowdown ratio is correspondingly *worse*
(0.415x vs. Mario's 0.523x), not identical.

**This is a real, useful new constraint on the bug**: a fixed/constant
QEMU-internal factor (e.g., an unconditional "compute period, then
double it" bug) would produce the *same* ratio regardless of the
actual target frequency. Instead the ratio scales with how high the
firmware asks the clock to go -- consistent with something computing
the SysTick/tick period against a **stale or baseline clock frequency
value** (e.g. captured before PLL1 finished locking, or before the
final `HPRE`/prescaler value lands) rather than the final locked
frequency, where a bigger gap between the stale baseline and the true
final frequency produces a proportionally worse ratio. This directly
supports and sharpens part 20's suspicion of a timing/ordering bug
between PLL1-lock completion and SysTick's clock-update callback
firing, rather than a simple missing division/multiplication bug.

**Also tried and set aside**: measured the DWT `CYCCNT` cycle counter
(`0xE0001004`, enabled via `DWT_CTRL`/`0xE0001000` and
`DEMCR.TRCENA`/`0xE0000EDC`) across a fixed 5-real-second window, for
both Mario and Zelda, expecting it to reveal the true effective
`cpuclk` rate independent of firmware symbols. Both games measured
**~281MHz**, regardless of Zelda's ~216MHz-class vs. Mario's ~88MHz-
class configured PLL1P target -- i.e. `CYCCNT` reads back a fixed rate
uncorrelated with either game's actual configured SYSCLK. This means
`CYCCNT` is not a trustworthy proxy for `cpuclk`'s modeled frequency in
this QEMU build (plausibly free-running off host/icount heuristics
rather than the `cpuclk` `Clock` object) -- don't use it for future
clock-rate cross-checks; the SysTick-handler-hit-counting method above
is the one that gave a real, game-comparable signal.

**Next-session starting point, sharpened**: the part-20 plan to
instrument `gnw_h7b0_rcc_update_sysclk_clock()` and
`ptimer_set_period_from_clock()`'s call sites with temporary
`fprintf`s is still the right next step, but now specifically look for
whichever one runs *before* PLL1 lock completes using a baseline/stale
frequency, since the frequency-dependent (not fixed) ratio strongly
implicates a stale-value-at-computation-time bug over a pure formula
bug.

## Part 22 (2026-07-13) -- real lead: this looks like host-throughput/emulation-speed bound, not (only) a clock-formula bug

Done without real-hardware access (temporarily unavailable this
session) -- purely QEMU-side investigation, to be confirmed against
real hardware next session.

**Key observation**: `qemu-system-arm` sits pegged at **101% CPU**
(one host core fully saturated) throughout steady-state Zelda CFW
execution (`ps -o pid,pcpu`). No `-icount` is passed anywhere in
`scripts/boot_qemu.sh` or the board's own launch args, so
`QEMU_CLOCK_VIRTUAL` tracks real host wall-clock time directly
(unthrottled by, and independent of, how many guest instructions TCG
actually manages to execute) -- and TCG is single-threaded under the
BQL, so any MMIO/device-model work directly steals from the same
budget as guest instruction execution.

**Decisive test**: relaunched the identical Zelda CFW image with
`-display none -audio none` (removing all SDL/PulseAudio host-side
overhead) on a second gdbstub port (`-gdb tcp::1235`, since the
existing `-s` default port was still in use by the visible instance --
note for future sessions: always check `pgrep -af qemu-system-arm`
before assuming a port is free, don't leave stray background instances
running past their measurement's usefulness). Re-ran the same
SysTick-hit-counting method from part 21: rate rose from **~415Hz ->
~587Hz**. Still well short of the real 1000Hz, but a large, real jump
purely from removing rendering/audio work.

**Why this matters**: a pure clock-frequency/formula bug (wrong Hz
value fed into `ptimer_set_period_from_clock()`) would produce the
*same* tick rate regardless of whether SDL/PulseAudio are attached --
it doesn't care about unrelated host-side rendering work. The fact
that removing display/audio measurably speeds up the tick rate means
at least part of (possibly all of) the "slowdown" is QEMU failing to
execute this workload's actual instruction+MMIO volume in real time on
this host. `armv7m_systick.c`'s ptimer uses
`PTIMER_POLICY_WRAP_AFTER_ONE_PERIOD` (no catch-up), so if the host
falls behind and only gets around to servicing a timer after its
virtual deadline has already passed, it reprograms for "one period
from *now*" rather than catching up the backlog -- under sustained
host CPU saturation this silently drops/coalesces ticks, which would
look exactly like "everything is slower," scale with render/device-
model load (matches Zelda vs. Mario's differing ratios from part 21),
and still leave every firmware-visible register matching real hardware
exactly (since it's a scheduling/timing-delivery problem, not a value-
correctness one) -- consistent with every finding in parts 20-21.

**Residual gap even headless (587 vs. 1000Hz)** means host-side
render/audio overhead isn't the whole story -- there's still real per-
instruction/MMIO cost beyond that, most likely the already-documented
register-access-heavy device models (JPEG/DMA2D, OCTOSPI polling
loops -- see STATUS.md's "Known issues" section).

**Concrete next-session starting point**: this reframes the
investigation from "find a clock-propagation formula bug" toward
"profile where QEMU's single host core's time actually goes" --
`perf record -p <qemu-pid> -- sleep 5` (or `perf top -p <pid>`) during
steady-state gameplay, on both a heavy screen (coverflow/menu) and a
light one (static clock face), to find the actual hot functions eating
the CPU budget. Confirm against real hardware once available again:
if real hardware also can't reach 1000Hz on a *host* comparison this
doesn't apply (real hardware has no such artificial bottleneck by
definition), but the interesting confirmation is whether the ratio
gets closer to 1:1 on lighter screens in QEMU specifically, which
would nail down render-load-proportional slowdown as the dominant
factor over any remaining formula-level bug.

**Correction (2026-07-13 part 5, `docs/session-2026-07-13-part5-retro-go-ltdc-vbr-and-stutter-investigation.md`)**:
the specific mechanism claimed above -- that `armv7m_systick`'s ptimer
"reprograms for one period from now" under load with "no catch-up" due
to `PTIMER_POLICY_WRAP_AFTER_ONE_PERIOD` -- turned out to be inaccurate.
Direct reading of `hw/core/ptimer.c`'s `ptimer_reload()` shows its
deadline math (`s->next_event = s->last_event + delta * period`)
correctly chains from the previous scheduled deadline, not from "now,"
so the ptimer layer itself does properly attempt to catch up. The real
mechanism is one level up: SysTick's pending-IRQ state is a single bit,
not a counter, so a tick that fires while the CPU is still busy
delivering/running the *previous* SysTick interrupt is silently lost
regardless of how correctly the ptimer scheduled it. The empirical
finding here (headless mode measurably raises the SysTick rate) is still
correct and remains the right supporting evidence for host-throughput
correlation -- only the specific ptimer-policy explanation was wrong.

## Part 23 (2026-07-13) -- real fix: DMA2D had the same per-pixel MMIO pattern as LTDC, worse; ~55% cumulative SysTick-rate improvement so far, still short of real-time

Followed up on part 22's LTDC per-pixel fix by checking DMA2D
(`hw/display/gnw_h7b0_dma2d.c`) for the same anti-pattern, since
`perf record` showed `gnw_h7b0_dma2d_write`/`gnw_h7b0_dma2d_read_argb8888`
in the profile. Found it was actually **worse** than LTDC had been:
`gnw_h7b0_dma2d_do_transfer()`'s `M2M_PFC` and `M2M_BLEND`/`_FG`/`_BG`
modes called `cpu_physical_memory_read()`/`_write()` once *per pixel*
for every format, and the YCbCr (JPEG cover-art) path did **three**
separate physical reads per pixel (Y/Cb/Cr planes) -- directly on the
coverflow cover-art rendering path STATUS.md already flags as the most
expensive on-screen content. L8's CLUT lookup was a second physical
read per pixel on top of the pixel data itself (fetching the same
256-entry CLUT over and over, once per pixel, for the whole transfer).

**Fix**: same batching pattern as part 22's LTDC fix --
`gnw_h7b0_dma2d_read_argb8888_buf()`/`_write_output_buf()`/
`_read_ycbcr_buf()` now decode/encode from/to a row buffer the caller
fetches with one `cpu_physical_memory_read()`/write per row (per
Y/Cb/Cr plane for YCbCr), and `gnw_h7b0_dma2d_load_clut()` loads the
256-entry CLUT once per transfer instead of once per pixel. `R2M`
(solid fill) also now builds one encoded pixel row once and reuses it
for every line instead of re-encoding the same constant color
`pixels_per_line * lines` times. Old per-pixel-address functions
(`gnw_h7b0_dma2d_read_argb8888`/`_read_ycbcr`/`_write_output`) removed
entirely -- no remaining callers.

Also removed a stray leftover debug `fprintf(stderr, "[dma-debug]...")`
in `gnw_h7b0_dma_stream_tick()` (`hw/misc/gnw_h7b0_dma.c`) -- capped at
40 prints via a static counter so it wasn't an actual sustained cost,
but stray debug cruft per CLAUDE.md's "always remove temporary
fprintf()s" rule.

**Measured effect** (same SysTick-hit-counting method as parts 21/22,
Zelda CFW, `-display sdl -audiodev pa`):
- Baseline (before part 22): ~415Hz
- After part 22's LTDC Layer2 batching fix: ~453Hz
- After this session's DMA2D batching fix: **~643Hz**

`perf record` after this fix shows the flatview/phys_page_find/
address-translation cluster that dominated parts 22's profile has
**disappeared entirely** from the hot list. What remains is legitimate
work: `cpu_exec_loop`/`arm_get_tb_cpu_state`/`cpu_tb_exec` (~67%
combined -- baseline TCG interpretation cost) and real per-pixel blend
math (`gnw_h7b0_ltdc_blend_over` ~4%, `gnw_h7b0_ltdc_capture_if_enabled`
~2%). QEMU still shows ~100-102% CPU (expected/normal for default
non-`-icount` TCG, which always runs the vCPU thread flat-out
regardless of whether the guest needs it) -- CPU% isn't a useful signal
here, the SysTick-rate number is.

**Still short of real-time** (643Hz vs. 1000Hz target) -- the
remaining gap is now genuine TCG-interpretation/per-pixel-blend
compute cost, not wasted address-translation overhead, so further
gains need either core-QEMU TCG tuning (translation block
caching/sizing) or reducing the LTDC blend math's own per-pixel cost
(e.g. fast-pathing the common case where Layer2 is disabled or a
region has no color-key/window-clip to check), not another instance of
this same batching pattern. Worth re-confirming this whole 415->643Hz
progression against real hardware once it's back, and profiling
`gnw_h7b0_ltdc_blend_over`/`gnw_h7b0_ltdc_capture_if_enabled`
specifically as the next concrete target if more speed is needed.
