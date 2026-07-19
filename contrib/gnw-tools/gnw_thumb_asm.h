/*
 * Minimal ARM Thumb-2 assembler for the gnw_patch firmware patcher -- C port
 * of gnwmanager's gnwmanager/cli/gnw_patch/thumb_asm.py (remove-keystone-engine
 * branch), itself a dependency-free replacement for keystone-engine. Supports
 * only the handful of Thumb-2 instruction forms emitted by the Mario/Zelda
 * firmware patches:
 *
 *   movw   Rd, #imm16                      -- MOV (immediate), T3
 *   mov.w  Rd, #const  (+ conditional)     -- MOV (immediate), T2 (modified immediate)
 *   mov    Rd, Rm                          -- MOV (register), T1
 *   add.w  Rd, Rn, #const  (+ conditional) -- ADD (immediate), T3 (modified immediate)
 *   sub.w  Rd, Rn, #const  (+ conditional) -- SUB (immediate), T3 (modified immediate)
 *   sub    sp, #imm                        -- SUB (SP minus immediate), T2
 *   ldr.w  Rt, [pc, #imm]                  -- LDR (literal), T2 (signed offset)
 *   b      <target>                        -- B, T2 (narrow)
 *   b.w    #<target>                       -- B, T4 (wide)
 *   it/itt/ite/...                         -- IT block
 *
 * Encodings follow the ARMv7-M Architecture Reference Manual and mirror the
 * Python original byte-for-byte (that original is itself verified against
 * keystone-engine in gnwmanager's tests/test_thumb_asm.py).
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
#ifndef GNW_THUMB_ASM_H
#define GNW_THUMB_ASM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Assembles one or more ';'-separated Thumb-2 instructions (case-insensitive,
 * matching the Python original's code.lower()) starting at `addr` (used to
 * resolve PC-relative branches). On success, returns true and sets `out` /
 * `out_len` to a malloc'd (caller frees) buffer of little-endian machine-code
 * bytes. On failure, returns false and (if error_msg is non-NULL) sets
 * `error_msg` to a malloc'd (caller frees) human-readable message; `out` /
 * `out_len` are left untouched.
 */
bool gnw_thumb_assemble(const char *code, uint32_t addr,
                         uint8_t **out, size_t *out_len, char **error_msg);

/*
 * Encodes a 32-bit constant as a 12-bit Thumb "modified immediate" (the
 * i:imm3:imm8 field consumed by the T2 MOV/ADD/SUB (immediate) encodings).
 * Returns -1 if `value` is not representable (matching keystone/the Python
 * original, which reject such operands rather than widening).
 */
int gnw_thumb_encode_modified_immediate(uint32_t value);

#endif
