# True aarch64 cross toolchain for the Linux arm64 (Raspberry Pi 4/5) build.
#
# The compiler runs NATIVELY on the x86_64 host and emits ARM binaries, so
# a full build takes minutes instead of the ~50 min the older emulated
# (qemu-user binfmt, --platform linux/arm64) recipe needed.
#
# Debian rather than Ubuntu on purpose: Debian serves every architecture
# from the same mirror, so `dpkg --add-architecture arm64` just works,
# whereas Ubuntu splits arm64 onto ports.ubuntu.com and needs hand-edited
# apt sources.
FROM debian:bookworm

ENV DEBIAN_FRONTEND=noninteractive

# Native build tooling (x86_64) -- meson/ninja/cmake/python all run on the
# host CPU; only the compilers target aarch64.
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential ninja-build cmake pkg-config git ca-certificates \
      python3 python3-pip python3-venv python3-yaml python3-setuptools \
      bison flex perl \
    && rm -rf /var/lib/apt/lists/*

# The cross toolchain plus every dev library the build links against,
# installed for the arm64 architecture.
#
# liblzma-dev: contrib/gnw-tools' LZMA compressor hard-errors without it,
#   and the failure surfaces after a wall of SDL3 output so it reads as an
#   SDL3 problem.
# libpng-dev: without it CONFIG_PNG is silently #undef'd and `screendump`
#   refuses to write PNGs -- headless screenshot validation breaks while
#   the build looks fine.
# The libx*/wayland/audio set is SDL3's build dependency list.
RUN dpkg --add-architecture arm64 && apt-get update && apt-get install -y --no-install-recommends \
      crossbuild-essential-arm64 \
      libglib2.0-dev:arm64 libpixman-1-dev:arm64 liblzma-dev:arm64 \
      zlib1g-dev:arm64 libpng-dev:arm64 \
      libx11-dev:arm64 libxext-dev:arm64 libxrandr-dev:arm64 libxi-dev:arm64 \
      libxcursor-dev:arm64 libxfixes-dev:arm64 libxss-dev:arm64 \
      libxkbcommon-dev:arm64 libxkbcommon-x11-dev:arm64 \
      libwayland-dev:arm64 wayland-protocols libdecor-0-dev:arm64 \
      libegl1-mesa-dev:arm64 libgl1-mesa-dev:arm64 libgles2-mesa-dev:arm64 \
      libasound2-dev:arm64 libpulse-dev:arm64 libpipewire-0.3-dev:arm64 \
      libdbus-1-dev:arm64 libudev-dev:arm64 libdrm-dev:arm64 libgbm-dev:arm64 \
    && rm -rf /var/lib/apt/lists/*

# PyYAML for subprojects/genconfig/gen_config.py. It is NOT vendored in
# python/wheels, and configure's pyvenv is built without system site
# packages, so stage a wheel here and install it offline after configure
# (see the runner script) instead of requiring network at build time.
RUN python3 -m pip download --no-cache-dir -d /opt/pywheels PyYAML

# pkg-config must never see the host's x86_64 .pc files -- that produces
# link failures against host libs that look like missing dependencies.
# LIBDIR (not just PATH) is the one that actually excludes the defaults.
ENV PKG_CONFIG_PATH=/usr/lib/aarch64-linux-gnu/pkgconfig \
    PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig \
    PKG_CONFIG_SYSROOT_DIR=/

# The SDL3 subproject is built by meson's cmake module, which generates
# its own CMake toolchain file from configure's config-meson.cross
# ([host_machine] + the aarch64 binaries) -- no hand-written toolchain
# file is needed, and SDL3 correctly finds the :arm64 X11/wayland libs
# through the PKG_CONFIG_LIBDIR above.
COPY build.sh /usr/local/bin/gwemu-arm64-build
RUN chmod +x /usr/local/bin/gwemu-arm64-build

# Containers run as root; without this the build can't read the bind-
# mounted repo's git metadata and gwemu_commit stamps blank into the binary.
RUN git config --global --add safe.directory '*'
