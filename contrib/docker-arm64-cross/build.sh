#!/bin/bash
# Configure + build gwemu for aarch64 inside the gwemu-arm64-cross image.
# Run from the out-of-tree build dir (see docs/cross-platform-builds.md):
#   docker run --rm -v "$PWD":/src -w /src/build-arm64 gwemu-arm64-cross \
#     gwemu-arm64-build
# Extra args are passed through to ../configure.
set -eux

if [ ! -f build.ninja ]; then
    ../configure --target-list=arm-softmmu \
        --cross-prefix=aarch64-linux-gnu- \
        --disable-sdl --disable-sdl-image --disable-gtk \
        "$@"
fi

# genconfig needs PyYAML and it is not vendored in python/wheels.
pyvenv/bin/python -m pip install --no-index --find-links /opt/pywheels PyYAML \
    >/dev/null 2>&1 || pyvenv/bin/python -m pip install PyYAML

ninja -j"${NINJA_JOBS:-12}" qemu-system-arm gwemu
