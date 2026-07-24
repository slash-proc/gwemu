/*
 * CLI wrapper for gnw_cfw_build_images() -- maps the historical
 * repo-root-relative layout (backup/ dumps, ../gnwmanager patch binary,
 * backup/qemu-images/ outputs) onto the explicit-path library API in
 * gnw_cfw_build.c, which holds the actual patch driver logic.
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
#include "gnw_cfw_build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2 || (strcmp(argv[1], "mario") != 0 && strcmp(argv[1], "zelda") != 0)) {
        fprintf(stderr, "usage: %s <mario|zelda> [repo_root]\n", argv[0]);
        return 1;
    }
    const char *game = argv[1];
    const char *repo_root = argc >= 3 ? argv[2] : ".";

    char internal_path[1024], external_path[1024], patch_path[1024];
    char out_bank1[1024], out_extflash[1024];
    snprintf(internal_path, sizeof(internal_path),
             "%s/backup/internal_flash_backup_%s.bin", repo_root, game);
    snprintf(external_path, sizeof(external_path),
             "%s/backup/flash_backup_%s.bin", repo_root, game);
    snprintf(patch_path, sizeof(patch_path),
             "%s/../gnwmanager/gnwmanager/cli/gnw_patch/binaries/%s/0x08032000.bin",
             repo_root, game);
    snprintf(out_bank1, sizeof(out_bank1),
             "%s/backup/qemu-images/%s-bank1-patched.bin", repo_root, game);
    snprintf(out_extflash, sizeof(out_extflash),
             "%s/backup/qemu-images/%s-extflash-patched.bin", repo_root, game);

    GnwCfwBuildResult res;
    char *err = NULL;
    if (!gnw_cfw_build_images(game, internal_path, external_path, patch_path,
                              out_bank1, out_extflash, &res, &err)) {
        fprintf(stderr, "%s\n", err ? err : "cfw build failed");
        if (err && strstr(err, patch_path)) {
            fprintf(stderr, "note: expects a gnwmanager checkout at ../gnwmanager relative to repo_root\n");
        }
        free(err);
        return 1;
    }

    /* Keep this line format stable: the GUI's size probe parses
     * "extflash-patched.bin (<used>" from it. */
    printf("wrote %s-bank1-patched.bin (%zu bytes used) and %s-extflash-patched.bin (%zu bytes used)\n",
           game, res.bank1_used, game, res.extflash_used);
    return 0;
}
