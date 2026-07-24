# Static Windows cross toolchain for the one-file gwemu.exe build.
#
# Base: xemu's MXE-based toolchain image (x86_64-w64-mingw32.static-*
# compilers with STATIC glib/pixman/zlib/pcre2/ffi/iconv -- the same
# stack that produces xemu's single-file xemu.exe). We add the one
# dependency it lacks that this fork hard-requires: static liblzma
# (contrib/gnw-tools' LZMA compressor, required: true in meson).
#
# The sha tag tracks xemu's own build-windows.yml -- bump deliberately.
FROM ghcr.io/xemu-project/xemu-win64-toolchain-gcc:sha-2881edd

ARG XZ_VERSION=5.8.1
# The base image ships no curl/wget (xemu's own workflow apt-installs
# curl at job time too).
RUN apt-get update && apt-get install -qy curl && rm -rf /var/lib/apt/lists/*
RUN set -eux; \
    curl -fsSL -o /tmp/xz.tar.gz \
      "https://github.com/tukaani-project/xz/releases/download/v${XZ_VERSION}/xz-${XZ_VERSION}.tar.gz"; \
    tar -C /tmp -xf /tmp/xz.tar.gz; \
    cd /tmp/xz-${XZ_VERSION}; \
    ./configure --host=x86_64-w64-mingw32.static \
                --prefix=/usr/local/mxe/usr/x86_64-w64-mingw32.static \
                --disable-shared --enable-static --disable-nls \
                --disable-xz --disable-xzdec --disable-lzmadec \
                --disable-lzmainfo --disable-scripts --disable-doc; \
    make -j"$(nproc)"; \
    make install; \
    rm -rf /tmp/xz-${XZ_VERSION} /tmp/xz.tar.gz; \
    x86_64-w64-mingw32.static-pkg-config --modversion liblzma
