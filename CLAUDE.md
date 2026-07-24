# CLAUDE.md

Rules and orientation for working in this repo. Keep this short and
directive; put rationale and history elsewhere (see below).

## What this repo is

`gwemu` (fork name `slash-proc/gwemu`) is a hard fork of upstream QEMU,
adding a real machine model for the Nintendo Game & Watch's STM32H7B0 SoC.
It exists because no STM32H7B0 machine exists in upstream QEMU. Modeled
after how `xemu` gives Xbox homebrew devs a fast, accurate dev loop against
real NV2A GPU behavior — see `docs/roadmap.md` for the full phased plan.

## Doc map

- `docs/STATUS.md` — current snapshot only (what phase, what works, what's next).
  Rewritten in place as state changes, not appended to. Keep under ~100
  lines.
- `CHANGELOG.md` — dated one-line entries of what landed. This is where
  "what happened" lives, not docs/STATUS.md.
- `docs/` — design rationale and reference material that doesn't fit as a
  code comment (e.g. clock-tree math, known hardware/datasheet
  discrepancies). Not a running diary: once an investigation's findings are
  acted on, the "why" belongs as a comment next to the code it justifies,
  and the writeup gets deleted rather than accumulated. If you're about to
  write a dated `session-*.md` narrative doc, prefer a code comment or a
  CHANGELOG entry instead.
- `docs/peripheral-coverage.md` — one-line-per-peripheral index of what has
  a real device model vs. a register-shadow stub vs. nothing at all. Hand-
  maintained, not a diary; update it when peripheral coverage changes.
- `docs/h7b0-clock-tree-findings.md` — hard-won HSI-vs-HSE, PLL2 VCO/
  fractional-N formula, SAI1SEL mux, and TIM2/HCLK clock-tree facts;
  consult before adding real-clock-dependent behavior to any new peripheral
  instead of re-deriving or re-guessing a frequency.
- **`gnwmanager` (a separate tool, not part of this repo)**: if you use it
  for real-hardware or `--qemu` gdbstub workflows, default to not adding
  new capabilities to it for this repo's convenience — treat it as a
  dependency with its own independent development, and prefer writing
  pure-consumer scripts against its existing public API. Small, targeted
  bugfixes in its own code are fine when something it does is actually
  broken; growing new features into it to serve this repo is not the
  default.

## Repo/remote conventions

- `origin` = `slash-proc/gwemu` (this fork, push target).
- `upstream` = `qemu/qemu` (read-only, fetch only, never push).
- Pinned base: tag `v11.0.2`. Don't casually rebase onto upstream `master`;
  bump the pin deliberately and note it in CHANGELOG.md when we do.
- All game-and-watch-specific additions live in-tree (like xemu's
  `hw/xbox/`), primarily under `hw/arm/` (SoC/board) and `hw/display/`
  (DMA2D device model), not as an out-of-tree build — QEMU's Kconfig/
  meson board registration isn't designed for out-of-tree boards.

## Source-of-truth rules

- `STM32H7B0.svd` at repo root (5.2MB, gitignored — not tracked, won't
  survive a fresh clone) is the authoritative register/address map. Use it
  for peripheral base addresses and register layouts instead of
  hand-transcribing from the reference manual. Re-fetch it yourself if
  it's missing (ST distributes it via the CMSIS-Pack index / STM32CubeMX,
  not a single stable download URL, so there's no `fetch-*.sh` for it —
  same reasoning as `rm0455.pdf` below). `scripts/snapshot_registers.py`,
  `triage_diffs.py`, and `state_transplant.py` parse it directly at this
  repo-root path.
- `rm0455.pdf` at repo root (STM32H7A3/7B3/7B0 reference manual) is
  expected to exist locally for memory-map/register lookups but is
  gitignored (58MB, copyrighted ST document) — not tracked, won't survive
  a fresh clone. Re-fetch it yourself if it's missing.
- `sdk/` (gitignored, ~65MB, not a build dependency) is the real
  STM32CubeH7 HAL driver + device-specific CMSIS source used by real
  Game & Watch firmware, so it reflects what real firmware actually does.
  Run `scripts/fetch-sdk.sh` to populate it if missing. Use
  `sdk/stm32h7xx-hal-driver/Src/stm32h7xx_hal_{rcc,dma2d,ospi}.c` and
  `sdk/cmsis-device-h7/Include/stm32h7b0xx.h` as ground truth for
  peripheral register behavior when writing device models — same role as
  `STM32H7B0.svd`/`rm0455.pdf`, but showing actual driver logic (e.g.
  which status bits a real init sequence polls for) rather than just
  register layout.
- RM0455 is wrong about internal flash on real H7B0 silicon (says 128K
  single-bank; real hardware is 2x256K dual-bank, community-verified, not
  documented anywhere official). Trust the project owner over RM0455 here.
  See `docs/h7b0-flash-discrepancy.md` before touching flash sizing.
- Real hardware reports imprecise BusFaults; QEMU's bus model is fully
  synchronous — don't assume QEMU's default fault timing matches real
  silicon without checking.
- Stock (official Nintendo) firmware ships with no debug symbols, so
  tracing what it's actually waiting on requires disassembly/decompilation
  (e.g. Ghidra, imported at base `0x08000000`, `ARM:LE:32:Cortex`). Always
  boot the user's supplied stock firmware images unmodified for
  stock-accuracy work — don't apply community firmware patches (save-data
  erasure, encryption bypass, etc.) meant for producing custom-firmware
  builds; those are a different use case.

## Build

Standard QEMU meson build, ARM softmmu target:
```
mkdir build && cd build
../configure --target-list=arm-softmmu
ninja
```
Needs the usual QEMU build prerequisites (`ninja`, `pkg-config`,
`libglib2.0-dev`, `libpixman-1-dev` at minimum) plus a display backend
dev package (`libsdl2-dev` or `libgtk-3-dev`) — without one, `configure`
succeeds silently but produces a QEMU binary with no display backend at
all, and the failure only surfaces later at `boot_qemu.sh` launch time
("no usable display backend"), disconnected from its actual cause. This
whole section is a placeholder for a real README once we write one (see
"Outsider workflow" below) — not yet gnw-specific beyond the note above.

## GUI (`gwemu`, Item 5 of the cleanup/roadmap effort)

A real ImGui-based GUI exists now, ported from xemu (github.com/xemu-project/xemu)
and rebranded — this repo's own product identity is `gwemu`/`GWemu`; "xemu" is kept
only where it's genuine attribution to the real upstream project (license headers,
"ported from xemu" comments, the About tab's credits — never blindly renamed).

- **No OpenGL anywhere (2026-07-23)**: the GUI renders via SDL_Renderer
  (D3D11/Metal/Vulkan per platform, software fallback) and ImGui's
  imgui_impl_sdlrenderer3 backend. Don't reintroduce direct GL calls, GL
  context creation, or epoxy usage into `ui/` -- the hard GL requirement was
  removed deliberately after a real "Unable to create OpenGL context" failure
  on Windows. Audio likewise: the fork's own `sdl3` audiodev
  (`audio/sdl3audio.c`) is the default on every host; prefer
  `-audiodev sdl3,id=snd0` over pa/coreaudio/dsound in new invocations.
- **Linux video driver defaults to x11** (XWayland) in `ui/gwemu.c` -- three
  confirmed native-Wayland breakages (libdecor crash, ImGui viewport
  whitelist, fractional-scale UI mis-sizing). `SDL_VIDEODRIVER=wayland`
  overrides for testing. The old "launch with SDL_VIDEODRIVER=x11" advice
  below is now automatic.
- **Headless capture** (CI/test-suite use): `-display none` is genuinely
  windowless; `GNW_TIMELINE=<script>` drives virtual-clock-repeatable
  button/screenshot sequences and `GNW_RECORD` captures A/V -- see
  `docs/headless-capture.md` and `contrib/docker-headless/`. Scripting is
  deliberately the ONLY headless control surface (no QMP commands/CLI/REST
  -- shelved by explicit decision; wall-clock input is unrepeatable).
- **Windows**: cross-built from Linux via Docker -- see
  `docs/cross-platform-builds.md` for the exact workflow (image, configure
  flags, dist packaging). `start.bat` at the repo root is the Windows launch
  path. Windows flash persistence uses the CreateFileMapping path in
  `hw/arm/gnw_h7b0_soc.c` -- the `-global gnw-h7b0-soc.*-image=` properties
  work there; `-device loader,...` remains the ephemeral alternative.
- Build additions: SDL3, Dear ImGui (a committed docking-branch vendor tree at
  `subprojects/imgui/` — NOT wrap-fetched, since no upstream URL hosts this
  project's exact xemu-patches-on-docking-branch merge; see the commit that
  landed it for provenance), ImPlot, `genconfig` (the `config_spec.yml` ->
  generated-settings-struct pipeline), tomlplusplus, nlohmann/json, libepoxy.
  Source lives in `ui/xui/` (the actual HUD: menu bar, tabs, widgets) and
  `ui/gwemu*.{c,h,cc,m}` (SDL3 window/GL-context/event-loop glue).
- `-display gwemu` is the GUI display backend (was `-display xemu` before the
  rebrand — if you find an old invocation using the xemu name, it's stale).
- **Always build with `ninja -j12`, not bare `ninja`** — bare `ninja` grabs
  every core on the host and has caused real problems in this dev environment
  when multiple things are building concurrently.
- **Multi-viewport (detachable windows) is deliberately OFF** (`ImGuiConfigFlags_ViewportsEnable`
  is not set, `DockingEnable` is). Dear ImGui's own SDL3 backend only wires up
  cross-window mouse-coordinate translation on a hardcoded platform whitelist
  (Windows/Mac/X11) — Wayland isn't on it, and this project's real dev/test
  environment is Wayland, so enabling it produces broken, offset mouse
  hit-testing on any detached window. Don't re-enable this without either
  forcing `SDL_VIDEODRIVER=x11` permanently or confirming on a whitelisted
  platform first.
- **Testing in a Wayland dev environment**: no longer needs manual
  `SDL_VIDEODRIVER=x11` -- gwemu defaults to the x11 driver on Linux itself
  (see the "Linux video driver defaults to x11" bullet above).
  Screen-capture tooling (`import`/ImageMagick) is blocked in this sandbox for
  every agent that's tried it this session — don't assume you can screenshot;
  fall back to process-stability checks (stays alive, clean log, correct GPU
  detection) and say plainly when visual confirmation wasn't possible.
- **Async discipline**: never run a slow operation (subprocess `popen()`,
  file I/O over more than a few hundred KB) synchronously inside an ImGui
  widget's click handler — it blocks the render/event loop and the whole
  app appears hung (confirmed real-world: building an SD card image this way
  froze the app for minutes). Use a background thread + an atomic/mutex-guarded
  status struct the render loop polls each frame instead.
- **`config_spec.yml` nesting matters** — e.g. `debug:` lives under `display:`,
  not top-level, so the generated field is `g_config.display.debug.foo`, not
  `g_config.debug.foo`. Check the actual YAML indentation before referencing a
  new field; a wrong assumption here fails at compile time with a
  `'struct config' has no member named ...` error, not a silent bug, but it's
  cost real time more than once this session.
- **`execv()`-based restart and settings**: the Flash tab's "Apply" flow
  restarts the process (`execv()`) to pick up new flash-image bindings, since
  QEMU can't hot-swap a RAM region's file backing after realize. `execv()`
  does NOT run `atexit` handlers — anywhere that relies solely on an
  atexit-registered settings save (rather than saving immediately on change)
  will silently lose that change across an Apply-triggered restart. Save
  explicitly before any `execv()` call.
- **`contrib/gnw-tools/`**: C ports of this project's own Python asset-building
  scripts, written specifically so the GUI can call them as real library
  functions instead of shelling out to Python. `gnw-make-boot-images` (from
  `make_boot_images.py`) and `gnw-make-cfw-images` (from `make_cfw_images.py`,
  including a full from-scratch C port of gnwmanager's Thumb-2 assembler,
  lz77 decompressor, LZMA1 compressor via system `liblzma`, and the
  relocation/patch engine) are both byte-exact verified against their Python
  originals for both games. The CFW driver is a linkable library
  (`gnw_cfw_build_images`) -- the GUI patches in-process, no popen.
- **Device profiles** (2026-07-24): the GUI is profile-centric -- named sets of
  bank1/bank2/extflash + optional SD living in per-profile dirs under app data
  (`ui/gwemu-profiles`), created by the staged wizard in `ui/xui/profile-wizard.*`
  (hosted inside the settings window). Images stay content-blind opaque blobs
  (owner decision -- no extflash offset awareness). UI colors/spacing route
  through `ui/xui/gnw-style-tokens.hh`; the compiled FontAwesome subset is ~55
  glyphs and `MergeMode` attaches icons PER FONT in `font-manager.cc` -- a '?'
  box means the glyph isn't merged into the font you're drawing with, or isn't
  in the subset at all.
- **Occluded-window render throttle** (`ui/gwemu.c` gl_render_frame): the settings
  window presents FIRST; a self-clocking throttle (2 consecutive >50ms main
  presents) skips the main window while covered, resumes on main-window events
  (EXPOSED/FOCUS/MOUSE_ENTER; Mutter never sets SDL's occlusion flag) with a 5s
  failsafe probe (a probe BLOCKS ~500ms -- never probe frequently).
  `GNW_UI_FRAME_TRACE=1` prints per-second pacing lines. GNW_* diag env vars
  treat unset/empty/0 as OFF (`gnw_env_enabled`) -- never bare getenv()!=NULL,
  and never let per-frame stderr spam reach an attached Windows console (it
  blocks the process into unusability).
- **`gnw-make-sd-image`** (contrib/gnw-tools): C MBR+FAT32 SD image builder
  behind a sector-write callback (QEMU-block-layer glue reuses it);
  `scripts/make_sdcard_image.py` is a test oracle only now.
- When multiple agents/sessions touch `ui/xui/main-menu.cc` (the shared tab
  registration point) concurrently, keep each tab's actual content in its own
  `.cc`/`.hh` file pair and only touch `main-menu.cc`/`.hh` for the minimal
  registration lines — this held up well across several genuinely-concurrent
  editing sessions this project has already gone through.

## Outsider workflow (first-run, end to end)

This is internal working notes for now; fold into a real README once we
get to it (tracked, not forgotten).

1. Build per "Build" above.
2. Get your own stock firmware dumps from real hardware: `gnwmanager dump`
   produces `internal_flash_backup_<game>.bin` and `flash_backup_<game>.bin`
   — place both in `backup/`. This repo cannot ship copyrighted Nintendo
   firmware, so there's no way around owning real hardware for this step.
3. `./scripts/make_boot_images.py <game>` — builds
   `backup/qemu-images/<game>-{bank1,bank2,extflash}.bin` from step 2's dumps.
4. `./scripts/boot_qemu.sh <game>` — boots stock firmware.
   **`boot_qemu.sh` is a bash script — Linux/Mac only, not Windows.**
   No Windows-native launch path exists yet; this is exactly the kind of
   gap the planned GUI (see roadmap) should paper over, not something to
   hand-solve piecemeal in the meantime.
5. Optional, CFW/dual-boot testing: `./scripts/make_cfw_images.py <game>`
   needs `gnwmanager` importable (`pip install gnwmanager`, or point
   `GNWMANAGER_PATH` at a local checkout) — then `boot_qemu.sh <game>
   --patched`.
6. Optional, homebrew/retro-go: needs a separately built
   `game-and-watch-retro-go-sd` checkout (its own project, its own build
   steps — out of scope to automate here) to produce an `sd_content/`
   directory, then `./scripts/make_sdcard_image.py --content <that dir>`.
7. `STM32H7B0.svd`/`sdk/`/`rm0455.pdf` are optional reference material for
   device-model development or the diagnostic scripts — never required
   just to boot something.
