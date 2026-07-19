/*
 * Minimal AES-128 single-block encryption for the (in-progress) C port of
 * the gnwmanager CFW patch pipeline -- used by ExtFirmware.crypt()'s OTFDEC
 * keystream generation (gnwmanager/cli/gnw_patch/firmware.py: encrypts a
 * counter block with AES-128-ECB, then XORs it into the extflash data,
 * i.e. AES-CTR built from a raw block-encrypt primitive). Standard
 * FIPS-197 algorithm, encrypt-only (no decrypt, no modes beyond what the
 * caller builds on top) -- ported from gnw-web-builder's aes.ts (itself
 * validated against the FIPS-197 published test vector), re-verified here
 * against that same vector plus real key/nonce material from this
 * project's own OTFDEC work (hw/misc/gnw_h7b0_otfdec.c already implements
 * the guest-side AES-128-CTR decryption this is the patch-side encrypt
 * counterpart of).
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
#ifndef GNW_AES128_H
#define GNW_AES128_H

#include <stdint.h>

typedef struct {
    uint8_t round_keys[176]; /* 11 round keys */
} GnwAes128Ctx;

void gnw_aes128_init(GnwAes128Ctx *ctx, const uint8_t key[16]);
void gnw_aes128_encrypt_block(const GnwAes128Ctx *ctx, const uint8_t in[16], uint8_t out[16]);

#endif
