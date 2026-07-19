/*
 * Byte-exact LZMA1 compressor for the (in-progress) C port of the gnwmanager
 * CFW patch pipeline -- matches gnwmanager's gnwmanager/cli/gnw_patch/
 * compression.py::lzma_compress() (remove-keystone-engine branch) exactly:
 * that function calls Python's lzma.compress(data, format=FORMAT_ALONE,
 * filters=[{id: FILTER_LZMA1, preset: 6, dict_size: 16*1024}]) and then
 * strips the 13-byte FORMAT_ALONE header, leaving the raw LZMA1 stream
 * (with embedded end-of-stream marker, since FORMAT_ALONE's header encodes
 * an "unknown" uncompressed size when the payload is unbounded in this
 * calling convention -- verified empirically, not assumed).
 *
 * This is NOT a reimplementation: it links real liblzma (the same library
 * Python's own lzma module wraps) via its raw-buffer encoder API with the
 * matching filter/preset/dict_size options, confirmed byte-for-byte
 * identical to the Python original across representative test inputs
 * (empty, random, repetitive, and edge sizes) before being trusted here.
 * Relocation/layout decisions in the patch pipeline depend on exact
 * compressed lengths, so this must never drift from the Python original's
 * output -- do not "optimize" the options away from what's documented here.
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
#ifndef GNW_LZMA_H
#define GNW_LZMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Compresses `in`/`in_len` to the same raw LZMA1 byte stream gnwmanager's
 * lzma_compress() produces. On success, returns true and sets `out`/
 * `out_len` to a malloc'd (caller frees) buffer. On failure, returns false
 * and (if error_msg is non-NULL) sets `error_msg` to a malloc'd (caller
 * frees) human-readable message.
 */
bool gnw_lzma_compress(const uint8_t *in, size_t in_len,
                        uint8_t **out, size_t *out_len, char **error_msg);

#endif
