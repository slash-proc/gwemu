# Roadmap: a real STM32H7B0 machine model for QEMU

## Why

Prior Game & Watch homebrew dev workflows tested pre-hardware via a QEMU
harness running on QEMU's generic Cortex-M7 MPS2-AN500 target — not a real
STM32H7B0 model, because none exists upstream. One common workaround is
fault-trapping accesses to the DMA2D peripheral (a BusFault handler decodes
the faulting STR instruction and executes the operation against a software
register shadow) instead of modeling flash/QSPI boot, RCC, or the real
memory map at all. That gets firmware booting and rendering, but it's slow
(~10+ faults per DMA2D call) and only covers the narrow slice of hardware
behavior a given test harness happens to exercise.

This project is the real-machine-model answer to that: a native STM32H7B0
device/machine model compiled into a maintained custom QEMU build, with
DMA2D implemented as a real device rather than trapped. Modeled after how
`xemu` gives Xbox homebrew/game devs a fast, accurate dev loop against real
NV2A GPU behavior instead of a slow stand-in.

## Decisions

- **GPU accel scope**: both a native DMA2D device model (real semantics, no
  fault-trap overhead) *and* a hardware-accelerated display output path
  (framebuffer as a host texture) — emulate real hardware precisely, but
  push pixel work onto the host efficiently, same as xemu. Revised
  2026-07-23: that path is SDL_Renderer, not OpenGL, and OpenGL is no
  longer a dependency anywhere in this project (see Phase 3).
- **SoC completeness**: broad, not minimal. Real memory map, flash/QSPI XIP
  boot path, and enough of the RCC/clock tree that unmodified retro-go
  firmware images can eventually boot the way they do on real hardware —
  not just the narrow GWHB test-harness path.
- **Repo strategy**: full hard fork of upstream QEMU (like xemu itself), with
  STM32H7B0 support added in-tree under `hw/arm/` and `hw/display/`,
  periodically rebased against upstream from a pinned tag. QEMU's board/
  device registration (Kconfig, meson.build device lists) isn't designed for
  true out-of-tree boards.

## Phases

### Phase 0 — Fork setup
- Baseline `arm-softmmu` build works unmodified from pinned tag `v9.2.4`.
- Add `hw/arm/gnw-h7b0.c` skeleton: bare Cortex-M7
  (`ARM_CPU_TYPE_NAME("cortex-m7")`) with just RAM at the real STM32H7B0
  addresses (AXI SRAM `0x24000000`+, DTCM `0x20000000`, per
  `STM32H7B0.svd`) and nothing else, registered in `hw/arm/Kconfig` and
  `hw/arm/meson.build`. Goal: boots to a spin loop, proves the fork/build/
  board-registration pipeline works before adding real complexity.

### Phase 1 — Memory map + boot path
- **Done**: real memory regions modeled from RM0455 Table 6 (not just the
  SVD, which doesn't cover plain RAM/flash regions): ITCM, DTCM, AXI
  SRAM1/2/3, AHB SRAM1/2, SRD SRAM, backup SRAM, internal flash banks 1/2,
  external OSPI flash placeholder. See `docs/STATUS.md` for exact
  addresses/sizes and `docs/h7b0-flash-discrepancy.md` for the internal
  flash size override vs RM0455.
- **Done**: boots a test kernel via ITCM (genuinely RAM at address `0x0`
  on real hardware) instead of Phase 0's temporary alias hack.
- **Done**: kernel now loads at flash bank 1 (`0x08000000`) with the
  ARMv7M CPU's `init-nsvtor` property pointed there, so reset reads SP/PC
  from flash's vector table — modeling real hardware's BOOT_ADD address-0
  remap without a fake alias region. See `docs/STATUS.md` for the
  `init-nsvtor`-vs-`init-svtor` gotcha (Cortex-M7 has no TrustZone-M).
- **Done**: minimal RCC stub — enough register read/write behavior that
  real firmware's clock-init code doesn't hang polling a permanently-zero
  status bit. Not cycle-accurate at this stage.
- **Done**: goal achieved and then some. A real `gnw-chainloader` firmware
  image boots from flash, gets through clock/memory/power init, and
  reaches real LTDC init before stopping on a (currently) unbacked LTDC
  register access. See `docs/STATUS.md` for the full list of gaps found and
  fixed along the way (RCC LSI/LSE ready bits; new PWR, OCTOSPI1/2, ADC
  devices; several plain-RAM peripheral placeholders).

**Note**: LTDC (and likely DMA2D alongside it, since retro-go/
chainloader firmware uses both together) got pulled forward from
Phase 2/3 below sooner than planned, because that's where real boot
now stops and it's the user's current priority over continuing
Phase 4's peripheral-gap whack-a-mole. Treat the phase numbers below as
soft ordering, not a strict gate.

### Phase 2 — DMA2D device model
- New device `hw/display/gnw-dma2d.c`: `MemoryRegionOps` for the register
  window at `0x52001000`, a `qemu_bh`/timer-driven operation that performs
  fill/blit/blend/CLUT against guest RAM directly (`address_space_rw`) when
  the START bit is set. Same operation semantics as the existing
  `dma2d_emu.c` fault-trap harness (RGB565/ARGB8888 conversion,
  palette-index-0 transparency), but as a real memory-mapped device.
- Test against minicraft-gnw's real `src/dma2d.c`, comparing rendered
  output to the known-good MPS2+fault-trap output as a regression baseline.

### Phase 3 — Efficient display output (no OpenGL)
- **Superseded in scope, not in goal.** The original plan was a GL-backed
  texture upload via `-display sdl,gl=on` / `gtk,gl=on`. That is dead: the
  hard OpenGL requirement was removed after a real "Unable to create
  OpenGL context" failure on Windows, and the `gwemu` GUI now renders
  through SDL_Renderer (D3D11/Metal/Vulkan per platform, software
  fallback) with ImGui's `imgui_impl_sdlrenderer3` backend. Don't
  reintroduce GL calls, GL context creation or epoxy into `ui/` — see
  `CLAUDE.md`. The goal that survives is the one GL was only ever a means
  to: get the framebuffer to the screen without the host render path
  costing more than the emulation.
- **Largely delivered** (2026-07-25/26): the framebuffer texture is
  uploaded only when the guest actually redraws, the upload moved out of
  the BQL (rendering's lock contention 130ms/s -> 1.7ms/s), the LTDC
  model stopped treating firmware's second `SRCR` reload as a frame
  boundary, and the host render loop is content-gated instead of
  free-running. Measured on a Raspberry Pi 4: system-wide CPU 199.7% ->
  129.3% of a core, renders/sec 118 -> 29 (1:1 with guest frame
  production), `SDL_RenderPresent` 715.8 -> 18.6 ms/s. Details and the
  measurement pitfalls in `docs/emulation-performance.md`.
- Remaining: a ~5% windowed-vs-headless frame-rate gap on the Pi, now
  characterised as a serialisation/scheduling effect (BQL hand-off or
  vblank timing) rather than CPU starvation.
- The pygame `gnw_qemu_viewer.py` bridge is gone — the framebuffer is a
  native QEMU display device and `-display gwemu` is the GUI backend.

### Phase 4 — Remaining peripherals for real-firmware boot
- GPIO (buttons), UART/USART, SysTick, and whatever else real firmware
  touches during boot that Phase 1's stubs don't cover — expand
  incrementally, driven by "what does the firmware actually fault on next."

## Key references

- `STM32H7B0.svd` (repo root) — authoritative register map.
- QEMU's existing `hw/arm/stm32f4*`-family boards as a structural template
  for SoC container objects.

## Verification per phase

- Phase 0: build succeeds, `qemu-system-arm -M gnw-h7b0 -kernel <spin-loop.elf>`
  runs, gdb (`-s -S`) confirms PC progresses.
- Phase 1: a real-hardware-addressed minicraft-gnw firmware image reaches
  the same init milestones already validated on real hardware
  (`backup_init`/`audio_init`/`input_init`/`screen_init`).
- Phase 2: visually compare rendered title/about screens against the
  existing MPS2+fault-trap harness's known-good output.
- Phase 3: confirm the SDL_Renderer path shows frames identical to the
  software blit, and measure host-side cost — renders/sec against guest
  frame production, present time, and **system-wide** CPU, not the
  emulator process alone (a process-only view understated the last
  render-path saving by more than half).
- Ongoing: keep minicraft-gnw's MPS2 fault-trap harness working and
  untouched as the regression baseline throughout.
