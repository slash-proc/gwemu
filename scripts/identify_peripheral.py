#!/usr/bin/env python3
"""Read the handle struct's first field (HAL Instance base address) to
identify which peripheral FUN_0800e45c(param_1=0)'s failing sub-call
(FUN_080112b6) is talking to. DAT_0800e538 holds a pointer to a RAM
array of struct entries; entry stride looks like 8 bytes per the
decompile (iVar1 + param_1*8), and the handle passed to FUN_080112b6 is
at offset +8 within a *different*-strided (4-byte) view of the same
base (DAT_0800e538 + param_1*4), per FUN_0800e45c's own decompile.
Just dump both interpretations' raw bytes and let a human/next step
sort out which field is the Instance pointer.
"""
import struct
import sys
from pathlib import Path
sys.path.insert(0, str(Path.home() / "Nerd" / "git" / "gnwmanager"))
from gnwmanager.ocdbackend.gdb_backend import GDBBackend

DAT_PTR_ADDR = 0x0800e538

qemu = GDBBackend()
qemu.open()
try:
    if qemu._is_running:
        qemu.halt()
    base_bytes = qemu.read_memory(DAT_PTR_ADDR, 4)
    base = struct.unpack("<I", base_bytes)[0]
    print(f"DAT_0800e538 (value stored there) = {base:#010x}")
    # dump first entry (param_1=0): both a 4-stride and 8-stride view, 32 bytes
    chunk = qemu.read_memory(base, 64)
    print("first 64 bytes at that pointer:")
    for i in range(0, 64, 4):
        print(f"  +{i:#04x}: {chunk[i:i+4].hex()} = {struct.unpack('<I', chunk[i:i+4])[0]:#010x}")
finally:
    qemu.close()
