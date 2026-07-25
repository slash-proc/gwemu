# Status

Last updated: 2026-07-25

Fork of upstream QEMU (`qemu/qemu`), pinned to tag `v11.0.2`. Working
branch `gnw-h7b0`.

## Where things stand

Real STM32H7B0 machine model, not a fault-trap workaround. Both homebrew
(retro-go, SD-backed) and official Nintendo stock/CFW firmware (Mario,
Zelda) boot end-to-end to a fully interactive, playable state: display,
audio, gamepad input, SD card, save/flash persistence.

## What works

- Core CPU, memory map, NVIC, real clock tree (HSI/HSE/PLL1-3, live-derived
  SYSCLK/HCLK/LTDC-pixel-clock — see `docs/h7b0-clock-tree-findings.md`),
  dynamic CPU-overclock support.
- SPI/OSPI flash (dual-bank, see `docs/h7b0-flash-discrepancy.md`), SD card
  (SPI-based), RTC, DMA, TIM1/TIM2/LPTIM1, ADC, PWR, CRC, CRYP
  (AES-ECB/CBC/CTR/GCM/CCM), OTFDEC (real AES-128-CTR extflash decryption),
  HASH, FLASH_R, TAMP, and the rest of the boot-path peripheral set.
- LTDC: real per-layer compositing (correct layer order, pixel formats,
  color key, window-clip, blend, dithering, per-layer CLUTs, real
  reload/vblank timing).
- DMA2D: real per-pixel fetch for every format firmware uses, real
  blend/fixed-color modes.
- JPEG: real polled output-register pipeline with correct chroma
  subsampling.
- Audio: confirmed solid across every core tested. Backend is the fork's
  own `sdl3` audiodev (default on every host; WASAPI/CoreAudio/PipeWire).
- Cross-platform: one rendering+audio stack (SDL_Renderer + sdl3
  audiodev, no OpenGL requirement) verified live on Linux (Vulkan),
  Windows (D3D11/software; VM-tested incl. sound) and macOS Intel
  (Metal). Local Windows cross-build via Docker and Mac-over-SSH
  workflows in `docs/cross-platform-builds.md`; `start.bat` is the
  Windows launch path.
- Headless capture appliance for CI/test suites: truly windowless
  `-display none`, virtual-clock timeline scripts (repeatable to the
  byte), whole-session A/V recording, Docker packaging — see
  `docs/headless-capture.md` and `contrib/docker-headless/`.

## Known issues (open)

- Native Wayland disabled by default on Linux (x11/XWayland instead) --
  three real breakages documented in `ui/gwemu.c`; revisit when SDL3's
  Wayland fractional-scale handling stabilizes.
- QEMU/TCG instruction-interpretation overhead means gameplay is not
  perfectly real-time-matched to real hardware in every scenario; most of
  the addressable overhead (per-pixel MMIO translation calls) has already
  been batched away. Remaining gap is generic TCG cost, not a device-model
  bug.
- JPEG-streaming firmware paths (retro-go launcher coverflow; stock-side
  zelda3/GB games) are bound by per-MMIO-access cost on every host (~2.3M
  register reads/s; fps tracks that rate). Root cause measured
  2026-07-25: QEMU forces an MMIO access to be its translation block's
  last instruction, so each one takes `cpu_io_recompile()` and never
  caches that -- 2.68M recompiles/s, ~60% of wall time. The polling is
  AUTHENTIC (verified on the real device: no MDMA, codec at 22% duty).
  Four approaches tried and rejected on measurement. Full analysis,
  per-platform primitive costs, diagnostic env vars and measurement
  pitfalls: `docs/emulation-performance.md`. Wine runs the Windows build
  at ~0.1fps for a separate unresolved reason (GUI-thread yield storm).
- SOLVED 2026-07-26: the in-game macOS/Windows deficit (worse than the
  10-17% previously believed -- up to 45% once wall-clock lag is counted,
  not just fps). Off Linux the main loop cannot wait less than 1ms:
  qemu_poll_ns() only uses ns-resolution ppoll() under CONFIG_PPOLL,
  which macOS/Windows lack, and the g_poll() fallback's timeout is
  rounded UP. Gameplay is ~92% idle, so wakeup promptness sets the frame
  rate. Celeste, five hosts: Win11 VM 20.0 -> 30.0fps, Win11 laptop
  24.7 -> 30.0 and 53.7s -> 29.1s wall, macOS 27.1 -> 30.0; both Linux
  hosts were already at Celeste's 30fps cap. Fixed: macOS via pselect(),
  Windows via a high-resolution waitable timer added to the wait set.
  `docs/emulation-performance.md`.
- Real subsampled chroma storage was traded for full-resolution internal
  storage in the JPEG model (a documented scope decision, not a bug).

## Now / next

- Repo cleanup pass (docs, sibling-repo references, script portability,
  automated first-run/SD-card setup) — done.
- CI is up: `.github/workflows/build-check.yml` (Linux, every push/PR) and
  `release.yml` (Linux/Mac-arm64/Mac-x86_64/Windows, tags + manual dispatch).
  Confirmed green end-to-end via real test-tag runs, including two real
  cross-platform bugs this surfaced and fixed (`timegm()`/MinGW,
  `memory_region_init_ram_from_file()` being POSIX-only — Windows now gets
  genuine persistent flash-image backing via `CreateFileMapping`, not a
  silent ephemeral-RAM fallback).
- GUI (`gwemu`, see `CLAUDE.md`'s "GUI" section): device-PROFILE-centric
  restructure, Phases 1-2 landed 2026-07-24 (profile store, staged
  wizard, in-process CFW patching, SD wiring). Remaining: Phase 3
  (Profiles top-level tab, Flash/SD tab demotion, CLI-adopt toast).
- `contrib/gnw-tools/`: C ports of the boot-image, CFW-patch and SD-image
  builders; the CFW driver is a linkable library the GUI calls in-process.
- Render path: framebuffer texture now uploaded only when the guest
  redraws, and the GPU upload moved out of the BQL -- lock contention
  from rendering 130ms/s -> 1.7ms/s (2026-07-25). Uncommitted alongside
  an SAI output-queue latency cap (Windows backlog 1.5s -> 110ms).

## Tooling notes worth keeping in mind

- QEMU's gdbstub halts the whole VM (including peripheral input-event
  delivery) on client *connect*, and for any command besides memory
  read/write. Reading peripheral state right after connecting can show
  stale pre-input values.
- `scripts/gdb_tap.py` (a logging GDB-RSP proxy) is the fastest way to see
  what a real client is actually sending when debugging an integration
  that talks to this QEMU over GDB RSP.
- Temporary `fprintf(stderr, ...)` debug prints added directly to device
  model source (rebuilt via `ninja qemu-system-arm`) are the fastest way to
  see internal QEMU device state that guest-side gdb reads can't reach at
  all — always remove them again once their diagnostic purpose is served.
  BUT: per-event prints distort what they measure. One line per DMA half
  (60/s) slowed a Windows guest 3x and produced a fabricated finding;
  clock-reads-per-MMIO cost Linux 42% of its frame rate at 2.3M
  accesses/s. Summarise once a second, gate per-event output behind `=2`,
  and re-check any surprising result with the probe switched off.
- Guest-reported metrics (firmware fps counters, LTDC vblank counts, DMA
  tick rates) are derived from emulated time and read "correct" while the
  game visibly crawls. Only wall-clock-anchored counters mean anything
  for speed — see `GNW_UI_FRAME_TRACE`'s `GUESTFPS` and the env-var table
  in `docs/emulation-performance.md`.
- `gnwmanager`'s OpenOCD backend reads device memory without halting the
  CPU (~3000 reads/s), which makes real-hardware peripheral duty cycles
  directly comparable against the emulator. That is the fastest way to
  settle "is this firmware behaviour or a modelling bug?".
