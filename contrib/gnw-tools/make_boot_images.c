/*
 * Standalone CLI wrapper for gnw_make_boot_images() -- C port of
 * scripts/make_boot_images.py. See gnw_boot_images.h.
 *
 * Usage: gnw-make-boot-images <game> [repo_root]
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
#include "gnw_boot_images.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s <game> [repo_root]\n", argv[0]);
        return 1;
    }
    const char *game = argv[1];
    const char *repo_root = argc == 3 ? argv[2] : ".";

    char *error_msg = NULL;
    if (!gnw_make_boot_images(game, repo_root, &error_msg)) {
        fprintf(stderr, "%s\n", error_msg ? error_msg : "unknown error");
        free(error_msg);
        return 1;
    }

    printf("Wrote %s/backup/qemu-images/%s-{bank1,bank2,extflash}.bin\n",
           repo_root, game);
    return 0;
}
