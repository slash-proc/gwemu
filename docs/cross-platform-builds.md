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

## Windows (cross-compiled from Linux, Docker)

No MSYS2 and no Windows machine needed to *build* (only to run/test).

1. One-time image setup (QEMU's own CI cross image + two extras):

   ```
   docker pull registry.gitlab.com/qemu-project/qemu/qemu/fedora-win64-cross:latest
   cat > /tmp/gwemu-win-cross.dockerfile <<'EOF'
   FROM registry.gitlab.com/qemu-project/qemu/qemu/fedora-win64-cross:latest
   RUN dnf -y install mingw64-xz mingw64-xz-libs cmake ninja-build && dnf clean all
   EOF
   docker build -t gwemu-win-cross -f /tmp/gwemu-win-cross.dockerfile .
   ```

   (`mingw64-xz` = liblzma for `contrib/gnw-tools`' LZMA compressor.)

2. Configure + build (from repo root; `build-win/` is the out-of-tree dir):

   ```
   mkdir -p build-win
   docker run --rm -v "$PWD":/src -w /src/build-win gwemu-win-cross \
     sh -c '../configure --target-list=arm-softmmu \
              --cross-prefix=x86_64-w64-mingw32- \
              --disable-sdl --disable-sdl-image --disable-gtk \
            && ninja qemu-system-arm.exe'
   ```

   `--disable-sdl/--disable-sdl-image` is REQUIRED, not cosmetic: the
   mingw SDL2.dll fails DllMain initialization and aborts the whole
   process at startup (status c0000142) before main() runs. SDL3 is
   unaffected (statically linked). `--disable-gtk` just drops an unneeded
   display backend and ~20 DLLs.

3. Package a portable folder (exe + every non-system DLL, stripped):
   the dist-collection loop lives in this repo's session history and CI;
   in short: recursively resolve `objdump -p | grep 'DLL Name'` against
   `/usr/x86_64-w64-mingw32/sys-root/mingw/bin`, copy matches next to the
   exe, `x86_64-w64-mingw32-strip` everything. Result ≈ 31 files / 55MB
   at `build-win/dist/`.

4. `start.bat` at the repo root launches it with the standard image set
   (run from the repo root so relative paths resolve). Flash images bind
   via the normal `-global gnw-h7b0-soc.*-image=` properties — Windows
   has real persistent mmap backing (`CreateFileMapping`/`MapViewOfFile`
   in `hw/arm/gnw_h7b0_soc.c`). `-device loader,file=...,addr=...,
   force-raw=on` (addresses 0x08000000 / 0x08100000 / 0x90000000) is the
   ephemeral alternative.

5. Smoke-testing under wine works for console paths (`--version`,
   `-M help`) and, since the SDL_Renderer port, for the actual GUI too.
   Wine console output is unreliable — absence of output is not failure;
   check the exit code and `WINEDEBUG=warn+module` for loader errors.
   The definitive test is real Windows (a libvirt Win11 VM with QXL
   works: renderer falls back to SDL's software rasterizer, audio via
   WASAPI; expect reduced speed from the double emulation).

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
