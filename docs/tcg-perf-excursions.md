# TCG performance excursions

Index of TCG/dispatch performance experiments run against this fork: what was
tried, what it measured, and whether it was kept. Refuted experiments are listed
too — the point of this file is that nobody re-runs a dead end, so a negative
result earns an entry exactly like a win does.

Each excursion keeps a `perf/*` branch even when rejected.

## How these were measured

Harness: `backup/dos-perf/` (gitignored; local working state, see its STATE.md).
It replays a recorded timeline under the virtual clock, so both arms of an A/B
do identical guest work.

**The metric is host CPU-seconds (`HOSTCPU`) over a fixed timeline, not `ips`.**
The guest's `ips` is capped by its own speed profile (20,000 instructions per
frame), so on any scene with idle headroom it reads the same number no matter
how fast the host is — it cannot see a host-side optimisation at all. Several
early A/Bs looked like "no effect" purely because of this. Cross-check with the
guest's DWT-derived `cpu%`/`idle%`/`cpi`, which is independent.

Noise floor is ~±2%; take a median of 3 and **interleave the arms** (A,B,A,B)
rather than batching them, so host drift cannot manufacture a result.

## Kept

### `perf/tb-jmp-cache` — TB jump cache sized for 1KB pages (ON by default)

`TB_JMP_CACHE_BITS` 12 → 14. **−24% host CPU** on the 8086tiny DOS core
(8.935s → 6.795s median, interleaved 4-round A/B).

QEMU's default of 12 assumes 4KB pages. ARM M-profile forces
`TARGET_PAGE_BITS = 10` (1KB) so sub-4K MPU regions can be modelled, and the
softmmu TB hash splits its bits evenly between page number and intra-page
offset — so a 12-bit cache indexes on only 6 page bits, and a working set over
64 pages aliases hard in a direct-mapped cache.

14 is the knee, measured: 15/16/18 are statistically indistinguishable from 14,
so the whole effect is the single step 12 → 14 and larger caches only cost
footprint. Costs 256KB/vCPU.

### `perf/goto-tb-crosspage` — cross-page `goto_tb` chaining (OPT-IN, off by default)

`GNW_GOTO_TB_CROSSPAGE=1`. **−27% host CPU** on the DOS core (7.24s → 5.28s);
guest DWT agrees (cpu 22→14%/frame, idle 75→85%, cpi 52→33, blit 342→87µs).

Cause, measured by histogramming the guest PC of all 412M TB lookups in a 30s
run: **~85% are plain direct branches whose target is in a different 1KB page** —
not indirect dispatch (4.9%) or function returns (0.65%).
`translator_use_goto_tb()` only chains within one page, so any guest function
larger than 1KB cannot chain internally. `dos_cpu_frame` is ~13KB. The
`dos_video_blit` inner loop is 34 bytes straddling a page boundary and alone
accounted for 11.6% of every TB lookup in the machine.

Why the same-page rule exists (upstream `d3a2a1d803`, "mmap and mprotect can
change page permissions"): reaching a TB implies `tb_lookup()` validated that
page's vaddr→paddr mapping and execute permission, and a same-page destination
inherits that. Nothing revalidates an already-patched jump — `tlb_flush()`
clears the TLB and the jump cache but never calls `tb_reset_jump()`.

The knob restores that invariant inversely: cross-page links are recorded and
every TLB flush tears them all down. Same-page links are deliberately *not*
recorded, so a flush-heavy guest keeps normal chaining. Code modification was
already covered independently by `tb_phys_invalidate()` → `tb_jmp_unlink()`.

**Why it stays opt-in.** The teardown is confirmed live (1970 links noted, 79
flush events, 1444 torn down per run), and every M-profile MPU write that
changes permissions does reach `tlb_flush`. But a negative control settled the
question: **with the teardown deliberately neutered, the workload still produced
bit-for-bit correct output.** The guest's runtime MPU changes only alter
cacheability and always keep `INSTRUCTION_ACCESS_ENABLE`, so it never revokes
execute permission on a page it is executing from — the one case the teardown
exists for. The hazard is therefore *untested*, not cleared, and passing runs
are not evidence of safety. Enabling by default needs a guest that makes an
executing page non-executable.

Residual risks: links are compared on *physical* pages (fine where VA==PA, would
under-record on an MMU target with aliasing); `tb_reset_jump()` does not clear
`jmp_dest`, so a torn-down link never re-links and the win decays on a guest
that flushes the TLB constantly.

## Refuted

Do not re-run these without new evidence.

| Experiment | Result |
|---|---|
| Jump cache beyond 14 bits (15/16/18) | Within noise of 14; pure footprint cost |
| Devirtualise `get_tb_cpu_state` (drop the `tcg_ops` indirect call) | +0.9%, within noise. Verified applied in the disassembly. Removing an indirect call and 3 dependent loads at 13.9M calls/s bought nothing — they were already cache-hot and well predicted |
| Disable `CF_PCREL` | Costs essentially nothing on the jump-cache fast path (which compares the full PC regardless), and `restore_state` depends on it |
| `--disable-qom-cast-debug` | 9.16s vs 8.73s baseline — no win |
| Cut guest-side call depth in the interpreter | Would remove <1%: returns are 0.65% of lookups. An earlier framing of this as the main cost was wrong |
| Region-wide `DIRTY_MEMORY_VGA` logging as the bottleneck | Does not reproduce on current firmware in either display mode; the dirty/SMC symbols are 0% of samples. `GNW_LTDC_NO_DIRTY_LOG=1` remains as an A/B knob and changes nothing at this scene |

## Scope of these wins — measured, not assumed

Both wins are in generic TCG code. Whether a given guest benefits is predictable
from one number: `helper_lookup_tb_ptr`'s share of its profile.

| Guest | `helper_lookup_tb_ptr` | Shape | Cross-page win |
|---|---|---|---|
| 8086tiny DOS core | 50% (13.9M lookups/s) | interpreter | **−27%** |
| retro-go tgbdual | ~47% (~8M/s, see `accel/tcg/cpu-exec.c:375`) | interpreter | expected, unmeasured |
| retro-go launcher | 2.5% | render-bound (LTDC blend 37%, compositor 16%) | −3.0%, inside noise |

**The launcher is the outlier, not the rule.** It is a menu; every actual
retro-go core is an emulator, i.e. an interpreter dispatching through a jump
table, which is exactly the dispatch-bound shape that benefits. The fork already
records ~47% dispatch on tgbdual, measured independently of this work and before
it.

So the claim "helps every guest" is wrong only in its edges: these help
**interpreter/emulator cores — the workloads this machine exists to run** — and
are unmeasurable on render-bound UI. Do not cite the launcher's −3% as evidence
against the change; cite it as evidence that render-bound scenes need a
different lever (the LTDC blend path, which is 53% there).

Still unmeasured: an actual emulator core end to end. tgbdual's 47% is a strong
prior but the cross-page A/B has not been run on it — that needs a recorded
timeline that launches a GB core.

Note also that on a scene with idle headroom these buy *headroom, not frames*:
the guest requests a fixed instruction budget per frame and already gets it, so
`ips` and fps are unchanged and the gain shows as increased idle. It converts to
real speed only where the guest saturates the host.

## Open

- Measure under saturation (a heavier scene or faster speed profile), where
  headroom no longer hides host-side cost. Needs a recorded timeline.
- `TB_JMP_CACHE_BITS` breadth: untested outside the DOS core, and it targets the
  same lookup path that is only 2.5% of the launcher profile.
- A `goto_ptr` inline cache would attack the remaining indirect-dispatch cost.
  Nothing like it exists upstream or in-fork; it needs a versioned pointer and a
  full key (flags can change without PC changing), and is the riskiest idea on
  this list.
