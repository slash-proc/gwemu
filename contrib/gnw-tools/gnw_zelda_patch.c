/*
 * Zelda patch sequence -- C translation of gnw-web-builder's zelda.ts
 * (verified port of gnwmanager's cli/gnw_patch/zelda.py). Same
 * generation/verification approach as gnw_mario_patch.c -- see that
 * file's header comment.
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
#include "gnw_cfw_engine.h"
#include "gnw_zelda_patch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) do { (void)(cond); if (err) { *out_err = err; return false; } } while (0)

static bool disable_save_encryption(GnwDevice *d, char **out_err)
{
    char *err = NULL;
    CHECK(gnw_fw_nop(&d->internal, 0xf222, 1, &err));
    CHECK(gnw_fw_asm(&d->internal, 0xf228, "add.w r2,r1,#0x10", &err));
    CHECK(gnw_fw_asm(&d->internal, 0xf22c, "sub.w r1,r8,#0x10", &err));
    CHECK(gnw_fw_b(&d->internal, 0x13ed8, 0x13f06, &err));
    CHECK(gnw_fw_asm(&d->internal, 0xb5c4, "mov r1,r2", &err));
    CHECK(gnw_fw_nop(&d->internal, 0xb5c6, 1, &err));
    CHECK(gnw_fw_nop(&d->internal, 0xb5cc, 1, &err));
    CHECK(gnw_fw_asm(&d->internal, 0xf12c, "add.w r7,r0,#0x10", &err));
    CHECK(gnw_fw_asm(&d->internal, 0xf130, "mov   r5,r1", &err));
    CHECK(gnw_fw_asm(&d->internal, 0xf132, "sub.w r6,r2,#0x10", &err));
    CHECK(gnw_fw_asm(&d->internal, 0xf136, "sub   sp,#0x10", &err));
    CHECK(gnw_fw_asm(&d->internal, 0xf138, "mov   r1,r6", &err));
    CHECK(gnw_fw_asm(&d->internal, 0xf13a, "mov   r0,r7", &err));
    { uint8_t b[]={0xf4, 0xf7, 0xbc, 0xfc}; CHECK(gnw_fw_replace_bytes(&d->internal,0xf13c,b,4,&err)); }
    CHECK(gnw_fw_asm(&d->internal, 0xf140, "mov   r2,r7", &err));
    CHECK(gnw_fw_asm(&d->internal, 0xf142, "mov   r1,r6", &err));
    CHECK(gnw_fw_asm(&d->internal, 0xf144, "mov   r0,r5", &err));
    { uint8_t b[]={0xfc, 0xf7, 0x29, 0xfc}; CHECK(gnw_fw_replace_bytes(&d->internal,0xf146,b,4,&err)); }
    CHECK(gnw_fw_b(&d->internal, 0xf14a, 0xf172, &err));
    CHECK(gnw_fw_b(&d->internal, 0x13f52, 0x13f94, &err));
    CHECK(gnw_fw_asm(&d->internal, 0xb528, "mov r7,r0", &err));
    CHECK(gnw_fw_nop(&d->internal, 0xb52a, 1, &err));
    { uint8_t b[]={0xc0, 0xb1}; CHECK(gnw_fw_replace_bytes(&d->internal,0xb54c,b,2,&err)); }
    return true;
}

static bool erase_savedata(GnwDevice *d, char **out_err)
{
    char *err = NULL;
    CHECK(gnw_fw_set_range(&d->external, 0x0000, 0x12000, 0xff, &err));
    CHECK(gnw_fw_set_range(&d->external, 0x3e8000, 0x3f0000, 0xff, &err));
    return true;
}

bool gnw_zelda_patch(GnwDevice *d, char **out_err)
{
    char *err = NULL;

    /* const bWMemcpyInflate = "b.w #" + hex(0xfffffffe & this.internal.address("memcpy_inflate")); */
    uint32_t memcpy_inflate_addr;
    CHECK(gnw_fw_address(&d->internal, "memcpy_inflate", false, &memcpy_inflate_addr, &err));
    char b_w_memcpy_inflate[64];
    snprintf(b_w_memcpy_inflate, sizeof(b_w_memcpy_inflate), "b.w #0x%x", 0xfffffffeu & memcpy_inflate_addr);

    if (!erase_savedata(d, out_err)) return false;
    if (!disable_save_encryption(d, out_err)) return false;

    CHECK(gnw_fw_replace_symbol(&d->internal, 0x4, "bootloader", &err));
    CHECK(gnw_fw_bl_symbol(&d->internal, 0xfe54, "read_buttons", &err));

    CHECK(gnw_fw_nop(&d->internal, 0xebd0, 1, &err));
    CHECK(gnw_fw_b(&d->internal, 0xeaa0, 0xeac2, &err));

    CHECK(gnw_fw_nop(&d->internal, 0x16536, 2, &err));
    CHECK(gnw_fw_nop(&d->internal, 0x1653a, 1, &err));
    CHECK(gnw_fw_nop(&d->internal, 0x1653c, 1, &err));

    if (d->no_hour_tune) { d->external.data[0x320025] = 0xe0; }
    if (d->no_second_beep) { CHECK(gnw_fw_nop(&d->external, 0x32002e, 1, &err)); }

    size_t compressedLen = gnw_fw_compress(&d->external, 0xd0000, 0x2000, &err); CHECK(!err);
    CHECK(gnw_fw_asm(&d->internal, 0xf430, b_w_memcpy_inflate, &err));
    { uint32_t _r = gnw_device_move_to_int(d, 0xd0000, NULL, 0, compressedLen, (size_t[]){0xfcf8}, 1, &err); CHECK(!err); (void)_r; }

    if (d->no_la) {
        CHECK(gnw_fw_clear_range(&d->external, 0xd2000, 0x1f4c00, &err));
        d->external.data[0x315b54] = 0x00;
        d->external.data[0x315b58] = 0x00;
        d->external.data[0x315b5c] = 0x00;
        d->external.data[0x315b60] = 0x00;
    }

    if (d->no_sleep_images) {
        CHECK(gnw_fw_clear_range(&d->external, 0x1f4c00, 0x288120, &err));
    }

    { size_t _tl = 0; CHECK(gnw_rwdata_write_table_and_data(&d->rwdata, 0x1b070, d->int_pos, &_tl, &err)); d->int_pos += _tl; }

    return true;
}
