/*
 * MBR + FAT32 SD card image orchestration -- C port of
 * scripts/make_sdcard_image.py's disk-assembly half (partition layout
 * matching the real, physically-dumped Game & Watch SD card: MBR, one
 * partition, type 0x0B, start LBA 2048). See gnw_fat32.h for the volume
 * half and the sector-write callback contract.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef GNW_SDIMG_H
#define GNW_SDIMG_H

#include "gnw_fat32.h"

/*
 * Build a complete SD card image of `total_bytes` through the sector-write
 * callback: MBR at LBA 0 (single 0x0B partition at LBA 2048), FAT32
 * volume (label "GNW SD") filling the rest, populated from `content_dir`
 * if non-NULL. `total_bytes` must be a power of 2 or a 512 KiB multiple
 * (QEMU's hw/sd/sd.c requirement) and >= 64 MiB. The target must read
 * back as zeros where never written.
 *
 * On failure returns false and, if err is non-NULL, sets *err to a
 * malloc'd caller-owned message (free() it).
 */
bool gnw_sdimg_build(gnw_sector_write_fn write, void *opaque,
                     uint64_t total_bytes, const char *content_dir,
                     char **err);

#endif
