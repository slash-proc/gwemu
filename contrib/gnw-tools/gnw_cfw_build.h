/*
 * Library entry point for the CFW patch driver (the logic that used to
 * live only in gnw_make_cfw_images.c's main()) -- lets the GUI build
 * patched bank1/extflash images in-process instead of popen()ing the
 * CLI tool (which the static Windows build cannot rely on at all).
 *
 * All inputs/outputs are explicit paths; no repo-layout assumptions.
 * See gnw_make_cfw_images.c for the thin CLI wrapper that maps the
 * historical backup/-relative layout onto this.
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
#ifndef GNW_CFW_BUILD_H
#define GNW_CFW_BUILD_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Result sizes of the last successful build (bytes actually used before
 * 0xFF padding) -- what the CLI historically printed and the GUI parses
 * for its extflash geometry display.
 */
typedef struct GnwCfwBuildResult {
    size_t bank1_used;
    size_t extflash_used;
} GnwCfwBuildResult;

/*
 * Build patched CFW images for `game` ("mario" or "zelda").
 *
 *   internal_path  stock internal flash dump (bank1 OFW)
 *   external_path  stock extflash dump
 *   patch_path     gnwmanager's pre-built novel-code binary for this
 *                  game (the 0x08032000.bin from cli/gnw_patch/binaries)
 *   out_bank1      output, 0xFF-padded to the 256K flash bank size
 *   out_extflash   output, 0xFF-padded to the 64M extflash size
 *   result         optional (may be NULL): used-byte counts
 *   error_msg      on failure: malloc'd message, caller frees
 *
 * Matches gnwmanager's bootloader=True patch flow exactly (see the
 * ordering comments inside -- rwdata MUST be parsed before the novel
 * code splice).
 */
bool gnw_cfw_build_images(const char *game,
                          const char *internal_path,
                          const char *external_path,
                          const char *patch_path,
                          const char *out_bank1,
                          const char *out_extflash,
                          GnwCfwBuildResult *result,
                          char **error_msg);

#ifdef __cplusplus
}
#endif

#endif
