#!/usr/bin/env python3
"""
make_sdcard_image.py -- build an MBR + FAT32 SD card disk image from any
local directory tree, for gnw-h7b0's SD card model.

NOTE: this script is now the TEST ORACLE only -- the product path uses the
C port in contrib/gnw-tools/ (gnw_fat32.c/gnw_sdimg.c, CLI
gnw-make-sd-image), verified against this script's output. Keep this
script working for parity testing, but new features belong in the C code.

Dead simple by design: point --content at ANY folder (e.g. a built
game-and-watch-retro-go-sd checkout's sd_content/ directory, or an
extracted release zip) and this generates a working SD image with that
folder's contents at the root of the filesystem. No incremental
add/list/management commands -- regenerate the whole image if the content
changes.

Partition layout matches a real, physically-dumped Game & Watch SD card
exactly (confirmed via `fdisk -l` against backup/qemu-images/sdcard.img):
MBR, one partition, type 0x0b (FAT32 CHS), start LBA 2048. This isn't a
strict firmware requirement -- retro-go's FatFs is built with
FF_MULTI_PARTITION=0 (external/firmware_update/Core/Src/porting/lib/FatFs/
ffconf.h), which auto-detects either a raw/superfloppy FAT32 volume or the
first MBR partition -- but matching the real card's layout exactly avoids
being the one untested variable if something ever doesn't mount.

Construction, deliberately in two independent steps rather than formatting
in place inside the final disk image:
  1. The FAT32 volume is formatted and populated as its own ordinary,
     standalone file (pyfatfs, a pure-Python FAT12/16/32 + VFAT/LFN
     implementation -- no native extensions, listed in
     scripts/requirements.txt) -- an ordinary volume at its own LBA 0, no
     partition-offset awareness needed.
  2. The MBR (512 bytes, hand-built directly -- no library needed) and
     that volume's bytes are concatenated into the final image, volume
     starting at byte offset 1MiB (LBA 2048).
This keeps pyfatfs's job simple (format a normal volume) and the
partitioning job simple (paste bytes at an offset) instead of asking one
tool to do both at once.

IMPORTANT, confirmed by direct testing (see set_bpb_geometry() below):
pyfatfs's mkfs() never populates BPB_SecPerTrk/BPB_NumHeads/BPB_HiddSec
(the boot sector's legacy CHS-geometry and hidden-sector-count fields)
regardless of whether it's formatting at offset 0 or some nonzero offset
-- this is NOT an artifact of formatting in place inside a larger image,
it's simply always the pyfatfs mkfs() output. An independent verification
tool (mtools' `mdir`) flatly refuses to read ANY pyfatfs-formatted volume,
standalone or otherwise, until those fields are populated ("zero number
of heads or sectors"). So this script still sets them explicitly on the
standalone volume before splicing it into the final image -- not a
patch-after-the-fact hack against a live partitioned image, but filling
in real geometry values a byte-for-byte correct FAT32 boot sector should
have anyway (real SD-formatter tools always populate these; BPB_HiddSec
in particular is supposed to record the partition's own start LBA within
the physical disk once a volume lives inside a partition, which the
standalone volume alone can't know without being told).

Usage:
    ./scripts/make_sdcard_image.py --content /path/to/sd_content
    ./scripts/make_sdcard_image.py --content /path/to/sd_content --size 16G
    ./scripts/make_sdcard_image.py --content /path/to/sd_content --out my-sd.img
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

try:
    from pyfatfs.PyFat import PyFat
    from pyfatfs.PyFatFS import PyFatFS
except ImportError:
    sys.exit(
        "error: pyfatfs not importable. Install it: pip install -r scripts/requirements.txt"
    )

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUT = REPO_ROOT / "backup" / "qemu-images" / "sdcard.qcow2"

SECTOR_SIZE = 512
PARTITION_START_LBA = 2048  # matches the real dumped card exactly
PARTITION_OFFSET = PARTITION_START_LBA * SECTOR_SIZE
PARTITION_TYPE = 0x0B  # W95 FAT32 (CHS) -- same as the real card

# Standard capacity presets only -- clean power-of-2 GiB sizes, not an
# attempt to bit-match any particular real card's odd reported capacity.
SIZE_PRESETS = {
    "8G": 8 * 1024**3,
    "16G": 16 * 1024**3,
    "32G": 32 * 1024**3,
}

# Standard SD-formatter geometry defaults (63 sectors/track, 255 heads).
# See the module docstring: pyfatfs never fills these in on its own, on
# any volume, at any offset.
GEOMETRY_SEC_PER_TRK = 63
GEOMETRY_NUM_HEADS = 255


def write_mbr(f, partition_sectors: int):
    mbr = bytearray(SECTOR_SIZE)
    # Bytes 0-445: bootstrap code, left zeroed (unused by any real boot
    # path -- gnw-h7b0's SD device is a plain block device, not booted
    # from directly).
    entry_off = 446
    mbr[entry_off + 0] = 0x00  # not bootable
    # CHS start/end: unused by FatFs (FF_MULTI_PARTITION=0 only reads the
    # LBA/sector-count fields below), filled with the standard
    # "CHS overflow" sentinel modern formatter tools use for large disks
    # (LBA is authoritative).
    mbr[entry_off + 1:entry_off + 4] = b"\xfe\xff\xff"
    mbr[entry_off + 4] = PARTITION_TYPE
    mbr[entry_off + 5:entry_off + 8] = b"\xfe\xff\xff"
    mbr[entry_off + 8:entry_off + 12] = PARTITION_START_LBA.to_bytes(4, "little")
    mbr[entry_off + 12:entry_off + 16] = partition_sectors.to_bytes(4, "little")
    mbr[510:512] = b"\x55\xaa"
    f.write(bytes(mbr))


def set_bpb_geometry(volume_path: Path, hidden_sectors: int):
    """Fill in BPB_SecPerTrk/BPB_NumHeads/BPB_HiddSec on a standalone,
    freshly-mkfs'd FAT32 volume file, in both the primary boot sector and
    its backup copy (BPB_BkBootSec, almost always relative sector 6).
    See the module docstring for why this is needed regardless of how the
    volume was constructed.
    """
    geometry = struct.pack("<HHI", GEOMETRY_SEC_PER_TRK, GEOMETRY_NUM_HEADS, hidden_sectors)
    with open(volume_path, "r+b") as f:
        boot = f.read(512)
        bkboot_sector = struct.unpack("<H", boot[50:52])[0]
        for sector in {0, bkboot_sector}:
            f.seek(sector * SECTOR_SIZE + 24)
            f.write(geometry)


def copy_tree_into_fs(fs: PyFatFS, src: Path):
    for root, _dirs, files in os.walk(src):
        rel = Path(root).relative_to(src)
        fs_dir = "/" + rel.as_posix() if rel != Path(".") else "/"
        if fs_dir != "/":
            fs.makedirs(fs_dir, recreate=True)
        for name in files:
            src_file = Path(root) / name
            fs_path = fs_dir.rstrip("/") + "/" + name
            with open(src_file, "rb") as in_f, fs.openbin(fs_path, "w") as out_f:
                shutil.copyfileobj(in_f, out_f)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--content", required=True, type=Path,
                     help="local directory to copy in as the SD card's root filesystem")
    ap.add_argument("--size", choices=sorted(SIZE_PRESETS), default="8G",
                     help="disk image capacity preset (default: 8G)")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT,
                     help=f"output image path (default: {DEFAULT_OUT.relative_to(REPO_ROOT)})")
    args = ap.parse_args()

    if not args.content.is_dir():
        sys.exit(f"error: --content {args.content} is not a directory")

    total_size = SIZE_PRESETS[args.size]
    partition_size = total_size - PARTITION_OFFSET
    partition_sectors = partition_size // SECTOR_SIZE
    partition_size = partition_sectors * SECTOR_SIZE  # whole-sector exact

    args.out.parent.mkdir(parents=True, exist_ok=True)
    print(f"[make_sdcard_image] target: {args.out} ({args.size} total, "
          f"{partition_size / (1024**3):.2f} GiB partition)")

    # Colocate with --out rather than the system tempdir: the volume file
    # is nearly as large as the final image, and /tmp is commonly a
    # size-limited tmpfs (confirmed hitting ENOSPC there while testing
    # this script against an 8G image on a 16G tmpfs) -- a directory
    # that's already expected to hold multi-GB disk images is a safer
    # default.
    with tempfile.NamedTemporaryFile(dir=args.out.parent, prefix="gnw-sdcard-vol-",
                                      suffix=".img", delete=False) as tf:
        vol_path = Path(tf.name)
    with tempfile.NamedTemporaryFile(dir=args.out.parent, prefix="gnw-sdcard-raw-",
                                      suffix=".img", delete=False) as tf:
        raw_path = Path(tf.name)
    try:
        print("[make_sdcard_image] formatting standalone FAT32 volume...")
        pf = PyFat(offset=0)
        pf.mkfs(str(vol_path), fat_type=PyFat.FAT_TYPE_FAT32, size=partition_size,
                label="GNW SD")
        pf.close()
        set_bpb_geometry(vol_path, hidden_sectors=PARTITION_START_LBA)

        print(f"[make_sdcard_image] copying {args.content} into volume...")
        fs = PyFatFS(str(vol_path), offset=0)
        try:
            copy_tree_into_fs(fs, args.content)
        finally:
            fs.close()

        print("[make_sdcard_image] assembling raw image (MBR + volume)...")
        with open(raw_path, "wb") as out_f:
            write_mbr(out_f, partition_sectors)
            out_f.write(b"\x00" * (PARTITION_OFFSET - SECTOR_SIZE))
            with open(vol_path, "rb") as vol_f:
                shutil.copyfileobj(vol_f, out_f)

        # qcow2, not raw: a freshly-built image is mostly empty (a real
        # 8GiB build with ~230KB of actual content still consumed the full
        # 8.1GB on disk as raw -- zero sparseness), and qcow2's thin
        # allocation only stores the blocks actually written. The old
        # qcow2-*overlay* trick (from real physical SD card testing, to
        # round an odd real-card size up to a QEMU-friendly power of 2)
        # doesn't apply here -- we already generate exact power-of-2 sizes
        # -- but qcow2's sparseness is independently worth having.
        print("[make_sdcard_image] converting to qcow2...")
        args.out.parent.mkdir(parents=True, exist_ok=True)
        # Prefer this repo's own built qemu-img (build/qemu-img) over
        # whatever's on PATH, if it exists -- consistent with how
        # scripts/boot_qemu.sh and the GUI invoke this project's own build
        # output explicitly rather than assuming a system install.
        qemu_img = REPO_ROOT / "build" / "qemu-img"
        qemu_img_cmd = str(qemu_img) if qemu_img.is_file() else "qemu-img"
        subprocess.run(
            [qemu_img_cmd, "convert", "-O", "qcow2", str(raw_path), str(args.out)],
            check=True,
        )
    finally:
        vol_path.unlink(missing_ok=True)
        raw_path.unlink(missing_ok=True)

    print(f"[make_sdcard_image] done: {args.out}")


if __name__ == "__main__":
    main()
