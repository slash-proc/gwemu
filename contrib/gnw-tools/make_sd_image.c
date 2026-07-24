/*
 * gnw-make-sd-image -- CLI wrapper around gnw_sdimg_build(): raw MBR +
 * FAT32 SD card image to a file. C replacement for the raw-image half of
 * scripts/make_sdcard_image.py (qcow2 conversion, if wanted, remains a
 * separate `qemu-img convert -O qcow2` step; the eventual GUI will use
 * QEMU's own block layer directly instead).
 *
 * Usage: gnw-make-sd-image <out.img> <size e.g. 4G|8G|512M> [content_dir]
 *
 * Verification run against this implementation (2026-07-24, recorded per
 * task requirement):
 *   - fsck.vfat -n on a 4G image with a nested LFN/8.3 test tree: clean,
 *     no errors or fs modifications needed.
 *   - mdir/mcopy -i out.img@@1M recursive listing + byte-compare of every
 *     file against the source tree: identical (loop-mount not available
 *     without root; mtools used instead).
 *   - Same test tree through scripts/make_sdcard_image.py (pyfatfs
 *     oracle), both file trees extracted via mtools and diffed: same
 *     files, same bytes, same directory structure. Byte-identity of
 *     metadata is a non-goal (pyfatfs mis-sizes its FAT, see gnw_fat32.c).
 *   - MBR partition entry byte-compared against the Python output:
 *     identical. Boot-sector geometry/label/layout fields
 *     (SecPerTrk/NumHeads/HiddSec/SecPerClus/label) match.
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

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int file_sector_write(void *opaque, uint64_t lba, const void *buf,
                             uint32_t count)
{
    int fd = *(int *)opaque;
    size_t len = (size_t)count * 512;
    const uint8_t *p = buf;
    off_t off = (off_t)(lba * 512);
    while (len > 0) {
        ssize_t n = pwrite(fd, p, len, off);
        if (n < 0) {
            return errno ? -errno : -EIO;
        }
        p += n;
        off += n;
        len -= (size_t)n;
    }
    return 0;
}

static uint64_t parse_size(const char *s)
{
    char *end;
    uint64_t v = strtoull(s, &end, 10);
    if (end == s || v == 0) {
        return 0;
    }
    if (!strcasecmp(end, "G")) {
        return v << 30;
    }
    if (!strcasecmp(end, "M")) {
        return v << 20;
    }
    if (*end == '\0') {
        return v;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4) {
        fprintf(stderr,
                "usage: %s <out.img> <size e.g. 4G|8G|512M> [content_dir]\n",
                argv[0]);
        return 2;
    }
    const char *out = argv[1];
    uint64_t total = parse_size(argv[2]);
    const char *content = argc == 4 ? argv[3] : NULL;
    if (total == 0) {
        fprintf(stderr, "error: bad size '%s'\n", argv[2]);
        return 2;
    }

    int fd = open(out, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || ftruncate(fd, (off_t)total) < 0) {
        fprintf(stderr, "error: %s: %s\n", out, strerror(errno));
        return 1;
    }

    char *err = NULL;
    if (!gnw_sdimg_build(file_sector_write, &fd, total, content, &err)) {
        fprintf(stderr, "error: %s\n", err ? err : "unknown failure");
        free(err);
        unlink(out);
        close(fd);
        return 1;
    }
    close(fd);
    printf("[gnw-make-sd-image] done: %s (%" PRIu64 " bytes%s%s)\n",
           out, total, content ? ", content from " : ", empty filesystem",
           content ? content : "");
    return 0;
}
