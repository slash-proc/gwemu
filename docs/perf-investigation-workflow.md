# Performance investigation workflow

A known-good loop for finding and fixing performance problems in this fork.
Written after it produced two real TCG wins (−24% and −27% host CPU) and five
documented refutations in a single session, at low context cost.

It is not specific to TCG. Anything with a repeatable workload and a trustworthy
number fits it. Companion: `docs/tcg-perf-excursions.md` (what this loop found).

## The loop

One cycle, repeated:

1. **Hypothesis** — from a profile or from source, never from intuition alone.
2. **Delegate** a single, tightly-scoped experiment to an agent.
3. **Measure** it — interleaved A/B, median of 3, against a known noise floor.
4. **Verify** the measurement before believing it (see *Verification*).
5. **Bank or refute** — commit the win, or record the negative with its data.
6. **Re-profile** — the next hypothesis comes from the *new* profile, not the
   old plan. The bottleneck moves after every win.

Step 6 is what makes it converge. After the jump-cache win, `tb_htable_lookup`
vanished from the profile and `helper_lookup_tb_ptr` *rose* to 50% — which
redirected the whole investigation from "make lookups hit more often" to "make
fewer lookups", and that reframing is where the second win came from.

## Orchestrator / agent split

The orchestrator (main session) holds the *thread of reasoning*. Agents hold the
*work*. This is the reason context barely moves across many experiments.

**Orchestrator does:** form hypotheses, write briefs, read diffs on
correctness-critical changes, decide bank-vs-refute, commit, keep the state doc
current.

**Agents do:** builds, benchmark runs, source spelunking, instrumentation,
sweeps. All the token-expensive, low-information-density work.

**Agents return numbers and file:line citations, not narrative.** Ask for "max
25 lines" and a fixed report format. A good agent report is a table plus a
verdict; if it needs prose to explain itself, the brief was underspecified.

Practical notes:
- Never read an agent's raw transcript file — it will overflow context. Only its
  final report.
- Run agents **serially** when they build or benchmark. Concurrent CPU load
  corrupts every timing number on the machine. Parallelise only read-only work.
- If an unrelated task arrives mid-investigation (a bug, a different subsystem),
  give it a **worktree** agent and explicitly forbid it from building the main
  project or running benchmarks, so it cannot disturb a measurement in flight.

## The agent brief

Briefs that worked share a shape. Include all of it — each line below exists
because omitting it cost a run:

- **Established facts, marked "do not re-derive"**, plus what was already tried
  and refuted. Otherwise agents rediscover known things and re-run dead ends.
- **The metric, and what makes a run invalid.** State the failure signature
  ("no prof lines means the run FAILED — report it, never invent a number").
- **Interleave, don't batch** — spell this out, with the reason.
- **Explicit permission to fail.** "Concluding this is unsafe is a SUCCESSFUL
  outcome" and "if it's within noise, say so plainly — that is a useful result"
  measurably change what comes back. Without it, agents find a win.
- **Machine hazards** (below).
- **Required end state**: leave the tree clean / leave the change applied, and
  confirm with `git diff --stat`.
- **A report format**, with a line budget.

Split research from implementation when a change touches a correctness
invariant: phase 1 establishes *why* the current code is the way it is, and only
a "safe" verdict unlocks phase 2. That structure is what produced a usable
cross-page `goto_tb` patch instead of a plausible-looking wrong one.

## Metric discipline

**Pick a metric that can see the thing you are changing.** The single most
expensive mistake in this investigation was using the guest's `ips` counter for
host-side optimisations. It is capped by the guest's own speed profile, so on any
scene with idle headroom it reports the same value no matter how fast the host
is. Several knobs were nearly written off as "no effect" because of it.

Rules that follow:
- Prefer a **host-side** metric for host-side changes (here: host CPU-seconds
  over a fixed timeline, from `/proc/PID/stat`).
- **Establish the noise floor first** — 3 identical runs — and never report a
  delta smaller than it.
- **Cross-check with an independent metric.** The guest's own DWT counters
  (`cpu%`/`idle%`/`cpi`) moved in step with host CPU on both real wins, and that
  agreement is much stronger evidence than either number alone.
- Fix guest *work*, not wall time. Replaying a recorded timeline under the
  virtual clock makes both arms do identical work; a wall-clock-bounded run does
  not, and the comparison is then invalid.
- A win on a scene with idle headroom buys **headroom, not frames** — it shows
  as increased idle, and converts to speed only where the guest saturates.

## Verification

Assume every result is wrong until it survives these:

- **Interleave arms** (A,B,A,B), never batch. Drift otherwise fakes results.
- **Re-measure the baseline on the current tree.** A baseline taken before an
  unrelated fix is not a baseline. One 20% win here had to be re-established
  after the harness itself changed — it survived, and grew to 24%, but it could
  as easily have evaporated.
- **Reproduce independently before committing** — a separate rebuild, ideally by
  the orchestrator rather than the agent that found it.
- **Run the negative control.** The sharpest result of the session: deliberately
  neutering a safety mechanism produced *bit-for-bit identical correct output*,
  proving the workload had no power to detect that mechanism failing. Without
  that control, a passing test suite would have been read as evidence of safety
  when it was evidence of nothing. **If you cannot break it deliberately, you
  have not tested it.**
- **Check whether the workload can even exercise the thing.** Headless mode once
  disabled the very display path being A/B'd, giving 0% in both arms.

## Git usage

- **One branch per excursion**, kept even when the excursion is rejected:
  `perf/<short-name>`. A refuted branch is a record that the idea was tried and
  what it measured.
- **Commit the experiment to its branch before reverting it.** Two refuted
  experiments here were reverted by agents before this rule existed, so only
  their numbers survive and the code is gone.
- **Commit messages carry the data**, not just intent: both arms' raw numbers,
  the median, the noise floor, the method, and the residual risks. The commit is
  the durable record; a chat log is not. Anyone re-tuning the value later should
  find the sweep that says not to.
- **Default-off for anything whose safety net is untested.** An opt-in env knob
  with a documented gating condition is a legitimate deliverable; silently
  enabling it is not.
- Keep a gitignored **state file** in the working directory (here
  `backup/dos-perf/STATE.md`) holding current numbers, traps, and queued
  experiments, so an interrupted or compacted session resumes without re-deriving.

## Machine hazards

These bit repeatedly. Put them in every brief.

- **Never `pkill -f <pattern>`** — it matches the agent's own shell command line
  and kills it, so cleanup never runs and stray processes accumulate. Use
  `pkill -x <name>`.
- **`ninja -j12`, never bare `ninja`** — bare ninja takes every core and breaks
  concurrent work on this box.
- **Never SIGKILL the emulator** — flash images are live-mapped and a hard kill
  can corrupt them. Shut down through QMP `quit`.
- **`/tmp` is RAM.** Perf captures (~100MB each) go on real disk, and get
  deleted between batches.
- **One heavy job at a time.** Any concurrent build or run invalidates timings.

## When an excursion ends

An excursion is done when it is either committed with its measurements, or
recorded as refuted with the data that refutes it. "Tried it, didn't seem to
help" is not a conclusion — it is an unfinished excursion, and it will be
retried by someone six months from now.
