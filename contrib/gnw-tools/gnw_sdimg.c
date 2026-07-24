/*
 * MBR + FAT32 SD card image orchestration -- see gnw_sdimg.h.
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
#include "gnw_sdimg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR_SIZE          512u
#define PARTITION_START_LBA  2048u   /* matches the real dumped card */
#define PARTITION_TYPE       0x0B    /* W95 FAT32 (CHS), same as real card */

static void seterr(char **err, const char *msg)
{
    if (err && !*err) {
        *err = strdup(msg);
    }
}

bool gnw_sdimg_build(gnw_sector_write_fn write, void *opaque,
                     uint64_t total_bytes, const char *content_dir,
                     char **err)
{
    uint8_t mbr[SECTOR_SIZE];
    uint64_t total_sectors, part_sectors;

    if (err) {
        *err = NULL;
    }
    /* QEMU hw/sd/sd.c: total size must be a power of 2 or 512K-aligned. */
    if (total_bytes < 64 * 1024 * 1024 ||
        ((total_bytes & (total_bytes - 1)) != 0 &&
         total_bytes % (512 * 1024) != 0)) {
        seterr(err, "size must be >= 64MiB and a power of 2 "
                    "or a 512KiB multiple (QEMU sd.c requirement)");
        return false;
    }
    total_sectors = total_bytes / SECTOR_SIZE;
    part_sectors = total_sectors - PARTITION_START_LBA;
    if (part_sectors > 0xFFFFFFFFu) {
        seterr(err, "partition exceeds MBR 32-bit sector count (max 2TiB)");
        return false;
    }

    memset(mbr, 0, sizeof(mbr));
    /* Bytes 0-445: bootstrap code, left zeroed (never booted from). */
    uint8_t *e = mbr + 446;
    e[0] = 0x00;                                 /* not bootable */
    /* CHS start/end: standard large-disk overflow sentinel; LBA rules. */
    e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;
    e[4] = PARTITION_TYPE;
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
    e[8] = PARTITION_START_LBA & 0xFF;
    e[9] = (PARTITION_START_LBA >> 8) & 0xFF;
    e[10] = (PARTITION_START_LBA >> 16) & 0xFF;
    e[11] = (PARTITION_START_LBA >> 24) & 0xFF;
    e[12] = part_sectors & 0xFF;
    e[13] = (part_sectors >> 8) & 0xFF;
    e[14] = (part_sectors >> 16) & 0xFF;
    e[15] = (part_sectors >> 24) & 0xFF;
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    if (write(opaque, 0, mbr, 1) < 0) {
        seterr(err, "MBR write failed");
        return false;
    }
    /* LBA 1..2047: unpartitioned gap, left as zeros. */

    return gnw_fat32_build(write, opaque, PARTITION_START_LBA, part_sectors,
                           PARTITION_START_LBA, "GNW SD", content_dir, err);
}
