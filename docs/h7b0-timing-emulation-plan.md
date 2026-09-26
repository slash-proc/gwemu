# STM32H7B0 timing emulation plan

## Goal and success criterion

Make gwemu useful for performance questions such as choosing a data-cache
size or moving an m68k emulator's working set between RAM regions. The first
useful result is a **relative timing estimator**: given the same firmware,
workload, and hardware configuration, it should predict whether a change is
faster on the Game & Watch and explain the predicted difference in terms of
instruction work, cache misses, memory traffic, and contention.

The final, more expensive result is an opt-in execution mode in which guest
time advances according to modeled CPU and memory cycles. This would make
`DWT_CYCCNT`, timers, interrupts, and peripheral overlap observe the same
modeled timeline. Neither result should change gwemu's default fast mode.

Use the developer's reported ~1.59% placement regression as a demanding
validation case, **not** as a calibration target. For that class of change,
the measurement noise and model uncertainty must be substantially smaller
than the effect before gwemu's ranking can be trusted. Real hardware remains
the authority for effects below the model's demonstrated resolution.

This plan does not aim to reproduce every Cortex-M7 pipeline detail or every
peripheral's exact cycle timing before yielding value. Progress is gated by
measured ability to rank the real workloads, not by the number of hardware
blocks modeled.

## Current starting point

* The SoC maps ITCM, DTCM, AXI SRAM, AHB SRAM, internal flash, and external
  flash as QEMU RAM regions (`hw/arm/gnw_h7b0_soc.c`). Flash image persistence
  and OSPI command semantics exist, but ordinary CPU reads from the mapped
  external-flash window are served from host memory. The OSPI model explicitly
  makes command completion instant and does not model dummy-cycle or clock
  timing (`include/hw/misc/gnw_h7b0_ospi.h`). OTFDEC decrypts an entire
  configured region into a RAM overlay (`hw/misc/gnw_h7b0_otfdec.c`).
* `DWT_CYCCNT` is currently `QEMU_CLOCK_VIRTUAL` elapsed nanoseconds scaled
  at a fixed 280 MHz (`hw/misc/gnw_h7b0_dwt.c` and its header). It is not an
  executed-cycle counter and does not follow RCC overclock changes. The board
  already has a live SYSCLK `Clock`, updated by RCC and connected to the
  Cortex-M container (`hw/arm/gnw_h7b0.c`, `hw/arm/gnw_h7b0_soc.c`,
  `hw/misc/gnw_h7b0_rcc.c`).
* QEMU `-icount` counts guest instructions and maps them to virtual time with
  a shift (`accel/tcg/icount-common.c`, `accel/tcg/translator.c`). It is a
  useful determinism mechanism, but one instruction is not one hardware
  cycle. Existing headless documentation notes that a fixed shift can change
  firmware behavior (`docs/headless-capture.md`).
* `stm32h7b0-diag` already builds firmware that runs on the device and gwemu,
  times cases with real DWT hardware, exposes results in AXI SRAM, and uses
  gnwmanager's OpenOCD/PyOCD or QEMU GDB backend to read them. Its host harness
  supports repeated sweeps and a single-case mailbox. That is the right
  foundation for calibration and held-out validation.
* The existing diagnostic cases cannot all be used as timing ground truth
  without audit. `case_mem_memcpy_axisram.c` says its ordinary static buffers
  actually land in DTCM. More seriously, disassembly of the current
  `fw/build/diag.elf` shows the compiler removed the second 20,000-iteration
  loop from `case_mpu_icache_throughput.c`; the reported I-cache-enabled time
  measures essentially a reset and counter read. Its published ~0.2 us result
  must not be used to fit an I-cache model. The diagnostic firmware normally
  leaves D-cache off to keep SWD-visible memory coherent; that configuration
  may differ from the application being optimized.

## Measurement contract

Collect calibration data from **actual Game & Watch hardware**, using the
same silicon revision and external flash part as the target workload when
possible. Record the full configuration with every result: firmware and ELF
hashes, compiler flags, relevant code/data addresses, SYSCLK/HCLK, flash
latency, OSPI prescaler/opcode/dummy cycles, MPU attributes, I/D-cache state,
LTDC/DMA activity, and board/flash identity. Keep raw cycle samples alongside
summary statistics; do not replace raw data with one average.

The host should configure and trigger a case, then read its completed result.
No SWD transaction, logging, allocation, display update, or host polling
belongs inside the timed interval. Where changing cache state makes host
memory reads unsafe, place results in the existing non-cacheable shared area,
clean/invalidate any touched cacheable range, and restore the original state
on every exit path. Keep each 32-bit CYCCNT interval below its ~15.3-second
wrap period at 280 MHz, or add an explicitly verified extension scheme.

Before accepting a probe, inspect its disassembly and map file: verify loop
count, load/store widths, code and buffer placement, alignment, and that the
compiler has neither deleted nor transformed the work being measured. Add a
checksum or other observed result outside the timed interval. Build both
flash-resident and RAM-resident code variants with explicit linker sections.
Protect bank 2 and external flash from benchmark writes; read-only probes
should be sufficient for the first timing model.

### Probe matrix

1. **Clock and overhead:** CYCCNT read/reset cost, empty loop, straight-line
   ALU, dependent ALU, branch/call/return, exception entry/return, and
   WFI-to-interrupt. Test at the clock settings used by the real workload.
2. **Instruction supply:** the same verified instruction sequences from ITCM,
   internal flash, and external OSPI XIP; hot and cold I-cache; code footprints
   around cache capacity; sequential and branch-heavy paths; alignment and
   set-conflict patterns. Explicitly control and report I-cache invalidation.
3. **Data supply:** dependent loads to expose latency, independent streams to
   expose throughput, reads and writes from DTCM/AXI/AHB/internal flash/OSPI,
   size and stride sweeps around D-cache capacity and line size, alignment,
   cacheable versus non-cacheable MPU attributes, and clean/invalidate cost.
   Separate cold, warm, and capacity-miss results.
4. **External flash:** memory-mapped XIP separately from indirect OSPI reads.
   Vary sequential versus random access, burst size, boundaries, controller
   configuration, and OTFDEC state where relevant. The existing
   `case_ospi_xip_read.c` is a correctness starting point, not a sufficient
   latency characterization.
5. **Contention:** repeat selected CPU memory probes with LTDC scanout,
   DMA2D, and relevant DMA streams active. Compare against each isolated
   engine using identical memory addresses. Preserve the diagnostic tool's
   off-screen-buffer discipline. The existing synthetic CPU/DMA2D case is a
   useful template, but its combined duration alone cannot identify which
   bus path caused a slowdown.
6. **Application oracle:** reproduce the m68k emulator's exact hot workload,
   cache layouts, linker placement, and clock/cache/MPU settings on hardware.
   Record frame/work-unit boundaries and per-phase DWT cycles. A small
   synthetic probe cannot establish that the model ranks the full emulator.

For effects around 1%, run paired A/B measurements in interleaved or
randomized order across multiple fresh boots. Report median paired delta,
spread, and a confidence interval, and inspect drift. Keep workload input
deterministic. Separate cold-start results from steady state. A/B binaries
must be checked for unrelated code-layout changes; moving data can also
change addresses, alignment, or compiled code. Require a confidence interval
that excludes zero and is narrow enough to resolve the proposed model error
budget before treating a 1.59% difference as a stable hardware fact.

The existing `--reps` option resets between full sweeps; add or use mailbox
driven repeated single-case runs for paired placement experiments. The
`DWT/280 MHz` values in the current `--compare` harness express elapsed time
on each target; QEMU's current values are host-dependent execution time,
not silicon cycle costs. Do not fit model parameters to those QEMU values.

## Stage 1: offline access and cycle estimator

Build this before changing guest time. Start with QEMU's opt-in
`contrib/plugins/cache.c` as a reference for instruction and memory callbacks.
Capture, for a selected guest PC/workload window, executed instruction
address and width, data address/size/read-write, and physical memory region.
Infer instruction fetches from the executed guest instruction stream; TCG's
translation-block cache is not the emulated M7 I-cache. Capture cache-control
and MPU changes that affect timing, or require a frozen configuration for the
first prototype. Keep traces bounded or aggregate online to avoid enormous
logs and host-memory pressure. Verify that tracing itself does not change
the guest's address or branch sequence.

Replay the access stream through a parameterized M7 timing estimator:
instruction baseline plus branch/call effects, I/D-cache tags and line fills,
region-specific load/store costs, OSPI transaction startup and streaming,
and explicit overlap rather than blindly summing independent engine times.
Use documented geometry where reliable and hardware measurements to fit
remaining parameters. Expose a breakdown by source, memory region, cache
hit/miss, and modeled cycles. Keep calibration and validation datasets
separate, versioned, and reproducible.

This stage can answer the central question at lower implementation risk:
does the measured memory-access sequence explain cache-size and placement
rankings? It also exposes identifiability limits. If several different
models fit the same probes, add discriminating probes rather than claiming
precise cycle counts.

**Gate:** on held-out probes and at least two real m68k layout variants, the
estimator must rank changes larger than its measured uncertainty and give a
credible cost breakdown. If it fails, improve the probes or model before
modifying QEMU's virtual clock.

## Stage 2: opt-in guest timing mode

Only after Stage 1's model is useful, integrate cycle accounting into gwemu's
ARM/TCG execution path for the `gnw-h7b0` machine. Do not attempt to add
delays by turning flash RAM into MMIO: that changes QEMU's execution path,
can make every read expensive on the host, and still misses instruction
fetches and cache behavior. Do not merely replace the DWT formula: guest
timers, DMA completion, audio, display, and interrupts would then disagree
with the counter.

The design needs one authoritative monotonically increasing guest timeline:

* Charge base execution cycles and dynamic fetch/data penalties at points
  where the executed instruction and memory address are known. Translation
  blocks may remain an optimization, but partial-block exits, exceptions,
  MMIO, faults, and self-modifying code must account only for instructions
  that actually retired. Feed RCC CPU-clock changes into cycle-to-time
  conversion without jumping or losing elapsed time.
* Model I/D-cache enable, invalidate, clean, and MPU cacheability without
  changing architectural memory contents. Account for internal-flash wait
  states, OSPI configuration, XIP burst behavior, and measured RAM paths.
  Introduce DMA/LTDC bus overlap after single-master behavior is validated.
* Drive `DWT_CYCCNT` from this accumulated cycle count. Make
  `QEMU_CLOCK_VIRTUAL` advance consistently so SysTick, TIM, RTC-related
  timers, DMA completions, NVIC delivery, and firmware timeouts retain their
  causal order. WFI/idle must allow pending peripheral events to advance
  virtual time without inventing retired CPU instructions.
* Define mode interaction with `-icount`, replay, migration/snapshots,
  debugging halts, and guest resets. The current fixed-shift icount is not
  itself a variable-cycle timing engine. Reject incompatible combinations
  clearly until they are supported, rather than silently mixing clocks.
* Keep normal gwemu execution unchanged unless the user opts in. Report
  modeled guest cycles/time separately from host wall time and host CPU use.

Implement in increments: base CPU cycles and DWT; then region penalties;
then cache state; then OSPI; then contention. Each increment must include
tests for counter monotonicity, timer ordering, exception/partial-block
accounting, clock changes, and mode isolation. A full-system timing run must
still boot and play the firmware; correct microbench numbers with broken
frame pacing are not a successful mode.

## Validation and release gates

Maintain three independent views for every reference workload: real-hardware
DWT cycles, offline estimator cycles, and integrated gwemu modeled cycles.
Compare **paired deltas** as well as absolute totals. Keep a held-out set of
working-set sizes, alignments, placements, and concurrent-device conditions
that are not used to fit parameters. Re-run the same firmware image on at
least two host systems; modeled guest cycles should not change with host
speed, while elapsed host time may. Record emulator build, model version,
firmware hash, and hardware configuration with each result.

Acceptance gates, to be refined after measuring hardware noise:

* Functionality and existing headless timelines remain correct in fast mode.
* Timing mode produces repeatable cycle counts and preserves guest event
  order across host machines.
* For each claimed scenario, held-out absolute timing error and paired-delta
  error are reported, not hidden in one aggregate score.
* For the m68k placement question, the measured hardware delta is stable,
  the model predicts its sign, and the model's demonstrated error interval is
  narrower than the change. Otherwise document that the model is not yet
  precise enough for that decision.
* Host overhead is measured. If full timing mode is too slow for interactive
  use, retain it as a focused profiling/headless mode; fast mode remains the
  regular emulator.

## Work items and deliverables

| Order | Deliverable | Completion evidence |
| --- | --- | --- |
| 1 | Audit and repair diagnostic timing cases; add placement and cache probes | Disassembly/map checks, hardware raw samples, documented configuration |
| 2 | Reproducible hardware dataset and paired m68k A/B workload | Raw results, variance and confidence intervals, exact binaries/addresses |
| 3 | Bounded gwemu instruction/data access trace | Trace matches known probes and preserves their functional result |
| 4 | Offline parameterized estimator and cost report | Held-out probes and m68k layout rankings evaluated honestly |
| 5 | Opt-in cycle-driven DWT/virtual-time prototype | Counter, timer, interrupt, reset, and clock-change tests pass |
| 6 | Incremental cache, flash, and contention timing | Hardware comparisons improve without fast-mode regressions |
| 7 | User-facing timing report and limitations | Reproducible commands, model/version metadata, measured error bounds |

## Main risks and decisions to settle with evidence

* **Calibration mismatch:** the diagnostic firmware's default cache/MPU setup
  is not automatically the application's setup. Capture application settings
  and run matched probes before extrapolating.
* **1% effects:** debugger interference, temperature/voltage, asynchronous
  display or DMA, cache warmup, compiler transformations, and changed code
  layout can swamp a small placement delta. Paired repetitions and binary
  inspection are required.
* **Model ambiguity:** cache misses, flash latency, and bus contention can
  produce similar totals. Use dependent loads, controlled footprints, and
  isolated/concurrent pairs to distinguish them.
* **QEMU architecture:** a plug-in can observe accesses and estimate costs,
  but cannot by itself turn those costs into a coherent guest clock. Dynamic
  time accounting will touch TCG, CPU clocks, and event scheduling, and needs
  a separately reviewed implementation design after the offline gate.
* **Hardware and protocol scope:** do not assume a particular cache geometry,
  OSPI part, or bus arbitration formula from a different STM32H7 variant.
  Confirm board-specific facts from the actual device and its configuration.
  The diagnostic protocol is append-only and manually mirrored in Python;
  any added result fields require a version bump and synchronized parser.

No hardware was run and no timing parameters were fitted while writing this
plan. The first implementation branch should begin with the diagnostic
audit, not with guessed wait-state constants in gwemu.
