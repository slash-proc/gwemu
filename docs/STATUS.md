# Status

Last updated: 2026-07-26 (`v0.0.16`)

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
  dynamic CPU-overclock support. RCC models real peripheral resets
  (AHB1RSTR/APB2RSTR) so `HAL_DeInit()` behaves as on silicon.
- SPI/OSPI flash (dual-bank, see `docs/h7b0-flash-discrepancy.md`), SD card
  (SPI-based), RTC, DMA, TIM1/TIM2/LPTIM1, ADC, PWR, CRC, CRYP, OTFDEC
  (real AES-128-CTR extflash decryption), HASH, FLASH_R, TAMP, and the
  rest of the boot-path peripheral set — index in
  `docs/peripheral-coverage.md`.
- LTDC (real per-layer compositing, reload/vblank timing, one publish per
  guest frame), DMA2D (real per-pixel fetch and blend), JPEG (real polled
  output-register pipeline).
- Cross-platform: one rendering+audio stack (SDL_Renderer + the fork's
  own `sdl3` audiodev, no OpenGL anywhere) verified live on Linux
  (Vulkan), Windows (D3D11/software) and macOS Intel (Metal), plus Linux
  aarch64 (Raspberry Pi 4, Celeste under retro-go). Audio is solid across
  every core tested. Local Windows and aarch64 cross-builds via Docker
  (aarch64 is a true cross-compile: ~1m45s vs ~50 min emulated) and a
  Mac-over-SSH workflow — `docs/cross-platform-builds.md`. `start.bat` is
  the Windows launch path.
- Headless capture appliance for CI/test suites: truly windowless
  `-display none`, virtual-clock timeline scripts, whole-session A/V
  recording, Docker packaging — `docs/headless-capture.md`.
- GUI (`gwemu`): profile-centric ImGui front end — device profiles and
  staged wizard, in-process CFW patching, in-process SD-card creation
  from a content folder, and GDB-stub configuration (`sys.gdb.*`, applied
  at runtime, loopback by default because the stub is unauthenticated
  full guest-memory access).
- Blank internal flash is a **supported state**: v7M Lockup halts and
  re-resets the machine every 250ms instead of aborting the process, and
  stands down under `RUN_STATE_DEBUG` so gnwmanager or GDB can load and
  run a flash loader from RAM.

## Known issues (open)

- **Black screen on retro-go's "quit to main menu", second cause still
  open.** The DMA half is fixed (peripheral resets are modelled; 24/118
  -> 0/119 across interleaved runs). A second, independent bug with the
  same symptom remains: the CPU wedges in the LTDC interrupt (exception
  104 / IRQ 88) while DMA1 is clean and at its reset value. Unaffected by
  any of the render or reset work. Prime suspect is the same class of bug
  one bus over — LTDC is on APB3 and `HAL_DeInit()` resets APB3RSTR bit 3
  (LTDCRST), deliberately not modelled: unverified, and an LTDC reset
  would blank live display config, so it needs its own measurement first.
  Failure rates for this family of bug track HOST SPEED (idle Linux 3%,
  loaded Linux 17%, older Intel Mac 34%, Pi 4 worse still): the
  vulnerable window is a fixed number of guest instructions while
  peripheral events are paced by wall time. Reproduce on a slow or loaded
  host; a fast x86 box will hide it.
- **~5% windowed-vs-headless frame-rate gap on the Pi 4.** Not CPU
  starvation — the vCPU thread sits at ~81% of one core with most of the
  machine idle — and not present cost (18.6 ms/s). That leaves a
  serialisation or scheduling effect: BQL hand-off or vblank timing.
- **The SD Card tab has never been visually verified.** A view only draws
  when its tab is selected, so its `Draw()` has not executed. Layout, the
  attach-to-profile checkbox and the settings round-trip are unconfirmed.
- **GUI behaviour on the Pi is unconfirmed.** The app starts, picks the
  `opengl` SDL_Renderer and guest rendering is correct, but nobody has
  confirmed what the window actually shows.
- Native Wayland disabled by default on Linux (x11/XWayland instead) —
  three real breakages documented in `ui/gwemu.c`; revisit when SDL3's
  Wayland fractional-scale handling stabilises.
- JPEG-streaming firmware paths (retro-go launcher coverflow; stock-side
  zelda3/GB games) are bound by per-MMIO-access cost on every host: QEMU
  forces an MMIO access to end its translation block, so each takes
  `cpu_io_recompile()` — ~60% of wall time. The polling is authentic
  (verified on the real device); four fixes tried and rejected on
  measurement (`docs/emulation-performance.md`).
- Real subsampled chroma storage was traded for full-resolution internal
  storage in the JPEG model (a documented scope decision, not a bug).

## Now / next

Raspberry Pi 4 render-path performance is done (system-wide CPU 199.7% ->
129.3% of a core, renders/sec 118 -> 29 matching guest frame production
1:1; `docs/emulation-performance.md`). Next, in rough order:

- Get eyes on the GUI on a Pi and on the SD Card tab — both are the
  unverified items above, and both are cheap to close.
- The LTDC-side black screen: measure what APB3RSTR/LTDCRST actually does
  on hardware before modelling it.
- GUI profile restructure Phase 3: Profiles as a top-level tab,
  Flash/SD tab demotion, CLI-adopt toast.

## Tooling notes worth keeping in mind

- QEMU's gdbstub halts the whole VM (including peripheral input-event
  delivery) on client *connect*, and for any command besides memory
  read/write — state read right after connecting can be stale.
  `scripts/gdb_tap.py` logs what a client is actually sending over RSP.
- Temporary `fprintf(stderr, ...)` prints in device-model source reach
  state guest-side gdb can't — but per-event prints distort what they
  measure (one line per DMA half slowed a Windows guest 3x and produced a
  fabricated finding). Summarise once a second, gate per-event output
  behind `=2`, re-check with the probe off.
- Guest-reported metrics (firmware fps counters, LTDC vblank counts, DMA
  tick rates) are derived from emulated time and read "correct" while the
  game visibly crawls. Only wall-clock-anchored counters mean anything.
  Likewise a process-only CPU reading understates any render-path cost: a
  third of the last saving was in the X server and window manager.
- `gnwmanager`'s OpenOCD backend reads device memory without halting the
  CPU (~3000 reads/s), making real-hardware peripheral duty cycles
  directly comparable — the fastest way to settle "is this firmware
  behaviour or a modelling bug?".
