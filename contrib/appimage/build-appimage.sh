#!/usr/bin/env bash
# Build the single-file Linux release artifact (AppImage) from an
# already-built qemu-system-arm/gwemu binary.
#
#   contrib/appimage/build-appimage.sh <built-binary> <output-file>
#
# The AppImage is the same portable-ization the old zip did (binary +
# every non-base-OS shared lib + LD_LIBRARY_PATH launcher) folded into
# one executable file. Runs on any distro with FUSE; kernels without it
# still work via --appimage-extract-and-run.
#
# appimagetool is expected on PATH or at $APPIMAGETOOL.
set -euo pipefail

SRC_BIN="$1"
OUT="$2"
APPIMAGETOOL="${APPIMAGETOOL:-appimagetool}"

APPDIR="$(mktemp -d)/gwemu.AppDir"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib"

cp "$SRC_BIN" "$APPDIR/usr/bin/gwemu"
strip "$APPDIR/usr/bin/gwemu" 2>/dev/null || true

# Same lib-bundling rule as the zip packaging in release.yml: bundle
# everything except the base toolchain/libc libraries that are
# guaranteed present (and must NOT be overridden -- shipping a foreign
# libc breaks the loader).
for lib in $(ldd "$SRC_BIN" | awk '{print $3}' | grep -v '^$'); do
  base=$(basename "$lib")
  case "$base" in
    libc.so*|libm.so*|libpthread.so*|libdl.so*|librt.so*|ld-linux*|libresolv.so*|libutil.so*)
      continue ;;
  esac
  cp -n "$lib" "$APPDIR/usr/lib/" 2>/dev/null || true
done

cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
DIR="$(cd "$(dirname "$0")" && pwd)"
exec env LD_LIBRARY_PATH="$DIR/usr/lib:${LD_LIBRARY_PATH:-}" "$DIR/usr/bin/gwemu" "$@"
EOF
chmod +x "$APPDIR/AppRun"

cat > "$APPDIR/gwemu.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=GWemu
Exec=gwemu
Icon=gwemu
Categories=Game;Emulator;
Terminal=false
EOF

# Minimal placeholder icon (appimagetool requires one; no real gwemu
# icon asset exists yet -- see the window-icon note in ui/gwemu.c).
python3 - "$APPDIR/gwemu.png" <<'EOF'
import struct, zlib, sys
w = h = 256
# flat dark teal square
row = b'\x00' + bytes([0x1f, 0x6f, 0x6a, 0xff]) * w
raw = row * h
def chunk(t, d):
    c = t + d
    return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c))
png = (b'\x89PNG\r\n\x1a\n'
       + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
       + chunk(b'IDAT', zlib.compress(raw))
       + chunk(b'IEND', b''))
open(sys.argv[1], 'wb').write(png)
EOF
ln -sf gwemu.png "$APPDIR/.DirIcon"

ARCH=x86_64 "$APPIMAGETOOL" "$APPDIR" "$OUT"
rm -rf "$(dirname "$APPDIR")"
echo "built: $OUT"
