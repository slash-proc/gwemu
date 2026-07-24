/*
 * FAT32 mkfs + populate-from-directory, all I/O through a caller-supplied
 * sector-write callback -- C port of the FAT32 half of
 * scripts/make_sdcard_image.py (which formatted via pyfatfs), for linking
 * directly into the eventual GUI (see ui/xui/) without a Python dependency.
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
#ifndef GNW_FAT32_H
#define GNW_FAT32_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Random-access sector writer. `lba` is absolute within whatever the
 * caller's opaque target is (the FAT32 code adds the volume's start LBA
 * itself); `count` is the number of 512-byte sectors in `buf`.
 * Return 0 on success, negative errno on failure.
 */
typedef int (*gnw_sector_write_fn)(void *opaque, uint64_t lba,
                                   const void *buf, uint32_t count);

/*
 * Format a FAT32 volume occupying `volume_sectors` 512-byte sectors
 * starting at absolute LBA `volume_lba`, then (if content_dir is non-NULL)
 * recursively copy that directory tree in as the filesystem root.
 *
 * Boot-sector geometry matches what scripts/make_sdcard_image.py produced:
 * SecPerTrk=63, NumHeads=255, BPB_HiddSec=`hidden_sectors` (the partition
 * start LBA), label "GNW SD" padded to 11 chars, FSInfo at sector 1,
 * backup boot sector + backup FSInfo at sectors 6/7, 32 reserved sectors,
 * two FATs. Cluster size follows the fatgen103 size table (the same table
 * pyfatfs used): 4KiB clusters up to 8GiB, 8KiB to 16GiB, 16KiB to 32GiB,
 * 32KiB above.
 *
 * Assumes the underlying target reads back as zeros where never written
 * (a freshly ftruncate()d file or new qcow2 both do) -- untouched data
 * clusters and FAT tail sectors are not explicitly zeroed.
 *
 * On failure returns false and, if err is non-NULL, sets *err to a
 * malloc'd caller-owned message (free() it).
 */
bool gnw_fat32_build(gnw_sector_write_fn write, void *opaque,
                     uint64_t volume_lba, uint64_t volume_sectors,
                     uint32_t hidden_sectors, const char *label,
                     const char *content_dir, char **err);

#endif
