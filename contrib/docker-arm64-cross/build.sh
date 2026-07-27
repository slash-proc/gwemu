#!/bin/bash
# Configure + build gwemu for aarch64 inside the gwemu-arm64-cross image.
# Run from the out-of-tree build dir (see docs/cross-platform-builds.md):
#   docker run --rm -v "$PWD":/src -w /src/build-arm64 gwemu-arm64-cross \
#     gwemu-arm64-build
# Extra args are passed through to ../configure.
set -eux

# --enable-lto: measured on a Raspberry Pi 400 (Cortex-A72), retro-go
# Link's Awakening DX workload, +11.8% guest fps (10.93 -> 12.22) and stock
# LA vCPU duty 56.2% -> 54.7%; the stripped binary also comes out *smaller*
# (133MB vs 141MB) and the build time is unchanged within noise. Enabled only
# for aarch64: on x86-64 hosts LTO is a null result *and* fails to link
# because the system SDL2 and the vendored SDL3 are both present and LTO
# catches their conflicting signatures. This recipe already passes
# --disable-sdl/--disable-gtk, so that conflict cannot arise here.
# Keep all hardening flags: removing them is not additive with LTO (measured
# 12.21 vs 12.22 fps, i.e. nothing).
if [ ! -f build.ninja ]; then
    ../configure --target-list=arm-softmmu \
        --cross-prefix=aarch64-linux-gnu- \
        --disable-sdl --disable-sdl-image --disable-gtk \
        --enable-lto \
        "$@"
fi

# genconfig needs PyYAML and it is not vendored in python/wheels.
pyvenv/bin/python -m pip install --no-index --find-links /opt/pywheels PyYAML \
    >/dev/null 2>&1 || pyvenv/bin/python -m pip install PyYAML

ninja -j"${NINJA_JOBS:-12}" qemu-system-arm gwemu
