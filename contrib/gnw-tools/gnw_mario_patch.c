/*
 * Mario patch sequence -- C translation of gnw-web-builder's mario.ts
 * (itself a verified 1:1 port of gnwmanager's cli/gnw_patch/mario.py,
 * remove-keystone-engine branch), against this project's gnw_cfw_engine.
 *
 * Generated mechanically (see /tmp/gnwc2/translate.py, a throwaway
 * translator, for the bulk of the simple magic-offset calls) with the
 * control-flow constructs (if/else, for loops, try/catch-equivalent
 * fallback) hand-spliced in and checked line-by-line against mario.ts,
 * specifically to avoid the transcription-error risk of retyping ~220
 * lines of hex constants by hand. Every CHECK() aborts with the first
 * error on failure, matching Python's raise-on-first-error semantics.
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
#include "gnw_mario_patch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RWDATA_DTCM_IDX 1

#define CHECK(cond) do { (void)(cond); if (err) { *out_err = err; return false; } } while (0)

static uint32_t round_down_word(uint32_t v) { return (v / 4) * 4; }
static uint32_t round_up_page(uint32_t v) { return ((v + 4095) / 4096) * 4096; }

static bool gnw_ext_shorten(GnwDevice *d, uint32_t amount, char **err)
{
    if (amount == 0) return true;
    if (d->external.len < amount) { d->external.len = 0; }
    else { d->external.len -= amount; }
    (void)err;
    return true;
}

bool gnw_mario_patch(GnwDevice *d, char **out_err)
{
    char *err = NULL;

    CHECK(gnw_fw_replace_symbol(&d->internal, 0x4, "bootloader", &err));
    CHECK(gnw_fw_bl_symbol(&d->internal, 0x6b52, "read_buttons", &err));

    CHECK(gnw_fw_nop(&d->internal, 0x6038, 1, &err));
    CHECK(gnw_fw_b(&d->internal, 0x5f08, 0x5f2a, &err));

    CHECK(gnw_fw_asm(&d->internal, 0x49e0, "mov.w r1, #0x00000", &err));

    if (d->sleep_time) {
        int frames = (int)(60 * d->sleep_time + 0.5); /* secondsToFrames: Math.round(60*s) */
        char ab[64];
        snprintf(ab, sizeof(ab), "movw r2, #%d", frames);
        CHECK(gnw_fw_asm(&d->internal, 0x6c3c, ab, &err));
    }
    if (d->disable_sleep) {
        CHECK(gnw_fw_replace_int(&d->internal, 0x6c40, 0x91, 1, &err));
    }

    CHECK(gnw_fw_nop(&d->internal, 0x10688, 2, &err));
    CHECK(gnw_fw_nop(&d->internal, 0x1068e, 1, &err));

    size_t compressedLen = gnw_fw_compress(&d->external, 0x0, 7772, &err); CHECK(!err);
    CHECK(gnw_fw_bl_symbol(&d->internal, 0x665c, "memcpy_inflate", &err));
    { uint32_t _r = gnw_device_move_ext(d, 0x0, compressedLen, (size_t[]){0x7204}, 1, &err); CHECK(!err); (void)_r; }
    d->ext_offset -= (int32_t)(7776 - round_down_word((uint32_t)compressedLen));

    uint32_t smb1Addr = 0x1e60;
    uint32_t smb1Size = 40960;
    uint32_t patchSmb1Refr; CHECK(gnw_fw_address(&d->internal, "SMB1_ROM", true, &patchSmb1Refr, &err));
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, smb1Addr, smb1Size, (size_t[]){0x7368, 0x10954, 0x7218, patchSmb1Refr}, 4, &err); CHECK(!err); (void)_r; }

    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xbe60, 11620, NULL, 0, &err); CHECK(!err); (void)_r; }

    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xebc4, 528, (size_t[]){0x4154}, 1, &err); CHECK(!err); (void)_r; }
    CHECK((gnw_device_rwdata_lookup_idx(d, RWDATA_DTCM_IDX, 0xebc4, 528, &err), !err));

    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xedd4, 100, (size_t[]){0x4570}, 1, &err); CHECK(!err); (void)_r; }

    {
        static const uint32_t pairs[][2] = { {0xee38, 0x4514}, {0xee78, 0x4518}, {0xeeb8, 0x4520}, {0xeef8, 0x4524} };
        for (int i = 0; i < 4; i++) {
            uint32_t _r = gnw_device_move_to_compressed_memory(d, pairs[i][0], 64, (size_t[]){pairs[i][1]}, 1, &err);
            CHECK(!err); (void)_r;
        }
    }

    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xef38, 128 * 10,
        (size_t[]){0x2ac, 0x2b0, 0x2b4, 0x2b8, 0x2bc, 0x2c0, 0x2c4, 0x2c8, 0x2cc, 0x2d0}, 10, &err); CHECK(!err); (void)_r; }

    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xf438, 96, (size_t[]){0x456c}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xf498, 180, (size_t[]){0x43f8}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xf54c, 1100, (size_t[]){0x43fc}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xf998, 180, (size_t[]){0x4400}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xfa4c, 1136, (size_t[]){0x4404}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xfebc, 864, (size_t[]){0x450c}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x1021c, 384, (size_t[]){0x4510}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x1039c, 384, (size_t[]){0x451c}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x1051c, 384, (size_t[]){0x4410}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x1069c, 384, (size_t[]){0x44f8}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x1081c, 384, (size_t[]){0x4500}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x1099c, 384, (size_t[]){0x4414}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x10b1c, 384, (size_t[]){0x44fc}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x10c9c, 384, (size_t[]){0x4504}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x10e1c, 384, (size_t[]){0x440c}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x10f9c, 384, (size_t[]){0x4408}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x1111c, 192, (size_t[]){0x44f4}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x111dc, 192, (size_t[]){0x4508}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x1129c, 304, (size_t[]){0x458c}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x113cc, 768, (size_t[]){0x4584}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x116cc, 1144, (size_t[]){0x4588}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x11b44, 768, (size_t[]){0x4534}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x11e44, 32, (size_t[]){0x455c}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x11e64, 32, (size_t[]){0x4558}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x11e84, 32, (size_t[]){0x4554}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x11ea4, 32, (size_t[]){0x4560}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x11ec4, 32, (size_t[]){0x4564}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x11ee4, 64, (size_t[]){0x453c}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x11f24, 64, (size_t[]){0x4530}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x11f64, 64, (size_t[]){0x4540}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x11fa4, 64, (size_t[]){0x4544}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x11fe4, 64, (size_t[]){0x4548}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x12024, 64, (size_t[]){0x454c}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x12064, 64, (size_t[]){0x452c}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x120a4, 64, (size_t[]){0x4550}, 1, &err); CHECK(!err); (void)_r; }

    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x120e4, 21 * 96, (size_t[]){0x4574}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x128c4, 192, (size_t[]){0x4578}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x12984, 640, (size_t[]){0x457c}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0x12c04, 320, (size_t[]){0x4538}, 1, &err); CHECK(!err); (void)_r; }

    uint32_t marioSongLen = 0x85e40;
    if (d->no_mario_song) {
        uint8_t *z = calloc(1, 0x85e40);
        CHECK(gnw_fw_replace_bytes(&d->external, 0x12d44, z, marioSongLen, &err));
        CHECK((gnw_device_rwdata_erase_idx(d, RWDATA_DTCM_IDX, 0x12d44, marioSongLen, &err), !err));
        d->ext_offset -= (int32_t)marioSongLen;
        CHECK(gnw_fw_asm(&d->internal, 0x6fc8, "b 0x1c", &err));
    } else {
        uint32_t _r = gnw_device_move_ext(d, 0x12d44, marioSongLen,
            (size_t[]){0x11a00, 0x11a00+4, 0x11a00+8, 0x11a00+12, 0x11a00+16, 0x11a00+20, 0x11a00+24, 0x1199c}, 8, &err);
        CHECK(!err); (void)_r;
        CHECK((gnw_device_rwdata_lookup_idx(d, RWDATA_DTCM_IDX, 0x12d44, marioSongLen, &err), !err));
    }

    compressedLen = gnw_fw_compress(&d->external, 0x98b84, 0x10000, &err); CHECK(!err);
    CHECK(gnw_fw_bl_symbol(&d->internal, 0x678e, "memcpy_inflate", &err));
    { uint32_t _r = gnw_device_move_ext(d, 0x98b84, compressedLen, (size_t[]){0x7350}, 1, &err); CHECK(!err); (void)_r; }
    d->ext_offset -= (int32_t)(0x10000 - round_down_word((uint32_t)compressedLen));

    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xa8b84, 192, (size_t[]){0xb720}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xa8c44, 8352, (size_t[]){0xbc44}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xaace4, 16128, (size_t[]){0xcea8, 0xd2f8}, 2, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xaebe4, 116,
        (size_t[]){0x0d010, 0x0d004, 0x0d2d8, 0x0d2dc, 0x0d2f4, 0x0d2f0}, 6, &err); CHECK(!err); (void)_r; }

    uint32_t smb2Addr = 0xaec58;
    uint32_t smb2Size = 0x10000;
    if (d->no_smb2) {
        uint8_t *z = calloc(1, 0x10000);
        CHECK(gnw_fw_replace_bytes(&d->external, smb2Addr, z, smb2Size, &err));
        d->ext_offset -= (int32_t)smb2Size;
        CHECK(gnw_fw_b(&d->internal, 0x69fc, 0x6a8c, &err));
    } else {
        compressedLen = gnw_fw_compress(&d->external, smb2Addr, smb2Size, &err); CHECK(!err);
        CHECK(gnw_fw_bl_symbol(&d->internal, 0x6a12, "memcpy_inflate", &err));
        { uint32_t _r = gnw_device_move_to_compressed_memory(d, smb2Addr, compressedLen, (size_t[]){0x7374}, 1, &err); CHECK(!err); (void)_r; }
        d->ext_offset -= (int32_t)(smb2Size - round_down_word((uint32_t)compressedLen));
        uint32_t padded = round_up_page((uint32_t)compressedLen);
        { char ab[64]; snprintf(ab, sizeof(ab), "mov.w r2, #%u", padded); CHECK(gnw_fw_asm(&d->internal, 0x6a0a, ab, &err)); }
        { char ab[64]; snprintf(ab, sizeof(ab), "mov.w r3, #%u", padded); CHECK(gnw_fw_asm(&d->internal, 0x6a1e, ab, &err)); }
    }

    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xbec58, 8 * 2, (size_t[]){0x10964}, 1, &err); CHECK(!err); (void)_r; }

    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xbec68, 320, NULL, 0, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xbeda8, 320, NULL, 0, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xbeee8, 320, NULL, 0, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xbf028, 320, NULL, 0, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xbf168, 320, NULL, 0, &err); CHECK(!err); (void)_r; }

    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xbf2a8, 45 * 8, NULL, 0, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xbf410, 144, (size_t[]){0x1658c}, 1, &err); CHECK(!err); (void)_r; }

    uint32_t lookupTableStart = 0xbf4a0;
    uint32_t lookupTableEnd = 0xbf838;
    for (uint32_t addr = lookupTableStart; addr < lookupTableEnd; addr += 4) {
        CHECK(gnw_fw_lookup_ref(&d->external, addr, &err));
    }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, lookupTableStart, lookupTableEnd - lookupTableStart, (size_t[]){0xdf88}, 1, &err); CHECK(!err); (void)_r; }

    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xbf838, 280, (size_t[]){0xe8f8, 0xf4ec, 0xf4f8, 0x10098, 0x105b0}, 5, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xbf950, 180, (size_t[]){0xe2e4, 0xf4fc}, 2, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xbfa04, 8, (size_t[]){0x16590}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xbfa0c, 784, (size_t[]){0x10f9c}, 1, &err); CHECK(!err); (void)_r; }

    uint32_t newLoc = gnw_device_move_ext(d, 0xbfd1c, 14244, NULL, 0, &err); CHECK(!err);
    {
        static const uint32_t refs[] = {
            0x00d330, 0x00d310, 0x00d308, 0x00d338, 0x00d348, 0x00d360, 0x00d368, 0x00d388, 0x00d358, 0x00d320,
            0x00d350, 0x00d380, 0x00d378, 0x00d318, 0x00d390, 0x00d370, 0x00d340, 0x00d398, 0x00d328,
        };
        for (size_t i = 0; i < sizeof(refs)/sizeof(refs[0]); i++) {
            CHECK(gnw_fw_lookup_ref(&d->internal, refs[i], &err));
        }
    }
    {
        static const uint32_t refs2[] = { 0xc1174, 0xc313c, 0xc049c, 0xc1178, 0xc220c, 0xc3490, 0xc3498 };
        for (size_t i = 0; i < sizeof(refs2)/sizeof(refs2[0]); i++) {
            uint32_t reference = refs2[i] - 0xbfd1c + newLoc;
            /* try internal.lookupRefs, fall back to external.lookupRefs on failure
             * (matches TS's try{...}catch{...}, which only catches the missing-
             * key error -- our gnw_fw_lookup_ref returns false+err on the same
             * condition, so a false return here is exactly the catch trigger). */
            char *inner_err = NULL;
            if (!gnw_fw_lookup_ref(&d->internal, reference, &inner_err)) {
                free(inner_err);
                CHECK(gnw_fw_lookup_ref(&d->external, reference, &err));
            }
        }
    }

    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xc34c0, 6168, (size_t[]){0x43ec}, 1, &err); CHECK(!err); (void)_r; }
    CHECK((gnw_device_rwdata_lookup_idx(d, RWDATA_DTCM_IDX, 0xc34c0, 6168, &err), !err));
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xc4cd8, 2984, (size_t[]){0x459c}, 1, &err); CHECK(!err); (void)_r; }
    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xc5880, 120, (size_t[]){0x4594}, 1, &err); CHECK(!err); (void)_r; }

    uint32_t totalImageLength = 193568;
    if (d->no_sleep_images) {
        uint8_t *z = calloc(1, 193568);
        CHECK(gnw_fw_replace_bytes(&d->external, 0xc58f8, z, totalImageLength, &err));
        static const uint32_t imageRefs[] = { 0x1097c, 0x1097c+4, 0x1097c+8, 0x1097c+12, 0x1097c+16 };
        for (size_t i = 0; i < sizeof(imageRefs)/sizeof(imageRefs[0]); i++) {
            uint8_t z4[4] = {0,0,0,0};
            CHECK(gnw_fw_replace_bytes(&d->internal, imageRefs[i], z4, 4, &err));
        }
        d->ext_offset -= (int32_t)totalImageLength;
    } else {
        uint32_t _r = gnw_device_move_ext(d, 0xc58f8, totalImageLength,
            (size_t[]){0x1097c, 0x1097c+4, 0x1097c+8, 0x1097c+12, 0x1097c+16}, 5, &err);
        CHECK(!err); (void)_r;
    }

    { uint32_t _r = gnw_device_move_to_compressed_memory(d, 0xf4d18, 2880, (size_t[]){0x10960}, 1, &err); CHECK(!err); (void)_r; }

    { uint8_t *z = calloc(1, 34728); CHECK(gnw_fw_replace_bytes(&d->external, 0xf5858, z, 34728, &err)); free(z); }
    d->ext_offset -= 34728;

    if (d->compressed_memory_pos) {
        CHECK(gnw_rwdata_append(&d->rwdata, d->compressed_memory.data, d->compressed_memory_pos, d->compressed_memory.flash_base, &err));
    }

    { size_t _tl = 0; CHECK(gnw_rwdata_write_table_and_data(&d->rwdata, 0x17db4, d->int_pos, &_tl, &err)); d->int_pos += _tl; }

    d->ext_offset = (int32_t)round_up_page((uint32_t)d->ext_offset);

    if (d->no_save) {
        static const uint32_t nops[] = { 0x495e, 0x49a6, 0x49b2 };
        for (size_t i = 0; i < 3; i++) { CHECK(gnw_fw_nop(&d->internal, nops[i], 2, &err)); }
        CHECK(gnw_fw_b(&d->internal, 0x4988, 0x49c0, &err));
        CHECK(gnw_fw_b(&d->internal, 0x48be, 0x4912, &err));
        d->ext_offset -= 8192;
    } else {
        /* `ite ne; movne.w r4, #${hex(0xff000+extOffset)}; moveq.w r4, #${hex(0xfe000+extOffset)}` */
        char ab[160];
        snprintf(ab, sizeof(ab), "ite ne; movne.w r4, #0x%x; moveq.w r4, #0x%x",
                 (unsigned)(0xff000 + d->ext_offset), (unsigned)(0xfe000 + d->ext_offset));
        CHECK(gnw_fw_asm(&d->internal, 0x4856, ab, &err));
        snprintf(ab, sizeof(ab), "ite ne; movne.w r4, #0x%x; moveq.w r4, #0x%x",
                 (unsigned)(0xff000 + d->ext_offset), (unsigned)(0xfe000 + d->ext_offset));
        CHECK(gnw_fw_asm(&d->internal, 0x48c0, ab, &err));
    }

    CHECK(gnw_fw_add(&d->internal, 0x106ec, (uint32_t)d->ext_offset, 4, &err));
    CHECK(gnw_ext_shorten(d, (uint32_t)(d->ext_offset < 0 ? -d->ext_offset : d->ext_offset), &err));

    return true;
}
