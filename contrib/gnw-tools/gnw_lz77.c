/*
 * See gnw_lz77.h for scope/rationale. Direct port of gnwmanager's
 * lz77_decompress() -- see that Python function's docstring (compression.py)
 * for the opcode/table layout being decoded here; comments below track its
 * variable names 1:1 to make the correspondence easy to audit.
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
#include "gnw_lz77.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct out_buf {
    uint8_t *data;
    size_t len, cap;
};

static bool out_reserve(struct out_buf *ob, size_t extra)
{
    if (ob->len + extra <= ob->cap) {
        return true;
    }
    size_t new_cap = ob->cap ? ob->cap * 2 : 256;
    while (new_cap < ob->len + extra) {
        new_cap *= 2;
    }
    uint8_t *n = realloc(ob->data, new_cap);
    if (!n) {
        return false;
    }
    ob->data = n;
    ob->cap = new_cap;
    return true;
}

#define FAIL(msg) do { \
    free(ob.data); \
    if (error_msg) { *error_msg = strdup(msg); } \
    return false; \
} while (0)

bool gnw_lz77_decompress(const uint8_t *in, size_t in_len,
                          uint8_t **out, size_t *out_len, char **error_msg)
{
    struct out_buf ob = { NULL, 0, 0 };
    size_t index = 0;

    while (index < in_len) {
        uint8_t opcode = in[index++];

        int direct_len = opcode & 0x03;
        int offset_256 = (opcode >> 2) & 0x03;
        int pattern_len = opcode >> 4;

        if (direct_len == 0) {
            if (index >= in_len) {
                FAIL("gnw_lz77_decompress: truncated (direct_len escape byte)");
            }
            direct_len = in[index++] + 3;
        }
        /* assert direct_len > 0 -- always true here (>=1 or >=3) */
        direct_len -= 1;

        if (pattern_len == 0xF) {
            if (index >= in_len) {
                FAIL("gnw_lz77_decompress: truncated (pattern_len escape byte)");
            }
            pattern_len += in[index++];
        }

        if (index + (size_t)direct_len > in_len) {
            FAIL("gnw_lz77_decompress: truncated (direct copy)");
        }
        if (!out_reserve(&ob, (size_t)direct_len)) {
            FAIL("gnw_lz77_decompress: out of memory");
        }
        memcpy(ob.data + ob.len, in + index, (size_t)direct_len);
        ob.len += (size_t)direct_len;
        index += (size_t)direct_len;

        if (pattern_len > 0) {
            if (index >= in_len) {
                FAIL("gnw_lz77_decompress: truncated (offset_add byte)");
            }
            int offset_add = in[index++];

            if (offset_256 == 0x03) {
                if (index >= in_len) {
                    FAIL("gnw_lz77_decompress: truncated (offset_256 escape byte)");
                }
                offset_256 = in[index++];
            }

            size_t offset = (size_t)offset_add + (size_t)offset_256 * 256;
            if (offset == 0 || offset > ob.len) {
                FAIL("gnw_lz77_decompress: back-reference out of range");
            }
            if (!out_reserve(&ob, (size_t)(pattern_len + 2))) {
                FAIL("gnw_lz77_decompress: out of memory");
            }
            for (int i = 0; i < pattern_len + 2; i++) {
                ob.data[ob.len] = ob.data[ob.len - offset];
                ob.len++;
            }
        }
    }

    *out = ob.data;
    *out_len = ob.len;
    return true;
}
