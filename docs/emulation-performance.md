# Emulation performance: where the time actually goes

Reference for anyone asking "why is gwemu slow here?". Written after a
full day of measuring the same workloads on Linux, macOS, Windows (VM
and real hardware), a Pi 4, and against the real Game & Watch over
SWD. Read the *measurement pitfalls* section before trusting any
number you take yourself -- most of that day was spent on readings that
turned out to be artefacts.

## The two regimes

gwemu workloads split cleanly, and conflating them wastes hours:

| regime | MMIO accesses/sec | what limits it |
|---|---|---|
| in-game (stock, Celeste, DOOM) | ~2,200 | almost nothing; guest is idle 90% of wall |
| retro-go launcher menu | **2,300,000** | MMIO exit cost, nothing else |

The launcher is not "slow because it renders covers". The firmware
drains the JPEG output FIFO a word at a time, checking the status
register between reads. That is authentic: verified on real hardware
(see below), where each access is a couple of CPU cycles. In QEMU each
access is tens to hundreds of nanoseconds, so the same loop costs
thousands of times more.

## Why an MMIO access is expensive

QEMU requires an MMIO access to be the **last instruction of its
translation block**, so an interrupt arriving after it lands precisely.
When it is not, `cpu_io_recompile()` (accel/tcg/translate-all.c):

1. `tcg_tb_lookup()` -- a glib tree search
2. `cpu_restore_state_from_tb()` -- unwind CPU state
3. `cpu_loop_exit_noexc()` -- **longjmp out of the block**
4. generate and execute a one-instruction block for the access

The decision is not remembered (`cflags_next_tb` is one-shot), so a
polling loop pays all of it on **every iteration**. Measured in the
launcher: 2.68M recompiles/sec, ~60% of all wall time. Skipping the
call outright (GNW_SKIP_IORECOMP=1, incorrect but diagnostic) took the
launcher from 53 to 129fps.

Caching which PCs do I/O and ending blocks there cuts recompiles 29x
(2.68M -> 93K/sec) but only gains ~5-10% fps: the resulting shorter
blocks lose TB chaining and cost back most of the saving. Do not expect
a large win from that direction.

## Platform cost differences

Same source, same guest, same workload. The gap is **not** CPU age --
a 7800X3D Windows VM and a modern arm64 Mac both show it, and a
Ryzen AI 7 350 laptop and a 7800X3D hypervisor do not.

| primitive | Linux | macOS |
|---|---|---|
| `pthread_mutex` lock+unlock | 2.3 ns | **18.7 ns** (8.1x) |
| `os_unfair_lock` lock+unlock | -- | 13.0 ns |
| `clock_gettime(MONOTONIC)` | 17.6 ns | 40.5 ns |

QEMU takes the BQL once per MMIO access, and the recompile path is
built from exactly the primitives that are expensive off Linux. macOS
profiles additionally show `_longjmp` at 4.5% (Darwin saves the signal
mask) and `_tlv_get_addr` at 2.0% (thread-locals go through a dyld
call, and `current_cpu` is read per access), plus ~33% of samples in
locking with 22% *blocked in the kernel* -- contention, not just
constant factor.

Windows has no per-region BQL opt-out to exploit:
`memory_region_clear_global_locking()` was removed upstream.

## The in-game deficit: millisecond poll rounding (solved 2026-07-26)

The above is the *launcher* (MMIO-bound) story. The separate in-game
deficit on macOS/Windows had a single, unrelated cause: **the main
loop cannot wait for less than a millisecond off Linux.**

`qemu_poll_ns()` (util/qemu-timer.c) uses nanosecond `ppoll()` only
under `#ifdef CONFIG_PPOLL`. Linux defines it; macOS and Windows do
not, and fall through to `g_poll()` with a `qemu_timeout_ns_to_ms()`
timeout that is deliberately rounded **up** ("better to wait too long
than to wait too little and effectively busy-wait"). Every timer
deadline closer than 1ms therefore overshoots. Check any build with
`grep -c "define CONFIG_PPOLL" <builddir>/config-host.h`.

The guest is ~92% idle in gameplay, so its frame rate is set by how
promptly the main loop wakes, not by throughput. Measured wakeups:
2095/s on Linux vs 1223/s on Windows for the same workload -- and the
guest then renders about two thirds of its frames. Emulated vblanks
still fired at the same rate on both (~1720 in 29s), which is why this
hid for so long: the display timer looks perfectly healthy.

Confirmed by A/B on one binary per host via `GNW_POLL_SPIN=1`
(diagnostic: spins instead of rounding), Celeste, 29 guest-seconds:

| host | rounded | sub-ms |
|---|---|---|
| Win11 VM (7800X3D) | 20.0 fps | **30.0** |
| Win11 laptop (i7-6820HK) | 24.7 fps, 53.7s wall | **30.0**, 29.1s |
| macOS (i5-10210U) | 27.1 fps | **30.0** |

The VM and its own Linux hypervisor share a physical CPU: 20.0 vs
30.0, hardware fully controlled for.

The **fix** is a real sub-millisecond wait, not the spin (which costs
3.8-4.4x the CPU of the fix for identical fps). Both live in
util/qemu-timer.c and compile out entirely on Linux;
`GNW_POLL_MS_ONLY=1` forces the old rounded behaviour back for A/B.

- **macOS** -- `qemu_pselect_ns()`. `pselect()`'s `struct timespec` is
  honoured at ns resolution. +7% CPU over baseline for +3 fps.
- **Windows** -- `qemu_timed_wait_ns()`. The macOS cure does not port:
  Winsock `select()` only understands sockets, but the "fds" this
  function is handed on win32 are Windows HANDLEs (`os_host_main_loop_wait()`
  stuffs `w->events[i]` into `poll_fds[].fd`, and glib's win32 `g_poll`
  waits on them via `MsgWaitForMultipleObjectsEx`). So the timeout is
  expressed *as a member of the wait set* instead: a cached per-thread
  `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` timer (100ns granularity)
  armed to the exact deadline, appended to the caller's fds, then
  `g_poll(..., -1)`. The timer firing *is* the timeout, so glib still
  does the real wait and only the rounding disappears. The timer is
  never reported as a ready fd. +34% CPU over baseline for the full
  20 -> 30 fps recovery, vs 4.4x for the spin.

Two traps if you re-measure this:

- **fps alone ranks hosts wrongly.** A host that cannot keep up fails
  in either of two ways: dropping frames while holding realtime (the
  VM: 20 fps but 29s wall), or letting the virtual clock lag (the
  laptop: 24.7 fps -- *better* -- but 53.7s wall for 29 guest-seconds,
  i.e. 55% of realtime). Always report wall time for a fixed-length
  timeline alongside GUESTFPS.
- **Celeste caps at 30 guest-fps**, so 30.0 is a ceiling, not a score.
  Linux hosts sit at it and cannot show improvement.

## Comparing against real hardware

The device is the arbiter for "is this firmware behaviour or a
modelling bug?". `gnwmanager` ships an OpenOCD backend that reads
device memory **without halting the CPU**:

```python
from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend
be = OpenOCDBackend(); be.open()
val = be.read_uint32(0x52003000 + 0x34)   # JPEG_SR
```

~3000 reads/sec through the probe, enough to sample a duty cycle.
Results that settled real questions:

- **JPEG_SR COF bit**: 22% duty on hardware while retro-go runs. The
  codec is not stalled; our model is not "too slow to decode".
- **MDMA**: `MDMA_GISR0 = 0`, no channel ever enabled, 0% duty over 200
  sweeps. Hardware drains the JPEG FIFO by CPU polling exactly as we
  emulate -- so the MMIO storm is authentic, not a missing DMA path.

`gnwmanager screenshot` needs the retro-go ELF to locate the
framebuffer symbol (`retro-go-temp/elf/gw_retro_go_bank2.elf`);
gnw-chainloader's `scripts/debug/` has better tooling (`fastcap.py`,
`measure_register_time.py`, `memory.py`).

## Measurement pitfalls

Every one of these produced a confident, wrong conclusion during the
investigation. They are listed in the order they cost the most time.

- **Instrumentation changes the answer.** The MMIO profiler does two
  `clock_gettime` calls per access; at 2.3M accesses/sec that cost
  Linux 42% of its frame rate (53 -> 30fps) and more on macOS, where
  the clock is 2.3x dearer. A "Mac is 4.6x slower" reading was mostly
  the instrument. Per-event `fprintf` is worse: printing one line per
  DMA half (60/sec) slowed the *Windows* guest threefold and produced a
  completely fictitious "83% of audio is injected silence" result.
  Summarise per second; gate per-event output behind `=2`.
- **Guest-relative metrics lie about wall-clock speed.** Firmware fps
  counters, LTDC vblank counts, and DMA tick rates are all derived from
  emulated time and read "correct" while the game visibly crawls. Only
  wall-clock-anchored counters (`GUESTFPS`, counted against
  `QEMU_CLOCK_REALTIME`) are trustworthy.
- **Verify what is on screen.** A timeline that reaches gameplay on one
  host lands in a menu on a slower one, because the timestamps are
  guest-time. Comparing a Mac in the launcher against Linux in gameplay
  produced a fake 5x "platform gap". Record a frame every run.
- **Occluded windows.** A covered window makes each present block ~1s
  (the compositor withholds swapchain images), so the whole render loop
  drops to 1 iteration/sec. Check `UI trace` present times before
  trusting anything measured with a window up.
- **Concurrent load.** A `ninja -j12` in the background invalidates any
  listening test and any fps measurement on the same box.
- **Disk.** The frame recorder writes 36MB/s at 30fps. Filling the disk
  mid-run distorts and then breaks the measurement.
- **Silent truncation.** The first profiler had 24 slots and dropped
  everything past them, reporting 15% of wall time with nothing to
  attribute it to -- hiding the single hottest region. Always count
  what you could not attribute.

## Diagnostic env vars

All off unless set; all treat unset/empty/`0` as off. Per-second
summaries unless noted.

| var | reports |
|---|---|
| `GNW_UI_FRAME_TRACE` | `GUESTFPS` (emulated frames per **wall** second), UI present/upload pacing, `VBLANK` loop rate and BQL wait |
| `GNW_MMIO_PROF` | per-MemoryRegion access counts, total/avg/max time, share of wall, and unattributed count |
| `GNW_MMIO_OFF` | per-register histogram for one device (currently the JPEG block) |
| `GNW_BQL_PROF` | BQL acquisitions/sec and wait time, split vcpu vs other threads |
| `GNW_IDLE_PROF` | share of wall the vCPU spends not executing guest code |
| `GNW_IORECOMP` | `cpu_io_recompile()` calls/sec |
| `GNW_SKIP_IORECOMP` | **diagnostic only** -- skips the recompile; loses interrupt precision around the access |
| `GNW_POLL_SPIN` | **diagnostic only** -- spins instead of rounding sub-ms waits up to 1ms; burns a core, ~3.8x the CPU of the real fix |
| `GNW_POLL_MS_ONLY` | forces the old rounded-millisecond `g_poll()` wait back, to A/B the sub-ms fix |
| `GNW_TIMER_LATE` | DMA stream tick rate and lateness (`=2` for per-tick lines) |
| `GNW_AUDIO_TRACE` | SAI FIFO/backend summaries (`=2` for per-DMA-half lines) |
| `GNW_JPEG_LAT` | JPEG decodes/sec and busy duty cycle |

## Known-good reference numbers

Linux (Ryzen AI 7 350), headless, no profiler attached:

| workload | GUESTFPS | vCPU idle | MMIO/sec |
|---|---|---|---|
| Celeste gameplay | 30.0 | 91% | 2,310 |
| retro-go launcher | 51-57 | 19% | 2,290,000 |
| DOOM | 59.7 | -- | -- |

Five-host Celeste gameplay comparison (2026-07-26), identical source,
identical 29-guest-second timeline, headless, screenshot-verified to
have reached the same in-game scene. `wall` is seconds for those 29
guest-seconds, so 29s == realtime:

| host | GUESTFPS | wall | notes |
|---|---|---|---|
| Linux laptop (Ryzen AI 7 350) | 30.0 | 29s | at Celeste's 30fps cap |
| Linux hypervisor (7800X3D) | 30.0 | 29s | at cap |
| Linux Pi 4 (aarch64) | 29.9 | 29s | at cap; ~60% of one core busy |
| macOS (i5-10210U) | 27.1 -> **30.0** | 29s | fixed by `pselect` |
| Win11 VM (on that 7800X3D) | 20.0 -> **30.0** | 29s | drops frames, holds realtime |
| Win11 laptop (i7-6820HK) | 24.7 -> **30.0** | 53.7 -> **29.1s** | also lags the clock |

Arrows are before/after the sub-ms poll fix (macOS `pselect`, Windows
high-resolution waitable timer). Both replicated. The three Linux hosts
are the negative control: they define `CONFIG_PPOLL`, so the whole
sub-ms path (and `GNW_POLL_SPIN`) is compiled out and provably cannot
move their numbers -- confirmed null on the Pi.

Older single-host figures, still useful for the launcher regime: Mac
20fps launcher; Windows (i7-6820HK) 19fps launcher with the GUI;
7800X3D hypervisor 59.7fps launcher.
