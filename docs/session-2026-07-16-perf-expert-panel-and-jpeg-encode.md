# 2026-07-16 (later) — performance "expert panel" + real JPEG encoder + a newly-exposed LTDC crash

## Context

Following the TCG-dispatch-overhead investigation (see
`docs/session-2026-07-16-tcg-dispatch-overhead-investigation.md`), the
project owner asked for a wider, more aggressive perf push: a panel of
specialist agents covering safe off-thread device-model work, hand-written
SIMD, finishing the earlier-abandoned PGO pipeline, and a general
wildcard audit — "find SOMETHING." Later expanded to threading three more
device models (JPEG, DMA2D, HASH/CRYP) beyond the initial LTDC compositor
work. This doc records the real outcome of all of that, plus two
significant side-findings that came out of verifying it (a silently
uncommitted device model, and a newly-exposed pre-existing crash).

## Current diag-suite baseline (as of this doc, main checkout)

Ran fresh against the current main-checkout build:
**63 OK / 39 FAIL / 8 UNRUN, out of 110 total cases.**

Notable: the run did NOT crash this time (see "the LTDC crash" section
below — it's real but apparently timing/state-dependent, not 100%
reproducible on every single run) — it instead **hung** (0% CPU, alive,
not crashed) at `jpeg_encode_correct`, leaving the 7 cases after it
UNRUN. This matches the JPEG-encode-not-implemented root cause exactly
(see below): `HAL_JPEG_Encode()`'s completion flag never gets set on
this build, so firmware's poll loop spins forever and the diag harness's
own case-sequencer never advances past it.

Most of the 39 FAILs are **pre-existing, unrelated to anything this
session touched** (RNG, most crypto/HMAC combinations, several
timer/DMA/GPIO cases) — this doc is not claiming to have fixed the
overall pass rate, only documenting where things stand and what's newly
understood.

## What was built, verified, and where it stands

All of the following are **uncommitted, preserved on their own named
worktree branches** (`worktree-agent-<id>`), not merged into the main
tree. Every one was independently rebuilt and re-verified by the
coordinator directly (not taken on the dispatched agent's self-report
alone) before being trusted.

### 1. LTDC compositor worker-thread (real win, verified)
Moves per-frame pixel compositing off the vCPU/BQL thread onto a
dedicated worker (job queue, mutex/condvar dispatch, front/back
ping-pong publish buffer, explicit wait-for-idle teardown). First attempt
was built against a stale ~10-commit-old base and silently deleted
several since-landed mechanisms (RAM-dirty-bitmap fallback, `vbr_active`,
`structural_transition_pending`, the NDEBUG segfault guard) — caught in
review, sent back, correctly redone preserving all of them. Independently
rebuilt and verified: all 4 diag LTDC cases OK, `ltdc_frame_composite`
dropped from ~17.8M to ~118K DWT cycles, 43+ second real-gameplay soak
test with no crash. Branch: `worktree-agent-a42415f0520ff768b`.

### 2. JPEG async-decode worker-thread (real win, verified — separate from the encode work below)
Moves the actual `stb_image`-based decode+YCbCr-conversion math to a
worker thread; the vCPU-side poll loop (mirroring real firmware's own
polling) does a non-blocking check each read instead of blocking
synchronously for the ~8.2ms a real cover-art decode takes. Firmware's
own poll loop gains nothing directly (it's a tight, uninterruptible
poll — verified against `sdk/stm32h7xx_hal_jpeg.c`), but the real win is
that decode no longer holds the BQL for that whole window, so it stops
freezing every other main-loop consumer (audio pacing, display-refresh
timers) during cover-art loads. Verified bit-identical against 5 real
cover-art JPEGs. Branch: `worktree-agent-ae3e4e3d0aa6d749c`.

### 3. DMA2D async-transfer worker-thread (built, correctness-verified, no measured win — parked per project owner's explicit "don't throw it away")
Initially declined (real firmware usage is `HAL_DMA2D_Start()` +
`HAL_DMA2D_PollForTransfer()` with a hardcoded 50ms timeout and zero
interleaved work — no obvious win). Project owner asked for it to be
built and tested anyway (reported hitting that 50ms timeout in practice).
Built it; found and fixed a real bug in the process (`CR.START` — a real
hardware busy-bit — was being cleared too early once the transfer became
async, breaking `PollForTransfer`'s wait-loop entry condition; real
hardware keeps it set until completion). After the fix, all 4 diag DMA2D
cases pass. Timing: no win (`dma2d_fill` 8.2ms baseline vs 10.4ms
threaded — slightly *slower*, added synchronization overhead with
nothing to overlap). Kept per explicit instruction not to discard it —
may be useful once combined with the other threading work and evaluated
in aggregate (the "frees the main thread for other concurrent work"
argument, not yet measured in combination). Branch:
`worktree-agent-a8b086d79117b4e21`. **Decision as of this doc: parked,
not merged, revisit once LTDC/JPEG threading are actually in the main
tree and can be measured together.**

### 4. HASH/CRYP threading — declined, no code built (correctly)
CRYP triggers one 16-byte AES block per `DIN` write — sub-microsecond of
real work per trigger, far too fine-grained for thread-handoff latency to
ever pay off. HASH: at the time this was evaluated, the device didn't
even exist in the actual committed git history (see below) — nothing to
thread. Also: zero diag-suite coverage exists for CRYP specifically. Good
evidence-based no, no speculative code written.

### 5. Hand-written SIMD for LTDC pixel conversion (real, small win, verified)
Audited every per-pixel/per-row loop in `gnw_h7b0_ltdc.c`/`gnw_h7b0_dma2d.c`
via objdump, not assumption — most are already GCC-auto-vectorized
(confirmed, not guessed) and were left alone. Found one real gap: the
scalar RGB565-to-ARGB8888 conversion in the hot per-pixel blend loop.
Built an SSE2 8-wide/16-bit-lane version (a first "obvious" 4-wide/32-bit
version was benchmarked and found *slower*, 0.82x — rejected; AVX2 was
also tried and rejected for inconsistent cross-lane-shuffle overhead).
Shipped version measured ~1.3-1.9x. Verified bit-exact against all 65536
possible RGB565 values plus 5000 random-buffer trials, and independently
re-verified by the coordinator against the real diag suite (applied
directly to the main checkout, all LTDC/DMA2D diag cases OK). **This one
IS already applied to the main checkout working tree** (uncommitted),
unlike the others which remain on their own branches — it's low-risk,
additive-only, and already diag-verified in place.

### 6. PGO (profile-guided optimization) — pipeline proven, no adopted verdict yet
Finished the pipeline this session started earlier in the day and never
completed: confirmed the earlier release+`-O3`+PGO-generate segfault
(from `hw/display/gnw_h7b0_ltdc.c`'s dirty-range check) is likely fixed
by commit `1249b01841` in headless mode (6/6 clean runs) but still
crashed 2/5 times under `-display sdl` — plausibly host-display
contention from other agents' concurrent SDL windows this session, not
confirmed as the original bug resurfacing. Got real `.gcda` profile data
via a clean QMP `quit` (not kill) after a real ~95s workload run —
1581 real files, verified by timestamp/size. Built a real `b_pgo=use`
binary. Benchmarked: 0.75-4% fewer cycles, but run-to-run variance on
this heavily-contended host (~6%) was bigger than the claimed effect —
correctly called this **noise, not a demonstrated win**, and recommended
against adopting yet. **Next step, not yet done**: re-run the same
benchmark on a quiet/uncontended host using guest-side DWT CYCCNT
throughput rather than host wall-clock/instructions, since host noise
currently swamps any real signal.

### 7. Wildcard hot-path audit — honest null result
Checked every specific lead below the top two profiled offenders (QOM
cast overhead in hot loops, redundant dirty-bitmap rechecking, per-tick
clock-tree recomputation, DMA/DMA2D I/O batching) against the actual
code, not function names. Found nothing new and fixable — everything
checked out as already appropriately optimized from earlier passes. No
manufactured "win." Did flag one useful lead for the still-open TCG
dispatch investigation: retro-go's RAM-resident core execution
potentially sharing pages with mutable data, worth checking if that
thread is ever picked back up.

## Two significant side-findings

### A. The HASH device model was silently uncommitted this entire session (fixed)
`hw/misc/gnw_h7b0_hash.c`/`.h` existed only as untracked working-tree
files. Commit `347ca514dc` ("Add real IWDG/LPUART1...") added the
`meson.build`/`Kconfig` references expecting them, but never the files
themselves or the SoC wiring. This meant **a fresh clone or worktree of
this branch had a broken build** (`meson: File gnw_h7b0_hash.c does not
exist`) the entire time, invisible only because every build this session
happened to reuse the one working tree that had the files. At least
three separate agents hit this independently before it was diagnosed and
fixed. **Fixed**: committed `hw/misc/gnw_h7b0_hash.c`/`.h` plus the
minimal SoC wiring (object_initialize_child/realize/mmio_map, replacing
the old unimplemented-device stub) as commit `7443ee6f96`, carefully
split out via a hand-built minimal patch (`git apply --cached`) from the
*other*, still-genuinely-pending soc.c/soc.h work (persistent
flash-image properties) that was entangled with it in the same dirty
files — that other work remains uncommitted, untouched, exactly as
before.

### B. A newly-exposed, genuinely pre-existing LTDC crash (UNRESOLVED)
`../stm32h7b0-diag` added real JPEG test cases today
(`case_jpeg_header_info.c`, `case_jpeg_decode_correct.c`,
`case_jpeg_encode_correct.c`, `case_jpeg_decode_throughput.c`). Running
the diag suite against builds with JPEG-related changes surfaced a real
QEMU host segfault in `gnw_h7b0_ltdc_capture_if_enabled()` (confirmed via
`journalctl -k` + `gdb -batch -ex "info symbol <addr>"` + `addr2line`: a
16-bit `lduw_he_p()` load from a NULL/invalid pointer in the per-pixel
RGB565-format read path). **Confirmed, by direct testing, to be
completely independent of every change this session made** — reproduces
identically on: the main checkout (with the SIMD patch applied), the
JPEG-encode worktree, and a completely clean/unmodified worktree freshly
rebased onto the current tip with zero functional changes. This is a
real, currently-live bug in the committed `gnw-h7b0` tip, just never
exposed before because nothing previously exercised whatever LTDC state
the new JPEG test cases put it into.

**Status: NOT FIXED.** A dedicated agent was dispatched to root-cause and
fix it, got partway through the investigation (was about to check
`gnw_h7b0_ltdc_fb_dirty_check_and_clear()` for the same class of issue),
and was killed by an unrelated host-process cleanup mid-task. Per the
project owner's explicit instruction, this was NOT restarted — the fix
remains outstanding. **Whoever picks this back up**: the crash appears
timing/state-dependent (doesn't reproduce on literally every single run
— a fresh run the same session hung on `jpeg_encode_correct` instead of
crashing, likely because the JPEG-encode hang prevented the firmware
from ever reaching whatever later state triggers the LTDC crash), so
budget for several repro attempts, and note that live `gdb -ex run`
debugging is impractically slow here (ptrace overhead pushed one attempt
past 9 minutes without reproducing what a plain run hits in ~10 seconds)
— prefer static disassembly/addr2line analysis or post-mortem core-dump
inspection over live ptrace-attached reproduction.

## Real JPEG encoder implementation (built, needs re-verification once the LTDC crash above is fixed)

Root cause of `jpeg_encode_correct`/`jpeg_decode_correct`/
`jpeg_header_info` all failing: `hw/misc/gnw_h7b0_jpeg.c` only ever
implemented JPEG *decode* (via vendored `stb_image.h`). `CONFR1.DE`
(the real hardware's decode-enable bit) was never checked anywhere, so
`HAL_JPEG_Encode()`'s raw pixel writes to `DIR` were fed straight into
`stbi_load_from_memory()` as if they were a JPEG bitstream — which fails,
and (because `EOCF` only ever gets set when the fed byte stream happens
to contain a `0xFFD9` EOI-marker pair at the right position) hangs
firmware's polling loop forever. `jpeg_decode_correct` fails as a direct
consequence: that test deliberately feeds `HAL_JPEG_Encode()`'s own
output back into `HAL_JPEG_Decode()` (independently-generated/Pillow
JPEG bitstreams were found not to be accepted by this real hardware's
decoder for unrelated reasons — see that test's own header comment) — so
garbage-in from a broken encoder means garbage/hang out of decode too.

Built: a real baseline JPEG encoder (8x8 DCT-II, IJG-scaled quant
tables, canonical Huffman tables, real JFIF markers) plus a real native
Y/Cb/Cr-direct baseline decoder (avoiding a lossy RGB round-trip the
existing `stb_image`-based path forces), confirmed against the real HAL
source (`sdk/stm32h7xx-hal-driver/Src/stm32h7xx_hal_jpeg.c`) that DIR
receives already-YCbCr pixel data directly (the codec IP does no color
conversion in software). Independently verified the encoder's own output
is genuinely valid JPEG via a from-scratch Python decode plus Pillow, not
just "our own decoder accepts it." Branch: `worktree-agent-acbc922798c640a55`.

**Not yet independently re-verified against the live diag suite** by the
coordinator, because doing so requires booting the diag firmware, which
is exactly what triggers the (separate, unrelated) LTDC crash above —
verification is blocked on that crash being fixed first, not on any
problem with the JPEG encoder work itself.

## Tool/workflow learnings this session

- **Every worktree-isolated agent must self-check `git log -1` against
  the expected current tip as its first step, and self-correct
  (`git reset --hard <branch>`) if wrong.** This bit at least five
  separate agents today (worktrees silently starting from a stale base,
  sometimes 10+ commits behind, sometimes an entirely unrelated upstream
  merge commit). Cheap to check, expensive to discover after a large
  diff has already been built against the wrong base.
- **Multiple agents running QEMU instances concurrently on one shared
  host clobber each other**: same X display fighting over `-display sdl`
  windows, same default gdbstub port (1234) colliding. Fix: `-display
  none` for all automated/verification work (reserve a visible display
  for actual human-in-the-loop checks only), and a private/unusual
  `-gdb tcp::<port>` per concurrent instance.
- **Agents (and the coordinator) repeatedly ended a turn to passively
  "wait for a notification" instead of blocking within one tool call**
  — this stalls real progress until someone notices and re-prompts.
  Fix: explicit instruction to chain a wait into the same blocking shell
  command (e.g. `sleep N && next_step`, or a real poll loop) rather than
  returning control and hoping to be re-invoked later.
- **Host-level `gdb -p <pid>` attach and `perf probe`/uprobes are both
  blocked without root** in this environment (`ptrace_scope=1`,
  `perf_event_paranoid=2`, no passwordless sudo). QEMU's own built-in
  `-trace enable=<event>,file=<path>` mechanism works without any
  special permission and should be reached for first for any
  call-rate/frequency question before assuming live debugging is
  necessary.
- **Live `gdb -ex run` reproduction of a crash that happens in ~10s
  unsupervised can take 5-10x longer (or fail to reproduce in a
  reasonable window at all) under ptrace's overhead.** For a
  fast/tight-timing-dependent crash, prefer: reproduce plainly first,
  then do post-mortem analysis (disassembly + `addr2line` at the known
  crash address, or a real core dump if the environment captures one)
  rather than attaching a live debugger from the start.
- **Independently re-verifying every dispatched agent's claims (fresh
  rebuild, fresh diag-suite run, not trusting the self-report) caught
  real problems this session**: a stale-base regression that silently
  deleted working code, an agent's own overclaiming on CSV status labels
  not matching its own written notes, and — most significantly — this
  session's own two largest findings (the uncommitted HASH device and
  the newly-exposed LTDC crash) were BOTH found *during* independent
  verification of unrelated agent work, not reported by any agent
  itself. Keep doing this; it is clearly worth the overhead.
- **Long-uncommitted "pending" working-tree state is a real, compounding
  risk**, not a neutral holding pattern — the HASH device sat
  uncommitted long enough that multiple independent agents lost hours
  collectively hitting the same "file does not exist" build break before
  anyone traced it to its root cause. Once something is functional and
  verified, commit it — don't let "not mine to commit" become
  indefinite.
