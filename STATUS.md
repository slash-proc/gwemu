# Status

Last updated: 2026-07-13

## Where things stand

Repo is a fork of upstream QEMU (`qemu/qemu`), pinned to tag `v11.0.2`.
Working branch `gnw-h7b0`. Phase 0/1 (boot path, memory map) and Phase 2/3
(DMA2D, LTDC display) are done. retro-go (homebrew) firmware boots
end-to-end through the SD-backed build, with working audio, gamepad input,
JPEG-decoded cover art, and menu/gameplay rendering. Mario and Zelda's
gnwmanager CFW images both boot to a fully interactive clock face, with
working GAME/PAUSE menus and game launching (see below). Not push-worthy
yet — local commits only, push only on explicit go-ahead.

**gnw-web-builder integration (browser/gnwmanager driving this QEMU over
GDB RSP) — several real bugs fixed, RESOLVED.** Full writeup:
`docs/session-2026-07-13-web-builder-integration-fixes.md`. Summary: (1)
a real HASH peripheral device model replaced an unimplemented stub that
permanently hung every internal-flash write (`HAL_HASHEx_SHA256_Start`
polls with no timeout); (2) `FLASH_R`'s sector-erase registers were a
pure stub with no connection to actual flash memory — erase finished
instantly and silently did nothing, now fixed; (3) QEMU's gdbstub no
longer halts the VM for memory reads/writes (only for commands that
genuinely need it), matching real SWD debug access and eliminating a
real CPU-starvation bug; (4) flash bank/extflash images are now
optionally (and by `scripts/boot_qemu.sh`'s default) backed live by their
`.bin` files via `memory_region_init_ram_from_file`, so flashing/erasing
through the emulator persists like it would on real hardware
(`--ephemeral` opts back out). New tool: `scripts/gdb_tap.py`, a logging
GDB-RSP proxy with Nagle/delayed-ACK fixes and single-client preemption.

**Mario/Zelda CFW GAME/PAUSE menu invisibility — RESOLVED, user-confirmed
live.** Root cause was two real bugs in QEMU's LTDC compositor
(`hw/display/gnw_h7b0_ltdc.c`), not a firmware issue: (1) pixel format
AL44 (used for anti-aliased overlay text) was completely unimplemented,
silently skipping Layer2 compositing entirely; (2) Layer1/Layer2
compositing order was backwards (real STM32 LTDC always shows Layer2
above Layer1). Both fixed; GAME→A now launches a game with the menu
visibly rendering throughout. Full investigation (an extensive, multi-part
live-tracing session covering button input, a firmware state machine, and
the render dispatch chain, all eventually exonerated) is in
`docs/session-2026-07-12-breakpoint-lockstep-tracing.md` parts 15-19.

**Mario CFW running at ~half real-hardware speed — partially fixed, one
residual gap open.** User confirmed via direct real-hardware comparison
(not just expectation). Root-caused and fixed a real bug: `RCC_CR`'s
write handler mirrored every oscillator's `*ON` bit into its `*RDY` bit
except CSI, which firmware requests as part of its PLL1 setup sequence —
added the missing `CSION`→`CSIRDY` mirror in `hw/misc/gnw_h7b0_rcc.c`.
This fixed PLL1 actually locking (confirmed via `gnwmanager`'s
`OpenOCDBackend`: every clock register — `PLLCKSELR`, `PLL1DIVR`,
`CDCFGR1`, `CR`, `CFGR` — now matches real hardware bit-for-bit, where
before QEMU was permanently stuck on HSI while real hardware ran on
PLL1). **Still open**: even with every register matching real hardware
exactly, a precisely-measured firmware tick counter still runs at ~half
real hardware's rate — meaning there's one more bug, purely inside QEMU's
own internal clock-propagation/timing code (not firmware- or
register-visible). Next-session starting point (already narrowed to
`gnw_h7b0_rcc_update_sysclk_clock()`/`armv7m_systick.c` timing) is in
`docs/session-2026-07-12-breakpoint-lockstep-tracing.md` part 20.

**Retro-go pause-overlay flicker + main-menu black-screen — RESOLVED.
General gameplay stutter — root cause found (SysTick tick-loss under
TCG load); real fix is `-icount`, scoped as a separate follow-up.**
Full writeup: `docs/session-2026-07-13-part5-retro-go-ltdc-vbr-and-stutter-investigation.md`.
The no-VBR auto-capture fallback in `gnw_h7b0_ltdc_vblank_tick()` (added
`1663797f50`, flagged by its own commit message as a flicker-regression
risk) needed to distinguish "firmware that never uses VBR" (gnwmanager,
retro-go's own main menu) from "firmware using VBR that just landed
between two writes" — conflating them caused, in turn: (1) mid-draw
tearing during retro-go's pause overlay (fixed via a `vbr_ever_used`
latch, committed `014612c688`); (2) that latch then permanently blacked
out the main menu after playing any game, since the menu itself never
writes `SRCR` again to re-arm it — fixed by replacing the latch with
`vbr_active`, reset on `SRCR.IMR` instead of on a timeout (an
idle-timeout version was tried first and reverted, since real in-game
firmware can leave multi-hundred-ms gaps between VBR writes on its own).
Separately, a *second*, unrelated black-screen cause was found and
resolved by the user: this project's flash images are persistent by
default, and repeated hard `kill`s of QEMU mid-session while games were
running had genuinely corrupted the backing files — fresh copies fixed
it immediately, no code involved.

The general gameplay stutter the user flagged as the real priority
turned out to have a shared root cause across completely unrelated
emulator cores (confirmed via PC-sampling both SMW's SNES engine and
Link's Awakening DX's gnuboy GBC core mid-stutter — both showed their
hottest code in their own audio-rendering function, `smw__dsp_cycle` and
`apu_snd::render` respectively). A dynamic DMAMUX-based DMA/SAI1 rebind
mechanism (added `77e86eacdd` for stock-Zelda/OFW compatibility) was a
strong suspect but was ruled out live (traced, only fired 3 times, all
during boot, none during the freezes). The actual mechanism: during a
freeze, live register reads showed the system genuinely parked on bare
HSI (both CSI and PLL1 disabled) for 5+ real seconds after a STOP2
sleep/wake cycle (retro-go's idle "attract loop" power-saving path,
`game-and-watch-retro-go-sd`'s `Core/Src/gw_sleep.c`), with `uwTick`
still incrementing but at ~228/sec instead of ~1000/sec. Root cause:
SysTick's pending-IRQ state is a single bit, not a counter — under
QEMU/TCG, `SysTick_Handler` takes far longer in real wall-clock time to
execute than on real hardware, so a tick that fires while the CPU is
still busy with the *previous* one is silently lost, no matter how
correctly `hw/core/ptimer.c` schedules its own deadlines. This is
consistent with (and refines) the already-known "~2x tick rate gap"
below, and also applies to SMW's own catchup-burst finding (real guest
firmware behavior, imperceptible on real hardware, made visible here by
the same TCG-speed mismatch). **Decision, agreed with the user**: this
needs `-icount` (deterministic instruction-scaled virtual time) to
actually fix, not another device-model patch — tracked as a separate,
larger follow-up effort, since it's a global timing-model change that
needs re-validating `gnw_h7b0_dma.c`'s accumulator-scheduled timers,
LTDC's vblank period, and SAI1 audio pacing against it.

**Actively in progress:** getting *stock* (official Nintendo, unpatched)
Game & Watch firmware booting to visible display output — separate from
the CFW work above, not yet reached. Process: boot both QEMU and real
hardware to the real reset entry point, walk forward checkpoint-by-
checkpoint via real breakpoints cross-referenced against Ghidra
decompilation, compare core registers, narrow toward the first real
divergence. Currently parked mid-trace at an OCTOSPI1 RSTEN-command
self-trap firmware takes on QEMU that it doesn't take on real hardware —
see `docs/session-2026-07-12-breakpoint-lockstep-tracing.md` (parts 6-10)
and `docs/session-2026-07-12-stock-firmware-boot-investigation.md` /
`-register-snapshot-diffing.md` for the full trail. Several real fixes
already landed along the way (`RCC_RSR.SFTRSTF`, GPIO reset defaults,
`GNW_H7B0_ADC_FULL_BATTERY_RAW`, OCTOSPI1/2 IRQ line, OSPI auto-polling,
a minimal OTFDEC model, CRYP AES-GCM).

## What works

- Core CPU, memory map, NVIC, real clock tree (HSI/HSE/PLL1-3, live-derived
  SYSCLK/HCLK/LTDC-pixel-clock — see `docs/h7b0-clock-tree-findings.md`),
  dynamic CPU-overclock support (retro-go's NORMAL/BOOST1/BOOST2 modes,
  and now Mario CFW's own PLL1 overclock sequence).
- SPI/OSPI flash, SD card (SPI-based), RTC, DMA (accumulator-scheduled, no
  drift), TIM2, ADC, PWR, CRC, CRYP (AES-GCM), OTFDEC (KEYCRC), and the
  rest of the boot-path peripheral set.
- LTDC: real per-layer compositing with the correct Layer1-below/Layer2-
  above order, real AL44 + L8 + RGB565 + ARGB8888/1555/4444 pixel formats,
  color key, window-clip/default-color, generalized constant-alpha/
  pixel-alpha blend formula, Bayer dithering, independent Layer1/Layer2
  CLUTs, correct RRIF/reload-timing semantics, vblank capture gated on
  firmware's actual `SRCR.VBR` write instead of a fixed timer.
- DMA2D: real per-pixel fetch for every input format retro-go uses
  (ARGB8888/RGB565/ARGB1555/ARGB4444/L8/YCbCr/A8), real `BLEND_BG`/`BLEND_FG`
  "fixed color" semantics, 3-bit `MODE` field.
- JPEG: real polled `DOR` output register, correct chroma subsampling,
  `OFTF`-modeled output FIFO threshold for the real bulk-read polling path.
- Audio: confirmed solid across every core tested (NES, Genesis, SMW,
  others).

## Known issues (open)

- **Mario/Zelda CFW clock speed**: PLL1 now locks correctly (see above), and
  a real, substantial host-CPU-throughput bottleneck was found and
  partially fixed this session — LTDC Layer2 and DMA2D were both doing a
  full `cpu_physical_memory_read()`/`_write()` guest-memory-translation
  call *per pixel* instead of batching a row at a time (JPEG cover-art
  YCbCr decode did 3 such calls per pixel). Batching these (now landed in
  `hw/display/gnw_h7b0_ltdc.c`/`gnw_h7b0_dma2d.c`) took Zelda CFW's
  measured SysTick rate from ~415Hz to ~643Hz (vs. real hardware's
  1000Hz) — `perf record` confirms the address-translation overhead this
  removed is gone from the hot path entirely. Still short of real-time:
  the remaining gap is now genuine QEMU TCG instruction-interpretation
  cost (confirmed not a clock-frequency/formula bug — see doc parts
  20-23), not further-fixable at the device-model level without deeper
  QEMU TCG-level tuning. GPU-offloading DMA2D's pixel math was considered
  and rejected: the G&W's screen is small enough that dispatch/readback
  overhead would likely cost more than the work itself, and it wasn't
  where the remaining time goes anyway (TCG interpretation dominates).
  See doc parts 20-23 for the full investigation.
- **Coverflow/menu flicker**: selected item's text and the leftmost
  background cover flicker, worse with a menu open over the carousel.
  Extensively investigated in `docs/session-2026-07-11-ltdc-flicker-investigation.md`
  and `-dma2d-jpeg-ycbcr-pipeline.md`. LTDC/DMA2D/JPEG pixel-fidelity gaps
  ruled out (all fixed, didn't help); a vblank-capture timing change
  landed the same investigation but was never confirmed live to actually
  fix the flicker — needs retest. The "known gap" flagged here (content
  that never writes `SRCR.VBR` not getting captured) did turn out to be
  a real regression, just via a different path than expected (a fix for
  a *different* flicker regression accidentally latched the fallback off
  permanently) — see the 2026-07-13 part 5 doc above for the full
  `vbr_active` fix.
- Real subsampled chroma storage was traded for full-resolution internal
  storage in the JPEG model (a documented scope decision, not a bug).
- Real JPEG-decode performance is inherently register-access-heavy;
  coverflow screens (5 covers redecoded every frame) are still the most
  expensive on-screen content by a wide margin.
- Stock (unpatched) firmware boot is still blocked on an OCTOSPI1 RSTEN
  self-trap divergence from real hardware — see "Actively in progress"
  above.

## Tooling notes worth keeping in mind

- `gnwmanager`'s `OpenOCDBackend`/`GDBBackend` let you read real hardware
  registers directly for ground truth — use this early when a QEMU vs.
  real-hardware behavioral question comes up, not as a last resort.
- QEMU's gdbstub still halts the whole VM (including peripheral
  input-event delivery, not just the vCPU) on client *connect*, and for
  any command besides memory read/write (registers, continue/step,
  breakpoints, etc. — see 2026-07-13's gdbstub fix in CHANGELOG.md).
  Reading peripheral state right after connecting can show stale
  pre-input values; verify via `query-status` that the VM is genuinely
  running, and prefer letting it run freely between short, deliberate
  halts over long watchpoint loops that keep it halted most of the time.
- `scripts/gdb_tap.py` (a logging GDB-RSP proxy) is the fastest way to see
  what a real client (browser, `gnwmanager --qemu`) is actually sending,
  instead of guessing from JS/Python source reading alone — read its log
  before hypothesizing about a "client can't talk to QEMU" report. For
  reproducing a suspected device-model bug in isolation, prefer launching
  a second QEMU instance on a scratch copy of the images with `-gdb
  tcp::<port> -S` and driving the RAM stub directly over raw GDB RSP —
  much faster than debugging through the full browser/backend/tap chain,
  and doesn't risk corrupting the user's live session (QEMU's gdbstub
  only serves one client; never connect a second one to an in-use
  instance "just to take a quick look").
- A full (non-`-noanalysis`) Ghidra auto-analysis pass takes only ~5s on
  this binary and is needed for reliable RAM-address xref tracing —
  `-noanalysis` only finds reads of flash literal-pool pointers, not
  writes to the RAM addresses they resolve to.
- Temporary `fprintf(stderr, ...)` debug prints added directly to device
  model source (rebuilt via `ninja qemu-system-arm`) are the fastest way
  to see internal QEMU device state that guest-side gdb reads can't reach
  at all — always remove them again once their diagnostic purpose is served.
