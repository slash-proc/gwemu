/*
 * See gnw_aes128.h for scope/rationale. Standard FIPS-197 AES-128,
 * textbook table-based implementation (S-box generated at init from
 * GF(2^8) log/antilog tables, not hardcoded, matching the reference port).
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
#include "gnw_aes128.h"
#include <string.h>

static uint8_t sbox[256];
static const uint8_t rcon[10] = { 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36 };
static int sbox_built = 0;

static uint8_t xtime(uint8_t b)
{
    return (uint8_t)((b << 1) ^ ((b & 0x80) ? 0x1b : 0));
}

static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t res = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) res ^= a;
        uint8_t hi = a & 0x80;
        a = (uint8_t)(a << 1);
        if (hi) a ^= 0x1b;
        b >>= 1;
    }
    return res;
}

static void build_sbox(void)
{
    uint8_t exp[256], log[256];
    uint8_t a = 1;
    for (int i = 0; i < 255; i++) {
        exp[i] = a;
        log[a] = (uint8_t)i;
        a ^= xtime(a);
    }
    uint8_t inv[256];
    inv[0] = 0;
    for (int i = 1; i < 256; i++) {
        inv[i] = exp[(255 - log[i]) % 255];
    }
    for (int i = 0; i < 256; i++) {
        uint8_t s = inv[i];
        uint8_t x = s;
        for (int j = 0; j < 4; j++) {
            s = (uint8_t)((s << 1) | (s >> 7));
            x ^= s;
        }
        x ^= 0x63;
        sbox[i] = x;
    }
    sbox_built = 1;
}

void gnw_aes128_init(GnwAes128Ctx *ctx, const uint8_t key[16])
{
    if (!sbox_built) {
        build_sbox();
    }
    memcpy(ctx->round_keys, key, 16);

    int bytes = 16, rcon_idx = 0;
    uint8_t temp[4];
    while (bytes < 176) {
        memcpy(temp, ctx->round_keys + bytes - 4, 4);
        if (bytes % 16 == 0) {
            uint8_t t = temp[0];
            temp[0] = temp[1]; temp[1] = temp[2]; temp[2] = temp[3]; temp[3] = t;
            for (int i = 0; i < 4; i++) temp[i] = sbox[temp[i]];
            temp[0] ^= rcon[rcon_idx++];
        }
        for (int i = 0; i < 4; i++) {
            ctx->round_keys[bytes] = (uint8_t)(ctx->round_keys[bytes - 16] ^ temp[i]);
            bytes++;
        }
    }
}

static void add_round_key(uint8_t s[16], const uint8_t *rk)
{
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
}

static void sub_bytes(uint8_t s[16])
{
    for (int i = 0; i < 16; i++) s[i] = sbox[s[i]];
}

static void shift_rows(uint8_t s[16])
{
    uint8_t t[16];
    memcpy(t, s, 16);
    /* column-major AES state: index = col*4 + row */
    for (int row = 1; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            s[col * 4 + row] = t[((col + row) % 4) * 4 + row];
        }
    }
}

static void mix_columns(uint8_t s[16])
{
    for (int c = 0; c < 4; c++) {
        int i = c * 4;
        uint8_t a0 = s[i], a1 = s[i+1], a2 = s[i+2], a3 = s[i+3];
        s[i]   = (uint8_t)(gmul(a0,2) ^ gmul(a1,3) ^ a2 ^ a3);
        s[i+1] = (uint8_t)(a0 ^ gmul(a1,2) ^ gmul(a2,3) ^ a3);
        s[i+2] = (uint8_t)(a0 ^ a1 ^ gmul(a2,2) ^ gmul(a3,3));
        s[i+3] = (uint8_t)(gmul(a0,3) ^ a1 ^ a2 ^ gmul(a3,2));
    }
}

void gnw_aes128_encrypt_block(const GnwAes128Ctx *ctx, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16];
    memcpy(s, in, 16);
    add_round_key(s, ctx->round_keys + 0);
    for (int round = 1; round < 10; round++) {
        sub_bytes(s);
        shift_rows(s);
        mix_columns(s);
        add_round_key(s, ctx->round_keys + round * 16);
    }
    sub_bytes(s);
    shift_rows(s);
    add_round_key(s, ctx->round_keys + 160);
    memcpy(out, s, 16);
}
