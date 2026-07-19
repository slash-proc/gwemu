# 2026-07-16 — TCG dispatch overhead investigation (unresolved, parked)

## Context

While chasing a real gameplay "stutter" (patched Zelda ROM + retro-go,
same scenario as the 2026-07-14 frame-integrator investigation), a fresh
`perf record --call-graph dwarf` profile was captured live against a
running `qemu-system-arm` process during an active stutter burst
(20s @ 999Hz, ~16-20K samples). This doc records what was found, what was
ruled out, and the concrete next steps for whoever picks this back up —
this was NOT root-caused before the session moved on to other work.

## What the profile showed

Two dominant, unrelated cost centers, together ~48% of all cycles:

1. **`helper_lookup_tb_ptr` (23%) + `arm_get_tb_cpu_state` (12%) — QEMU
   TCG core, not this project's code.** Almost entirely inside
   `tb_lookup` (inlined) -> `tb_jmp_cache_hash_func` (inlined). This is
   the per-block-transition dispatch helper QEMU's generated code calls
   whenever it can't take a direct native jump (`goto_tb`) to the next
   translated block.
2. **`gnw_h7b0_ltdc_blend_over`/`capture_if_enabled` (~13%) — this
   project's own LTDC device model.** Investigated separately (see the
   2026-07-16 agent dispatch in this session) and concluded to be
   correct, expected behavior (real per-`lcd_swap()` VBR reload cost,
   amplified by the already-known frame_integrator firmware behavior),
   not a device-model bug. Not discussed further in this doc.

This doc is about cost center #1 only.

## What was ruled out

- **NVIC/interrupt-dispatch overhead as the cause.** No meaningful time
  in `arm_cpu_do_interrupt`, `nvic_recompute_state` (0.23%), or any
  exception-dispatch path. If frequent IRQ delivery were forcing TBs to
  end in non-linkable exits, this class of symbol would be far higher.
  Clean negative result.
- **Full `tb_flush()` churn.** Confirmed via a live `-trace
  enable=tb_flush,file=...` run (no special permissions needed — this
  worked where host-level `gdb`/`perf probe` did not, see below):
  **zero** `tb_flush` events over 90+ seconds of sustained, CPU-busy
  (~104%) real gameplay. This rules out "the whole TB cache keeps
  getting nuked and every block needs full re-translation+re-linking"
  as the mechanism — that theory is dead.
- **Our own device models directly calling `tb_flush`/`tlb_flush`.**
  Grepped every `hw/misc/gnw_h7b0_*.c` / `hw/display/gnw_h7b0_*.c` /
  `hw/arm/gnw_h7b0_*.c` file — none do. Whatever's happening is generic
  QEMU-core machinery, not a device-model bug calling these functions
  directly.
- **TB jump-cache thrashing.** Within `helper_lookup_tb_ptr`'s own
  breakdown, ~89% of the cost is the *hit* path
  (`tb_jmp_cache_hash_func`, i.e. computing/checking the cache key) and
  only ~11% is the slow full-hashtable miss path (`tb_htable_lookup`).
  So recently-run blocks are still generally findable in the per-CPU
  jump cache — the cache itself isn't unhealthy.

## What's still unresolved (two live hypotheses)

The clean ruling-out above actually *sharpens* the puzzle: if the jump
cache is healthy and full flushes never happen, then `helper_lookup_tb_ptr`
is being called this often (35% of all cycles between the two dispatch
symbols) because **direct TB-to-TB chaining (`goto_tb`) specifically
isn't engaging** for a large fraction of block transitions — every one of
those transitions falls through to the generic dispatcher instead of a
native jump, even though the target block usually already exists in the
cache.

Two candidate explanations, not yet distinguished from each other:

1. **Inherent cost of call/return-heavy C code under TCG.** Any indirect
   branch — and that includes every ordinary function *return*
   (`bx lr`, `pop {pc}`), not just computed jumps — can't be statically
   chained by `goto_tb`, because the return address varies by call site
   and isn't known at translation time. Retro-go's SNES/GBC core code is
   ordinary structured C, not a tight loop, so a high rate of
   un-linkable dispatch from routine call/return traffic alone could
   plausibly explain most of this. If true, this is **not a bug** —
   it's the same class of conclusion the 2026-07-13/14 SysTick
   investigation already reached (`-icount` is the only real lever, and
   it was already measured to cost ~27% more host CPU for zero
   throughput gain — not adopted). No file in this repo would need to
   change; this would just be an accepted cost of the target/workload
   shape.

2. **Self-modifying-code-style page invalidation from retro-go's own RAM
   layout.** A wildcard performance audit run the same session (see
   below) raised a concrete, plausible alternative: retro-go loads game
   cores (SNES/GBC emulator cores) into RAM (AXISRAM) and executes them
   from there for speed rather than executing in place from flash. If a
   core's *mutable* data (save state, audio scratch buffers, working
   RAM) shares a page with that same core's *executable* code, every
   write QEMU sees to that data trips its conservative
   self-modifying-code write-protection for the whole page, forcing
   `tb_invalidate_phys_page_range()` + retranslation for code that never
   actually changed. This would show up as exactly the kind of
   dispatch-helper-heavy profile observed here (freshly retranslated
   blocks start unlinked and only get chained again once revisited)
   while still being consistent with zero `tb_flush()` events (this is
   page-level, not global). Supporting evidence already in the profile:
   `tb_invalidate_phys_page_range__locked` (1.8%), `page_find_alloc`
   (1.2%), `page_trylock_add` (1.0%), `page_collection_lock` (0.9%),
   `tb_page_addr_cmp` (0.8%), `physical_memory_get_dirty`/`set_dirty_range`
   (1.4% combined) — a real, measurable cluster, just much smaller than
   the 35% headline number, so it would only be a *partial* explanation
   even if fully confirmed.

**These two are not mutually exclusive** — the true answer may be "mostly
#1, with a real but smaller contribution from #2."

## Where a fix would actually live, if #2 is confirmed

This matters because the two live hypotheses point at completely
different codebases:

- **If #1 (call/return dispatch cost, inherent):** no fix available
  short of a fundamentally different execution strategy (already-rejected
  `-icount`, or a much larger TCG-chaining research project) — nothing in
  this repo to change.
- **If #2 (code/data page-sharing in retro-go's RAM layout):** the real
  fix would be in **`game-and-watch-retro-go-sd`** (a sibling repo, the
  actual retro-go firmware project this SoC boots) — specifically its
  core-loading linker script / memory map, to separate a RAM-resident
  core's `.text` from its mutable data onto distinct pages. That's a
  legitimate firmware-side contribution (unlike stock Nintendo firmware,
  which this project treats as fixed/unmodifiable input per CLAUDE.md) —
  but it is emphatically **not a qemu-gnw change**; nothing here would
  need to touch this repo's own device models to fix it. QEMU's own
  generic TCG self-modifying-code detection (`accel/tcg/`) is fundamental
  machinery shared by every target and should not be touched to
  special-case one guest's memory layout.

## Why this wasn't finished this session

Confirming which of the two hypotheses (or what mix) is actually
responsible requires a live call-rate/address measurement — specifically,
counting `tb_invalidate_phys_page_range()` calls and inspecting which
guest physical addresses they target (code region vs. known mutable-data
region) during a live stutter. Two avenues were blocked:

- **Host-level `gdb -p <pid>` attach**: blocked by
  `/proc/sys/kernel/yama/ptrace_scope = 1` with no passwordless `sudo`
  available in this environment.
- **`perf probe` / uprobes** (to add a dynamic tracepoint on
  `tb_invalidate_phys_page_range__locked` without attaching a debugger):
  blocked by `/proc/sys/kernel/perf_event_paranoid = 2`, also requiring
  root.

**What *did* work without special permissions**: QEMU's own built-in
`-trace enable=<event>,file=<path>` mechanism (used successfully above to
get the definitive `tb_flush` = 0 result). The concrete next step is to
re-run with a trace event that actually exists in this build for the
finer-grained signal — check `qemu-system-arm -trace help` for
`exec_tb`/`exec_tb_exit`/`translate_block` (all confirmed present in this
build's trace-events list) and correlate their rates/arguments against a
live stutter session. This doesn't need root and wasn't tried before the
session moved on.

## Concrete next steps for whoever picks this up

1. Re-run with `-trace enable=exec_tb_exit,file=...` (or
   `translate_block`) during a reproduced stutter, and look at call
   rate and, if the trace event carries a PC/address argument, whether
   retranslated addresses cluster around a specific RAM range.
2. Cross-reference any implicated address range against retro-go's
   actual RAM layout (where loaded cores + their working data actually
   sit) — this needs looking at `game-and-watch-retro-go-sd`'s own
   linker scripts/core-loading code, a different repo.
3. If hypothesis #2 is confirmed as a material contributor, the fix
   belongs in `game-and-watch-retro-go-sd`, not here — scope it there.
4. If hypothesis #1 turns out to dominate, this is very likely a dead
   end absent a much larger TCG-execution-strategy change; don't keep
   re-chasing it without new information.
5. If temporary root/sudo ever becomes available in this environment,
   host-level `gdb -p <pid>` or `perf probe` would make this much faster
   to nail down directly (real call counts + backtraces) instead of
   inferring from trace-event rates.

## Related session work (same day)

A parallel four-agent "expert panel" was also dispatched this session to
look for other performance wins from different angles: safe off-thread
LTDC compositing, hand-written SIMD for pixel-math hot loops, finishing
an earlier-abandoned PGO (profile-guided optimization) pipeline, and a
general wildcard hot-path audit (the one that raised hypothesis #2
above). See CHANGELOG.md's 2026-07-16 entries for their individual
outcomes once landed/reviewed.
