# Cross-platform builds

Reference for producing gwemu binaries for all three supported platforms.
CI (`release.yml`) covers tag/dispatch builds; this documents the *local*
workflows used when iterating without GitHub.

## The one-stack rule

Since 2026-07-23 every platform runs the identical rendering and audio
code: SDL_Renderer (D3D11 on Windows, Metal on macOS, Vulkan→GL→software
on Linux) and the fork's own `sdl3` audiodev (WASAPI / CoreAudio /
PipeWire). There is no OpenGL requirement and no per-platform display or
audio backend to keep working. If a platform-specific rendering/audio bug
appears, suspect SDL3 or our thin glue (`ui/gwemu.c`,
`ui/xui/gl-helpers.cc`, `audio/sdl3audio.c`) — not a divergent code path.

## Linux (native)

Standard build (see CLAUDE.md "Build"). Notes:

- The GUI defaults to the x11 video driver (XWayland) — deliberate, see
  the comment in `ui/gwemu.c` near `SDL_Init()`. `SDL_VIDEODRIVER=wayland`
  overrides.
- Renderer preference is Vulkan (hint in `ui/gwemu.c`), falling back
  automatically.
- `scripts/boot_qemu.sh` auto-picks `sdl3` as the audiodev.

### Linux aarch64 (Raspberry Pi 4/5)

Same native build — no cross-compilation, no source changes. The release
pipeline builds it on GitHub's native `ubuntu-24.04-arm` runner and ships
`gwemu-<version>-aarch64.AppImage` alongside the x86_64 one;
`contrib/appimage/build-appimage.sh` picks its `ARCH` from `uname -m`
unless overridden.

To reproduce the release leg locally on an x86_64 dev box, run it under
qemu-user binfmt (correctness check only — emulated, so it's slow):

```
docker run --rm --platform linux/arm64 -v "$PWD":/src -w /src ubuntu:22.04 ...
```

Note this validates that the build and packaging work; it says nothing
about runtime performance on real Pi hardware (TCG throughput, and the
Vulkan/GL/software renderer fallback chain on the Pi's VideoCore driver,
both need testing on a real device).

## Windows (cross-compiled from Linux, Docker)

No MSYS2 and no Windows machine needed to *build* (only to run/test).

Since 2026-07-24 the Windows build is FULLY STATIC (xemu's model): the
MXE-based toolchain in `contrib/docker-win-static/` (derived from xemu's
public toolchain image, plus a static liblzma for `contrib/gnw-tools`)
links everything -- glib included -- into a single `gwemu.exe` that
imports only Windows system DLLs. No bundled-DLL dist folder anymore.

1. One-time image setup:

   ```
   docker build -t gwemu-win-static contrib/docker-win-static/
   ```

2. Configure + build (from repo root; `build-win-static/` is the
   out-of-tree dir; the `.static-` cross prefix is what selects MXE's
   static libs):

   ```
   mkdir -p build-win-static
   docker run --rm -v "$PWD":/src -w /src/build-win-static gwemu-win-static \
     bash -c 'git config --global --add safe.directory "*" \
            && ../configure --target-list=arm-softmmu \
              --cross-prefix=x86_64-w64-mingw32.static- \
              --disable-sdl --disable-sdl-image --disable-gtk \
            && ninja -j12 qemu-system-arm.exe gwemu.exe \
            && x86_64-w64-mingw32.static-strip gwemu.exe'
   ```

   `safe.directory`: without it the root-run container can't read git
   metadata and `gwemu_commit` silently stamps blank into the binary.
   `--disable-sdl/--disable-sdl-image` is QEMU's legacy SDL2 display,
   not our SDL3 GUI (built static via its cmake subproject).
   `gwemu.exe` (the meson alias) is already flipped to a GUI-subsystem
   PE (`scripts/make-gwemu-alias.py`) so it opens no console window;
   `qemu-system-arm.exe` deliberately stays a console app. NOTE: a
   GUI-subsystem exe's stderr only goes somewhere if redirected
   (`2> file`); an *attached* console flooded with per-frame trace
   output blocks the process into unusability -- keep the GNW_* trace
   env vars off (unset/`=0`) for normal runs.

   Verify self-containment: `objdump -p gwemu.exe | grep 'DLL Name'`
   must list only Windows system DLLs (CI enforces this).

3. Installer (optional): `contrib/gwemu-installer/gwemu.nsi` via
   `makensis` (`apt-get install nsis` inside the same container works),
   `-DDISTDIR=<dir with gwemu.exe> -DVERSION=x.y.z -DOUTFILE=...`.

4. `start.bat` at the repo root launches it with the standard image set
   (run from the repo root so relative paths resolve). Flash images bind
   via the normal `-global gnw-h7b0-soc.*-image=` properties — Windows
   has real persistent mmap backing (`CreateFileMapping`/`MapViewOfFile`
   in `hw/arm/gnw_h7b0_soc.c`). `-device loader,file=...,addr=...,
   force-raw=on` (addresses 0x08000000 / 0x08100000 / 0x90000000) is the
   ephemeral alternative.

5. Do NOT test under wine. Confirmed 2026-07-24: the GUI does launch
   and boot firmware there (absolute `Z:\...` paths required for the
   flash-image properties), but at ~1 frame per 10-15s it is useless
   for judging anything, its console/stderr handling swallows output,
   and conclusions drawn from wine runs in this project have repeatedly
   been wrong. The definitive test is real Windows; make each real run
   count via targeted stderr diagnostics (`2> gwemu.log`).

## macOS (native build over SSH)

The Intel Mac builds natively (Homebrew toolchain). Workflow from the
Linux dev box, no GitHub:

1. Sync: `git push <mac-remote> gnw-h7b0` (the Mac repo has
   `receive.denyCurrentBranch ignore`; `git reset --hard` after) plus
   `rsync -aR --files-from=<git-status-list>` for uncommitted work.
   Verify with checksums, not faith.
2. Build: `ssh mac 'zsh -lc "cd gwemu/build && ../configure
   --target-list=arm-softmmu && ninja"'` — `zsh -lc` matters
   (non-interactive SSH lacks the Homebrew PATH).
3. Meson caches subproject options in the build dir's coredata:
   changing `default_options` for a subproject in meson.build does NOT
   reconfigure it. Apply with
   `meson configure -Dimgui:sdl3_renderer=enabled -Dimgui:opengl=disabled`
   (same trap exists on every platform's existing build dir).
4. SDL3's Apple framework list lives in meson.build
   (`sdl3_macos_frameworks`) and must name every framework SDL3's static
   backends touch: AudioToolbox, CoreHaptics, ForceFeedback,
   GameController, Carbon, UniformTypeIdentifiers, Metal, QuartzCore,
   IOKit, Cocoa, CoreVideo. Undefined `_MTL*`/`_IO*`/`_CA*` symbols at
   link mean the list is missing one.
5. Gitignored assets (`backup/qemu-images/*`, `sdcard.img` +
   `sdcard-overlay.qcow2`) do NOT travel with git — stale copies on the
   Mac have masqueraded as code regressions before (missing SD card
   reproduced an already-fixed black-screen bug; stale extflash carried
   old retro-go settings). Sync and checksum them too when behavior
   differs between machines.
