#!/usr/bin/env python3
"""Resume a halted QEMU gdbstub target and sample PC a few times."""
import sys, time
from pathlib import Path
sys.path.insert(0, str(Path.home() / "Nerd" / "git" / "gnwmanager"))
from gnwmanager.ocdbackend.gdb_backend import GDBBackend

qemu = GDBBackend()
qemu.open()
try:
    if not qemu._is_running:
        qemu.resume()
    for i in range(5):
        time.sleep(1)
        qemu.halt()
        pc = qemu.read_register("pc")
        print(f"t={i+1}s pc={pc:#010x}")
        qemu.resume()
finally:
    qemu.close()
