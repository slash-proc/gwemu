#!/usr/bin/env python3
"""Copy qemu-system-arm to the gwemu alias binary (see meson.build).

On Windows the copy is additionally flipped from console-subsystem to
GUI-subsystem: gwemu.exe is a byte-copy of qemu-system-arm.exe, and QEMU
links console PEs, so double-clicking the alias opened a console window
alongside the GUI. qemu-system-arm.exe itself deliberately stays a
console app (query invocations like -M help print to stdout).
"""
import shutil
import struct
import sys

IMAGE_SUBSYSTEM_WINDOWS_GUI = 2

src, dst = sys.argv[1], sys.argv[2]
shutil.copy2(src, dst)

if dst.endswith(".exe"):
    with open(dst, "r+b") as f:
        f.seek(0x3C)
        (pe_off,) = struct.unpack("<I", f.read(4))
        f.seek(pe_off)
        if f.read(4) != b"PE\0\0":
            sys.exit(f"{dst}: not a PE image")
        # COFF header (20 bytes) follows the signature; Subsystem is at
        # offset 68 in the optional header for both PE32 and PE32+.
        f.seek(pe_off + 4 + 20 + 68)
        f.write(struct.pack("<H", IMAGE_SUBSYSTEM_WINDOWS_GUI))
