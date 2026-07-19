/*
 * LZ77-style decompressor for the (in-progress) C port of the gnwmanager CFW
 * patch pipeline -- C port of gnwmanager's gnwmanager/cli/gnw_patch/
 * compression.py::lz77_decompress() (remove-keystone-engine branch), which
 * decompresses firmware rwdata-init tables. This is a firmware-specific
 * scheme (opcode byte: 2-bit direct-length, 2-bit high-offset, 4-bit
 * pattern-length, with escape extensions), not any standard LZ77 variant --
 * see the Python original's docstring for the exact table/opcode layout
 * this decodes.
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
#ifndef GNW_LZ77_H
#define GNW_LZ77_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Decompresses `in`/`in_len`. On success, returns true and sets `out`/
 * `out_len` to a malloc'd (caller frees) buffer. On failure (malformed
 * input -- e.g. a back-reference past the start of the output), returns
 * false and (if error_msg is non-NULL) sets `error_msg` to a malloc'd
 * (caller frees) human-readable message.
 */
bool gnw_lz77_decompress(const uint8_t *in, size_t in_len,
                          uint8_t **out, size_t *out_len, char **error_msg);

#endif
