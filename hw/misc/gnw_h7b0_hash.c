/*
 * STM32H7B0 HASH processor (Nintendo Game & Watch)
 *
 * See gnw_h7b0_hash.h for scope/rationale. This is a real (if
 * synchronous/instant) MD5/SHA-1/SHA-224/SHA-256 engine, plain and
 * HMAC, backed by QEMU's own crypto/hash.h and crypto/hmac.h -- not a
 * byte-serial reimplementation -- plus a from-scratch SHA-224
 * fallback (see sha256_generic() below) for hosts whose crypto
 * backend doesn't support it (glib's GChecksum has no SHA224 mode).
 *
 * Three data-delivery paths are modeled:
 *   - polled: HAL writes DIN directly, then STR.DCAL; SR.DCIS polled.
 *   - interrupt: HAL enables DINIE/DCIE and feeds DIN entirely from
 *     its own ISR (HAL_HASH_IRQHandler -> HASH_IT()) in response to
 *     our qemu_irq -- this device just needs to keep asserting the
 *     line whenever SR.DINIS/DCIS is set and the matching IMR bit is
 *     enabled (real hardware is level-sensitive here: DINIS/DCIS stay
 *     set until hardware clears them, so re-entering the ISR on every
 *     word is correct, not a bug).
 *   - DMA: HAL's HASH_Start_DMA()/HASH_DMAXferCplt() never write
 *     STR.DCAL themselves for a DMA transfer -- real hardware
 *     auto-triggers digest calculation once the configured NBLW-sized
 *     transfer completes (confirmed by HASH_Finish() only ever polling
 *     SR.DCIS, never writing DCAL). This device's generic DMA
 *     controller (gnw_h7b0_dma.c) has no real bus-access model of its
 *     own -- it only fires a notifier with the stream's M0AR/NDTR at
 *     half/full-transfer time -- so this device does the actual
 *     guest-RAM read itself from that notifier and, on the
 *     full-transfer notification, auto-triggers the same digest/HMAC-
 *     phase-advance logic STR.DCAL would (see hash_trigger_dcal()).
 *
 * HMAC mode (CR.MODE) is a genuine 3-phase hardware sequence (key,
 * message, key again -- see HMAC_Processing()/HASH_IT()'s HMAC_STEP_*
 * handling in stm32h7xx_hal_hash.c): each phase ends with its own
 * DCAL/auto-trigger, but only phase 3's actually computes+signals the
 * final digest (SR.DCIS); phases 1/2 just advance internal state (real
 * hardware clears BUSY almost immediately, which this instant model
 * satisfies for free by never setting BUSY at all).
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

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "crypto/hash.h"
#include "crypto/hmac.h"
#include "hw/core/irq.h"
#include "exec/cpu-common.h"
#include "hw/misc/gnw_h7b0_hash.h"
#include "hw/misc/gnw_h7b0_dma.h"

/* ---------------------------------------------------------------------
 * From-scratch SHA-224/SHA-256 core (RFC 6234), used only for SHA224
 * (plain and HMAC) since QEMU's glib crypto backend reports
 * QCRYPTO_HASH_ALGO_SHA224 as unsupported (no SHA224 mode in GLib's
 * GChecksum API). SHA256 itself always goes through qcrypto_hash_bytes/
 * qcrypto_hmac_bytes -- glib supports that one fine.
 * --------------------------------------------------------------------- */

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define SHA256_ROTR(x, n)  (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_transform(uint32_t h[8], const uint8_t block[64])
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, hh;

    for (int i = 0; i < 16; i++) {
        w[i] = ldl_be_p(block + i * 4);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = SHA256_ROTR(w[i - 15], 7) ^ SHA256_ROTR(w[i - 15], 18) ^
                      (w[i - 15] >> 3);
        uint32_t s1 = SHA256_ROTR(w[i - 2], 17) ^ SHA256_ROTR(w[i - 2], 19) ^
                      (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = h[0]; b = h[1]; c = h[2]; d = h[3];
    e = h[4]; f = h[5]; g = h[6]; hh = h[7];

    for (int i = 0; i < 64; i++) {
        uint32_t s1 = SHA256_ROTR(e, 6) ^ SHA256_ROTR(e, 11) ^ SHA256_ROTR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t temp1 = hh + s1 + ch + sha256_k[i] + w[i];
        uint32_t s0 = SHA256_ROTR(a, 2) ^ SHA256_ROTR(a, 13) ^ SHA256_ROTR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;

        hh = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }

    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

/* Runs the full SHA-256 compression pipeline (padding included) from a
 * caller-supplied initial state, so SHA224's differing IV can share
 * this code -- SHA224 is defined as exactly this algorithm with a
 * different initial hash value, truncated to 28 bytes of output. */
static void sha256_generic(const uint32_t iv[8], const uint8_t *data,
                            size_t len, uint32_t out[8])
{
    uint32_t h[8];
    size_t full_blocks = len / 64;
    size_t rem = len % 64;
    uint8_t tail[128];
    size_t tail_len;

    memcpy(h, iv, sizeof(h));

    for (size_t i = 0; i < full_blocks; i++) {
        sha256_transform(h, data + i * 64);
    }

    /* Padding: 0x80, zeros, 64-bit big-endian bit length -- fits in
     * one or two more blocks depending on how much tail remains. */
    memcpy(tail, data + full_blocks * 64, rem);
    tail_len = rem;
    tail[tail_len++] = 0x80;
    if (tail_len > 56) {
        while (tail_len < 64) {
            tail[tail_len++] = 0;
        }
        sha256_transform(h, tail);
        tail_len = 0;
    }
    while (tail_len < 56) {
        tail[tail_len++] = 0;
    }
    uint64_t bitlen = (uint64_t)len * 8;
    stq_be_p(tail + 56, bitlen);
    sha256_transform(h, tail);

    memcpy(out, h, sizeof(h));
}

static const uint32_t sha224_iv[8] = {
    0xc1059ed8, 0x367cd507, 0x3070dd17, 0xf70e5939,
    0xffc00b31, 0x68581511, 0x64f98fa7, 0xbefa4fa4,
};

static void sha224_digest(const uint8_t *data, size_t len, uint8_t out[28])
{
    uint32_t h[8];

    sha256_generic(sha224_iv, data, len, h);
    for (int i = 0; i < 7; i++) {
        stl_be_p(out + i * 4, h[i]);
    }
}

/* Generic HMAC construction (RFC 2104) built on sha224_digest(), for
 * hosts where the crypto backend's qcrypto_hmac_bytes() doesn't
 * support SHA224 either (same GLib limitation as the plain hash). */
static void hmac_sha224(const uint8_t *key, size_t keylen,
                         const uint8_t *msg, size_t msglen,
                         uint8_t out[28])
{
    uint8_t k[64];
    uint8_t ipad[64], opad[64];
    uint8_t inner_digest[28];
    g_autofree uint8_t *inner_input = NULL;
    g_autofree uint8_t *outer_input = NULL;

    memset(k, 0, sizeof(k));
    if (keylen > sizeof(k)) {
        sha224_digest(key, keylen, k); /* first 28 bytes; rest stays 0 */
    } else {
        memcpy(k, key, keylen);
    }

    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }

    inner_input = g_malloc(64 + msglen);
    memcpy(inner_input, ipad, 64);
    memcpy(inner_input + 64, msg, msglen);
    sha224_digest(inner_input, 64 + msglen, inner_digest);

    outer_input = g_malloc(64 + 28);
    memcpy(outer_input, opad, 64);
    memcpy(outer_input + 64, inner_digest, 28);
    sha224_digest(outer_input, 64 + 28, out);
}

/* ---------------------------------------------------------------------
 * Register-level model
 * --------------------------------------------------------------------- */

/* ALGO1:ALGO0 -> QCryptoHashAlgo, matching stm32h7xx_hal_hash.h's
 * HASH_ALGOSELECTION_* (00=SHA1, 01=MD5, 10=SHA224, 11=SHA256). */
static QCryptoHashAlgo hash_algo(const GnwH7B0HashState *s)
{
    bool algo0 = (s->cr & HASH_CR_ALGO0) != 0;
    bool algo1 = (s->cr & HASH_CR_ALGO1) != 0;

    if (algo1 && algo0) {
        return QCRYPTO_HASH_ALGO_SHA256;
    }
    if (algo1) {
        return QCRYPTO_HASH_ALGO_SHA224;
    }
    if (algo0) {
        return QCRYPTO_HASH_ALGO_MD5;
    }
    return QCRYPTO_HASH_ALGO_SHA1;
}

static void hash_update_irq(GnwH7B0HashState *s)
{
    bool pending = (s->sr & s->imr & (HASH_SR_DINIS | HASH_SR_DCIS)) != 0;

    qemu_set_irq(s->irq, pending);
}

static void hash_start_digest(GnwH7B0HashState *s)
{
    s->message_len = 0;
    s->key_len = 0;
    memset(s->digest, 0, sizeof(s->digest));
    s->sr = HASH_SR_RESET_VALUE;
    s->hmac_phase = (s->cr & HASH_CR_MODE) ? 1 : 0;
    hash_update_irq(s);
}

/* Trim a raw accumulated byte count down to the true message length
 * using STR.NBLW (number of valid bits in the last word), matching
 * HASH_GetDigest()'s notion of "how many of the last word's bytes
 * actually count". */
static uint32_t hash_trim_len(const GnwH7B0HashState *s, uint32_t raw_len)
{
    uint32_t nblw = s->str & HASH_STR_NBLW_MASK;
    uint32_t last_word_valid_bytes = (nblw == 0) ? 4 : ((nblw + 7) / 8);
    uint32_t total_bytes = raw_len;

    if (total_bytes >= 4) {
        total_bytes = total_bytes - 4 + last_word_valid_bytes;
    } else {
        total_bytes = last_word_valid_bytes < total_bytes
                       ? last_word_valid_bytes : total_bytes;
    }
    if (total_bytes > raw_len) {
        total_bytes = raw_len;
    }
    return total_bytes;
}

static bool hash_compute_digest(QCryptoHashAlgo alg, const uint8_t *data,
                                 size_t len, uint32_t out[8], uint32_t *nwords)
{
    if (alg == QCRYPTO_HASH_ALGO_SHA224) {
        sha224_digest(data, len, (uint8_t *)out); /* writes 28 bytes = 7 words */
        /* out[] filled big-endian-per-word by sha224_digest already;
         * re-read as words below via the shared byte->word loop, so
         * just stash the raw bytes and let the caller's loop handle it. */
        *nwords = 7;
        return true;
    }

    g_autofree uint8_t *result = NULL;
    size_t resultlen = 0;
    Error *err = NULL;

    if (qcrypto_hash_bytes(alg, (const char *)data, len,
                            &result, &resultlen, &err) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "gnw-h7b0-hash: digest failed: %s\n",
                      err ? error_get_pretty(err) : "unknown error");
        error_free(err);
        return false;
    }

    uint32_t words = resultlen / 4;
    if (words > 8) {
        words = 8;
    }
    for (uint32_t i = 0; i < words; i++) {
        out[i] = ldl_be_p(result + i * 4);
    }
    *nwords = words;
    return true;
}

static bool hash_compute_hmac(QCryptoHashAlgo alg,
                               const uint8_t *key, size_t keylen,
                               const uint8_t *msg, size_t msglen,
                               uint32_t out[8], uint32_t *nwords)
{
    if (alg == QCRYPTO_HASH_ALGO_SHA224) {
        hmac_sha224(key, keylen, msg, msglen, (uint8_t *)out);
        *nwords = 7;
        return true;
    }

    Error *err = NULL;
    g_autoptr(QCryptoHmac) hmac = qcrypto_hmac_new(alg, key, keylen, &err);
    if (!hmac) {
        qemu_log_mask(LOG_GUEST_ERROR, "gnw-h7b0-hash: hmac init failed: %s\n",
                      err ? error_get_pretty(err) : "unknown error");
        error_free(err);
        return false;
    }

    g_autofree uint8_t *result = NULL;
    size_t resultlen = 0;
    if (qcrypto_hmac_bytes(hmac, msg, msglen, &result, &resultlen, &err) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "gnw-h7b0-hash: hmac failed: %s\n",
                      err ? error_get_pretty(err) : "unknown error");
        error_free(err);
        return false;
    }

    uint32_t words = resultlen / 4;
    if (words > 8) {
        words = 8;
    }
    for (uint32_t i = 0; i < words; i++) {
        out[i] = ldl_be_p(result + i * 4);
    }
    *nwords = words;
    return true;
}

/* Store a just-completed digest (words already in the right order for
 * HR0..HRx) into the register file and signal completion.
 *
 * hash_compute_digest()/hash_compute_hmac() write SHA224's raw 28-byte
 * result directly into the `out` buffer as bytes (not pre-split into
 * host-order words), unlike the qcrypto-backed path which already
 * splits into big-endian words -- so callers pass a scratch uint32_t[8]
 * and this function re-derives the words from bytes uniformly via a
 * big-endian reinterpretation for the SHA224 case. To keep this simple
 * and avoid two representations, SHA224's helpers above actually write
 * big-endian bytes straight into the digest[] register array's byte
 * layout, which -- since digest[] is itself read back word-at-a-time --
 * requires the words to be reloaded from those bytes big-endian. Doing
 * this via ldl_be_p keeps a single code path below. */
static void hash_store_digest(GnwH7B0HashState *s, uint32_t words[8],
                               uint32_t nwords, bool is_sha224)
{
    if (is_sha224) {
        /* words[] currently holds raw bytes reinterpreted as a
         * uint32_t[8] scratch area written by sha224_digest()/
         * hmac_sha224() via stl_be_p -- reload each word big-endian
         * from that same memory to get the correct host-order value. */
        uint8_t bytes[32];
        memcpy(bytes, words, sizeof(bytes));
        for (uint32_t i = 0; i < nwords; i++) {
            s->digest[i] = ldl_be_p(bytes + i * 4);
        }
    } else {
        for (uint32_t i = 0; i < nwords && i < 8; i++) {
            s->digest[i] = words[i];
        }
    }

    s->sr |= HASH_SR_DCIS;
    hash_update_irq(s);
}

/* Common finalize path for both STR.DCAL (polled/IT) and DMA
 * auto-trigger (see file header comment) -- handles plain digest
 * completion and each step of the real 3-phase HMAC sequence. */
static void hash_trigger_dcal(GnwH7B0HashState *s)
{
    QCryptoHashAlgo alg = hash_algo(s);
    bool is_hmac = (s->cr & HASH_CR_MODE) != 0;
    bool is_sha224 = (alg == QCRYPTO_HASH_ALGO_SHA224);
    uint32_t words[8];
    uint32_t nwords;

    if (!is_hmac) {
        uint32_t len = hash_trim_len(s, s->message_len);
        if (hash_compute_digest(alg, s->message, len, words, &nwords)) {
            hash_store_digest(s, words, nwords, is_sha224);
        }
        s->message_len = 0;
        return;
    }

    switch (s->hmac_phase) {
    case 1: /* key phase complete */
        s->key_len = hash_trim_len(s, s->key_len);
        s->hmac_phase = 2;
        break;
    case 2: /* message phase complete */
        s->message_len = hash_trim_len(s, s->message_len);
        s->hmac_phase = 3;
        break;
    case 3: /* key re-fed, HMAC complete */
        if (hash_compute_hmac(alg, s->key, s->key_len, s->message,
                               s->message_len, words, &nwords)) {
            hash_store_digest(s, words, nwords, is_sha224);
        }
        s->hmac_phase = 0;
        s->message_len = 0;
        s->key_len = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gnw-h7b0-hash: DCAL outside an HMAC phase\n");
        break;
    }
}

/* Accumulate one 32-bit DIN write into whichever buffer the current
 * mode/phase directs it to, applying CR.DATATYPE's byte-swap. All four
 * DATATYPE values (32B/16B/8B/1B) are handled with real semantics,
 * derived and verified via case_crypto_hash_swap_modes.c's
 * self-consistency cross-check (each non-32B mode hashing buffer B must
 * equal 32B mode hashing a software-pre-transformed copy of B -- see
 * that file's header comment for the full derivation). Every DIN write --
 * full 4-byte-aligned bulk words and the final partial-word tail alike
 * -- is always a 32-bit register store from the HAL's perspective (see
 * HASH_WriteData(): even its 1/2/3-byte tail cases cast up to a
 * uint32_t store); STR.NBLW trims the true valid length at finalize
 * time, so any garbage tail byte read past a short buffer's end is
 * harmless. */
static void hash_din_write(GnwH7B0HashState *s, uint32_t value)
{
    uint32_t datatype = (s->cr & HASH_CR_DATATYPE_MASK) >> HASH_CR_DATATYPE_SHIFT;
    bool is_hmac = (s->cr & HASH_CR_MODE) != 0;

    uint8_t *dest;
    uint32_t *lenp;
    uint32_t cap;

    if (is_hmac && s->hmac_phase == 1) {
        dest = s->key;
        lenp = &s->key_len;
        cap = sizeof(s->key);
    } else if (is_hmac && s->hmac_phase == 3) {
        /* Key re-fed for real hardware's benefit; the true key is
         * already captured from phase 1, so just drop these bytes. */
        return;
    } else {
        dest = s->message;
        lenp = &s->message_len;
        cap = sizeof(s->message);
    }

    if (*lenp + 4 > cap) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gnw-h7b0-hash: message exceeds %u-byte model limit\n",
                      cap);
        return;
    }

    switch (datatype) {
    case 0:
        /* DATATYPE_32B: "no swapping" -- the true stream order the
         * engine sees is the byte-reverse of the native-endian word
         * HASH_WriteData() loaded (see case_crypto_hash_swap_modes.c's
         * header comment for the derivation). */
        stl_be_p(dest + *lenp, value);
        break;
    case 2:
        /* DATATYPE_8B ("all bytes swapped", what gnwmanager/HAL's own
         * usage always configures): plain passthrough in true memory
         * byte order -- the byte-reversal HAL/hardware describes nets
         * out to identity once combined with DATATYPE_32B's own
         * byte-reverse (see derivation in case_crypto_hash_swap_modes.c). */
        stl_le_p(dest + *lenp, value);
        break;
    case 1: {
        /* DATATYPE_16B ("each half word is swapped"): swap the two bytes
         * within each 16-bit half, keeping the halves themselves in
         * place -- i.e. (b0,b1,b2,b3) -> (b1,b0,b3,b2). Derived from
         * case_crypto_hash_swap_modes.c's self-consistency check against
         * DATATYPE_32B's own byte-reverse. */
        uint32_t swapped = ((value & 0x00FF00FFu) << 8) | ((value >> 8) & 0x00FF00FFu);
        stl_le_p(dest + *lenp, swapped);
        break;
    }
    case 3:
    default: {
        /* DATATYPE_1B ("in the word all bits are swapped"): a full
         * 32-bit bit-reversal of the native word, then stored in the
         * same byte-reversed order DATATYPE_32B uses. Derived from
         * case_crypto_hash_swap_modes.c's self-consistency check. */
        uint32_t v = value;
        v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
        v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
        v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
        v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
        v = (v >> 16) | (v << 16);
        stl_be_p(dest + *lenp, v);
        break;
    }
    }
    *lenp += 4;
}

static void gnw_h7b0_hash_reset(DeviceState *dev)
{
    GnwH7B0HashState *s = GNW_H7B0_HASH(dev);

    s->cr = 0;
    s->str = 0;
    s->imr = 0;
    s->sr = HASH_SR_RESET_VALUE;
    memset(s->digest, 0, sizeof(s->digest));
    memset(s->csr, 0, sizeof(s->csr));
    s->message_len = 0;
    s->key_len = 0;
    s->hmac_phase = 0;
    hash_update_irq(s);
}

static uint64_t gnw_h7b0_hash_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0HashState *s = GNW_H7B0_HASH(opaque);

    switch (addr) {
    case HASH_CR:
        return s->cr;
    case HASH_DIN:
        return 0; /* write-only in practice; gnwmanager never reads it back */
    case HASH_STR:
        return s->str;
    case HASH_HR0:
        return s->digest[0];
    case HASH_HR1:
        return s->digest[1];
    case HASH_HR2:
        return s->digest[2];
    case HASH_HR3:
        return s->digest[3];
    case HASH_HR4:
        return s->digest[4];
    case HASH_IMR:
        return s->imr;
    case HASH_SR:
        return s->sr;
    default:
        if (addr >= HASH_CSR_BASE && addr <= HASH_CSR_END) {
            return s->csr[(addr - HASH_CSR_BASE) / 4];
        }
        if (addr >= HASH_DIGEST_BASE && addr <= HASH_DIGEST_END) {
            return s->digest[(addr - HASH_DIGEST_BASE) / 4];
        }
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }
}

static void gnw_h7b0_hash_write(void *opaque, hwaddr addr,
                                 uint64_t val64, unsigned int size)
{
    GnwH7B0HashState *s = GNW_H7B0_HASH(opaque);
    uint32_t value = val64;

    switch (addr) {
    case HASH_CR:
        s->cr = value & ~HASH_CR_INIT; /* INIT is write-only/self-clearing */
        if (value & HASH_CR_INIT) {
            hash_start_digest(s);
        }
        return;
    case HASH_DIN:
        hash_din_write(s, value);
        return;
    case HASH_STR:
        /* STR.DCAL is write-only/self-clearing on real hardware (see
         * STM32H7B0.svd) -- never persist it into s->str. Persisting it
         * was a real bug: a later innocent read-modify-write of STR
         * (e.g. __HAL_HASH_SET_NBVALIDBITS()'s MODIFY_REG, which reads
         * STR back before writing just the NBLW field) would read the
         * stuck DCAL=1 bit back and re-OR it into its own write, causing
         * a spurious extra hash_trigger_dcal() call before any new
         * message/key bytes had been written for that round. Harmless
         * for single-shot plain hashing (the real trigger later in the
         * same call still fires with the correct final byte count,
         * silently overwriting the bogus early digest) but fatal for
         * HMAC's 3-phase state machine (confirmed via live tracing: it
         * advanced hmac_phase 1->2 immediately after CR.INIT, before any
         * key bytes were fed, permanently corrupting the phase
         * sequence) and for DMA/IT flows that do more than one STR
         * read-modify-write per digest. */
        s->str = value & HASH_STR_NBLW_MASK;
        if (value & HASH_STR_DCAL) {
            hash_trigger_dcal(s);
        }
        return;
    case HASH_IMR:
        s->imr = value;
        hash_update_irq(s);
        return;
    case HASH_SR:
        /* DCIS/DINIS are read-write-to-clear on real hardware. */
        s->sr &= ~(value & (HASH_SR_DCIS | HASH_SR_DINIS));
        hash_update_irq(s);
        return;
    default:
        if (addr >= HASH_CSR_BASE && addr <= HASH_CSR_END) {
            s->csr[(addr - HASH_CSR_BASE) / 4] = value;
            return;
        }
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
    }
}

static const MemoryRegionOps gnw_h7b0_hash_ops = {
    .read = gnw_h7b0_hash_read,
    .write = gnw_h7b0_hash_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* DMA1/2-stream notifier for DMAMUX1 request 78 (HASH_IN). Real
 * hardware's DMA engine writes each transferred word straight into
 * DIN; since this SoC's generic DMA controller has no real bus-access
 * model (see gnw_h7b0_dma.c), this device does the guest-RAM read
 * itself at each half/full-transfer notification and feeds the words
 * through the same accumulation path a CPU-driven DIN write would.
 * The half/full split mirrors gnw_h7b0_sai1.c's notifier: `ndtr` is
 * the stream's configured item count (here, 32-bit words, per
 * DMA_PDATAALIGN_WORD/DMA_MDATAALIGN_WORD -- see
 * case_crypto_hash_md5_dma.c's hdma_hash_in.Init), half-transfer
 * covers the first ndtr/2 items, full-transfer the second half. */
static void gnw_h7b0_hash_dma_notify(void *opaque, bool half,
                                     uint32_t m0ar, uint32_t ndtr)
{
    GnwH7B0HashState *s = opaque;

    /* Deliberately NOT gated on CR.DMAE here. HASH_Start_DMA() (see
     * stm32h7xx_hal_hash.c) writes CR.DMAE *after* calling
     * HAL_DMA_Start_IT() -- with this device's low_latency/synchronous
     * DMA completion (see gnw_h7b0_dma.c's low_latency doc), that means
     * this notifier fires and finishes the whole transfer, INCLUDING
     * the auto-DCAL trigger below, before the guest's own CR.DMAE write
     * ever executes. A CR.DMAE check here was reading only leftover
     * state from whatever the *previous* digest/HMAC phase (or even a
     * prior unrelated case) last left CR.DMAE as, causing real,
     * confirmed-via-live-tracing silent no-ops on essentially a coin
     * flip. This notifier only ever fires for a stream this device
     * itself registered against DMAMUX1 request 78 (HASH_IN) -- see
     * gnw_h7b0_dma_set_request_notifier()'s per-request-id binding --
     * so reaching this function at all is already sufficient proof the
     * transfer is legitimately ours; no separate enable check is
     * needed or safe to make here. */

    uint32_t half_words = ndtr / 2;
    uint32_t half_bytes = half_words * 4;
    uint32_t addr = m0ar + (half ? 0 : half_bytes);

    if (half_bytes != 0) {
        g_autofree uint8_t *buf = g_malloc(half_bytes);
        cpu_physical_memory_read(addr, buf, half_bytes);
        for (uint32_t i = 0; i < half_bytes; i += 4) {
            hash_din_write(s, ldl_le_p(buf + i));
        }
    }

    if (!half) {
        /* Full transfer complete: real hardware auto-triggers digest/
         * HMAC-phase-advance here (see file header comment) -- HAL
         * never writes STR.DCAL itself for a DMA-fed transfer. */
        s->cr &= ~HASH_CR_DMAE;
        hash_trigger_dcal(s);
    }
}

void gnw_h7b0_hash_set_dma(GnwH7B0HashState *s, GnwH7B0DmaState *dma)
{
    if (dma) {
        /* low_latency=true: HASH_IN is a bulk, one-shot transfer with
         * no perceptual pacing need (unlike SAI1 audio) -- see
         * gnw_h7b0_dma_set_request_notifier()'s parameter doc for why
         * the generic controller's default audio-paced timing model
         * caused real, intermittent hash_*_dma/hmac_*_dma failures. */
        gnw_h7b0_dma_set_request_notifier(dma, GNW_H7B0_HASH_DMA_REQUEST,
                                           gnw_h7b0_hash_dma_notify, s,
                                           NULL, NULL, true);
    }
}

static void gnw_h7b0_hash_init(Object *obj)
{
    GnwH7B0HashState *s = GNW_H7B0_HASH(obj);

    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_hash_ops, s,
                           TYPE_GNW_H7B0_HASH, GNW_H7B0_HASH_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription vmstate_gnw_h7b0_hash = {
    .name = TYPE_GNW_H7B0_HASH,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cr, GnwH7B0HashState),
        VMSTATE_UINT32(str, GnwH7B0HashState),
        VMSTATE_UINT32(imr, GnwH7B0HashState),
        VMSTATE_UINT32(sr, GnwH7B0HashState),
        VMSTATE_UINT32_ARRAY(digest, GnwH7B0HashState, 8),
        VMSTATE_UINT32_ARRAY(csr, GnwH7B0HashState,
                              (HASH_CSR_END - HASH_CSR_BASE) / 4 + 1),
        VMSTATE_UINT32(message_len, GnwH7B0HashState),
        VMSTATE_UINT8_ARRAY(message, GnwH7B0HashState, HASH_MAX_MESSAGE_BYTES),
        VMSTATE_UINT32(hmac_phase, GnwH7B0HashState),
        VMSTATE_UINT32(key_len, GnwH7B0HashState),
        VMSTATE_UINT8_ARRAY(key, GnwH7B0HashState, HASH_MAX_KEY_BYTES),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_hash_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_hash;
    device_class_set_legacy_reset(dc, gnw_h7b0_hash_reset);
}

static const TypeInfo gnw_h7b0_hash_info = {
    .name          = TYPE_GNW_H7B0_HASH,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0HashState),
    .instance_init = gnw_h7b0_hash_init,
    .class_init    = gnw_h7b0_hash_class_init,
};

static void gnw_h7b0_hash_register_types(void)
{
    type_register_static(&gnw_h7b0_hash_info);
}

type_init(gnw_h7b0_hash_register_types)
