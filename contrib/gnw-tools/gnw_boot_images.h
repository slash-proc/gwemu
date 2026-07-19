/*
 * Build standardized bank1/bank2/extflash boot images for a Game & Watch
 * title from the raw firmware dumps in backup/ -- C port of
 * scripts/make_boot_images.py, for linking directly into the eventual GUI
 * (see ui/xui/) without a Python dependency.
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
#ifndef GNW_BOOT_IMAGES_H
#define GNW_BOOT_IMAGES_H

#include <stdbool.h>

/*
 * Builds backup/qemu-images/<game>-{bank1,bank2,extflash}.bin from
 * backup/internal_flash_backup_<game>.bin and backup/flash_backup_<game>.bin,
 * 0xFF-padded to FLASH_BANK_SIZE/EXTFLASH_SIZE (read from
 * include/hw/arm/gnw_h7b0_soc.h at repo_root).
 *
 * On failure, returns false and (if error_msg is non-NULL) sets *error_msg
 * to a malloc'd, caller-owned human-readable message (free() it).
 */
bool gnw_make_boot_images(const char *game, const char *repo_root,
                           char **error_msg);

#endif
