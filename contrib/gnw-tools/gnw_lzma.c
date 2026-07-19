/*
 * See gnw_lzma.h for scope/rationale.
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
#include "gnw_lzma.h"

/*
 * Links the host's system liblzma dynamically rather than vendoring/
 * statically bundling a specific version -- a deliberate, considered
 * choice, not an oversight. gnw-web-builder (this repo's TypeScript sibling
 * implementation of this exact patch pipeline) compiles a *pinned* liblzma
 * (XZ Utils 5.4.1) to WASM instead, specifically to avoid 5.6.0/5.6.1 (the
 * well-known xz-utils supply-chain backdoor, CVE-2024-3094). For a native
 * Linux build like this one, we accept inheriting whatever liblzma the
 * host's package manager provides -- standard practice for a native build
 * consuming a system library, and every mainstream distro's package
 * manager already excludes/reverted the compromised 5.6.0/5.6.1 releases
 * -- rather than vendor a pinned static copy the way the WASM build needs
 * to. Revisit this if that calculus ever changes (e.g. if this code starts
 * targeting environments without a trustworthy system package manager).
 */
#include <lzma.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool gnw_lzma_compress(const uint8_t *in, size_t in_len,
                        uint8_t **out, size_t *out_len, char **error_msg)
{
    lzma_options_lzma opt;
    if (lzma_lzma_preset(&opt, 6)) {
        if (error_msg) {
            *error_msg = strdup("gnw_lzma_compress: lzma_lzma_preset(6) failed");
        }
        return false;
    }
    opt.dict_size = 16 * 1024;

    lzma_filter filters[2] = {
        { .id = LZMA_FILTER_LZMA1, .options = &opt },
        { .id = LZMA_VLI_UNKNOWN,  .options = NULL },
    };

    /* Worst-case LZMA1 expansion is small, but give plenty of headroom --
     * this is a one-shot buffer encode, not a stream, so the destination
     * must be sized up front. */
    size_t cap = in_len + (in_len / 2) + 4096;
    uint8_t *buf = malloc(cap);
    if (!buf) {
        if (error_msg) {
            *error_msg = strdup("gnw_lzma_compress: out of memory");
        }
        return false;
    }

    size_t out_pos = 0;
    lzma_ret ret = lzma_raw_buffer_encode(filters, NULL, in, in_len,
                                           buf, &out_pos, cap);
    if (ret != LZMA_OK) {
        free(buf);
        if (error_msg) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "gnw_lzma_compress: lzma_raw_buffer_encode failed (%d)", ret);
            *error_msg = strdup(msg);
        }
        return false;
    }

    *out = buf;
    *out_len = out_pos;
    return true;
}
