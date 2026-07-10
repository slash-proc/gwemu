# Roadmap: a real STM32H7B0 machine model for QEMU

## Why

`minicraft-gnw` (a GBA-to-Game & Watch port) tests pre-hardware via a QEMU
harness running on QEMU's generic Cortex-M7 MPS2-AN500 target — not a real
STM32H7B0 model, because none exists upstream. The harness works around
this by fault-trapping accesses to the DMA2D peripheral (a BusFault handler
decodes the faulting STR instruction and executes the operation against a
software register shadow) and doesn't model flash/QSPI boot, RCC, or the
real memory map at all. This got firmware booting and rendering correctly,
but it's slow (~10+ faults per DMA2D call) and only covers the narrow slice
of hardware behavior the GWHB test harness happens to exercise.

This project is the deferred "real correct answer" flagged in
`../../minicraft-gnw/docs/qemu-testing.md`: a native STM32H7B0 device/
machine model compiled into a maintained custom QEMU build, with DMA2D
implemented as a real device rather than trapped. Modeled after how `xemu`
gives Xbox homebrew/game devs a fast, accurate dev loop against real NV2A
GPU behavior instead of a slow stand-in.

## Decisions

- **GPU accel scope**: both a native DMA2D device model (real semantics, no
  fault-trap overhead) *and* a GPU-backed display output path (host OpenGL
  texture upload for the framebuffer) — emulate real hardware precisely,
  but push pixel work onto the host efficiently, same as xemu.
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
- Model real memory regions from the SVD: ITCM, DTCM, AXI SRAM, backup
  SRAM, and QSPI/OSPI flash where G&W firmware actually lives (XIP boot,
  not the RAM_EMU load-and-jump the current GWHB harness fakes).
- Minimal RCC stub: enough register read/write behavior that real
  firmware's clock-init code doesn't hang polling a permanently-zero status
  bit. Not cycle-accurate at this stage.
- Goal: a real STM32H7B0-target firmware image begins executing from flash
  and gets through early clock/memory init without faulting.

### Phase 2 — DMA2D device model
- New device `hw/display/gnw-dma2d.c`: `MemoryRegionOps` for the register
  window at `0x52001000`, a `qemu_bh`/timer-driven operation that performs
  fill/blit/blend/CLUT against guest RAM directly (`address_space_rw`) when
  the START bit is set. Same operation semantics as the existing
  `dma2d_emu.c` fault-trap harness (RGB565/ARGB8888 conversion,
  palette-index-0 transparency), but as a real memory-mapped device.
- Test against minicraft-gnw's real `src/dma2d.c`, comparing rendered
  output to the known-good MPS2+fault-trap output as a regression baseline.

### Phase 3 — GPU-accelerated display output
- Upload the DMA2D-produced framebuffer as a texture via QEMU's existing
  GL-backed UI backends (`-display sdl,gl=on` / `gtk,gl=on`) instead of a
  software blit — additive on top of Phase 2, not blocking it.
- Decide whether the pygame `gnw_qemu_viewer.py` bridge is still needed once
  the framebuffer is a native QEMU display device (likely not).

### Phase 4 — Remaining peripherals for real-firmware boot
- GPIO (buttons), UART/USART, SysTick, and whatever else real retro-go/GWHB
  firmware touches during boot that Phase 1's stubs don't cover — expand
  incrementally, driven by "what does the firmware actually fault on next,"
  same methodology already proven in minicraft-gnw's bring-up sessions.

## Key references

- `STM32H7B0.svd` (repo root) — authoritative register map.
- `../../minicraft-gnw/tools/retro-go-porting-toolkit/host/qemu/dma2d_emu.h`,
  `bus_fault.c`, `fault_decode.c` — validated DMA2D operation semantics to
  port into Phase 2's native device model.
- `../../minicraft-gnw/tools/retro-go-porting-toolkit/host/qemu/startup.c`
  — existing SysTick/vector-table handling understanding for Phase 4.
- `../../minicraft-gnw/docs/qemu-testing.md` and `docs/real-hardware-testing.md`
  — catalog of real STM32H7B0 behavior differences already discovered.
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
- Phase 3: confirm GL-backed display renders identical frames, measure
  frame-time improvement vs Phase 2's software path.
- Ongoing: keep minicraft-gnw's MPS2 fault-trap harness working and
  untouched as the regression baseline throughout.
