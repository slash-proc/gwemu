/*
 * STM32H7B0 HASH processor (Nintendo Game & Watch)
 *
 * See gnw_h7b0_hash.h for scope/rationale. This is a real (if
 * synchronous/instant) MD5/SHA-1/SHA-224/SHA-256 engine backed by
 * QEMU's own crypto/hash.h -- not a byte-serial reimplementation.
 * gnwmanager only ever drives this via HAL's simplest polling API
 * (HASH_Start(): feed the whole message via DIN, write STR to trigger
 * the digest, poll SR.DCIS, read HRx) -- no DMA, no interrupts, no
 * HMAC, no suspend/resume. Model exactly that path faithfully; leave
 * DMAE/interrupt/CSR context-swap registers as inert storage since
 * nothing here ever exercises them.
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
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "crypto/hash.h"
#include "hw/misc/gnw_h7b0_hash.h"

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

static void hash_start_digest(GnwH7B0HashState *s)
{
    s->message_len = 0;
    memset(s->digest, 0, sizeof(s->digest));
    s->sr = HASH_SR_RESET_VALUE;
}

/* Compute the digest over the accumulated message and latch it into
 * HR0-4 / the extended HASH_HR5-7 slots, matching HASH_GetDigest()'s
 * read order (HR[0..N-1] in big-endian 32-bit words -- see
 * HAL_HASH_GetDigest's __REV() calls, which is what makes a
 * plain big-endian word layout here come back byte-correct on the
 * guest side). */
static void hash_finish_digest(GnwH7B0HashState *s)
{
    uint32_t nblw = s->str & HASH_STR_NBLW_MASK;
    uint32_t last_word_valid_bytes = (nblw == 0) ? 4 : ((nblw + 7) / 8);
    uint32_t total_bytes = s->message_len;

    if (total_bytes >= 4) {
        total_bytes = total_bytes - 4 + last_word_valid_bytes;
    } else {
        total_bytes = last_word_valid_bytes < total_bytes ? last_word_valid_bytes : total_bytes;
    }
    if (total_bytes > s->message_len) {
        total_bytes = s->message_len;
    }

    QCryptoHashAlgo alg = hash_algo(s);
    g_autofree uint8_t *result = NULL;
    size_t resultlen = 0;
    Error *err = NULL;

    if (qcrypto_hash_bytes(alg, (const char *)s->message, total_bytes,
                            &result, &resultlen, &err) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "gnw-h7b0-hash: digest failed: %s\n",
                      err ? error_get_pretty(err) : "unknown error");
        error_free(err);
        return;
    }

    uint32_t nwords = resultlen / 4;
    if (nwords > ARRAY_SIZE(s->digest)) {
        nwords = ARRAY_SIZE(s->digest);
    }
    for (uint32_t i = 0; i < nwords; i++) {
        s->digest[i] = ldl_be_p(result + i * 4);
    }

    s->sr |= HASH_SR_DCIS;
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
        /* Every DIN write -- full 4-byte-aligned bulk words and the final
         * partial-word tail alike -- is always a 32-bit register store from
         * the HAL's perspective (see HASH_WriteData(): even its 1/2/3-byte
         * tail cases cast up to a uint32_t store). Accumulate all 4 bytes
         * unconditionally; STR.NBLW (below) trims the true valid length at
         * finalize time, so any garbage tail byte read past a short buffer's
         * end is harmless. */
        if (s->message_len + 4 <= sizeof(s->message)) {
            stl_le_p(s->message + s->message_len, value);
            s->message_len += 4;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "gnw-h7b0-hash: message exceeds %zu-byte model limit\n",
                          sizeof(s->message));
        }
        return;
    case HASH_STR:
        s->str = value & (HASH_STR_NBLW_MASK | HASH_STR_DCAL);
        if (value & HASH_STR_DCAL) {
            hash_finish_digest(s);
        }
        return;
    case HASH_IMR:
        s->imr = value;
        return;
    case HASH_SR:
        /* DCIS/DINIS are read-write-to-clear on real hardware. */
        s->sr &= ~(value & (HASH_SR_DCIS | HASH_SR_DINIS));
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

static void gnw_h7b0_hash_init(Object *obj)
{
    GnwH7B0HashState *s = GNW_H7B0_HASH(obj);

    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_hash_ops, s,
                           TYPE_GNW_H7B0_HASH, GNW_H7B0_HASH_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const VMStateDescription vmstate_gnw_h7b0_hash = {
    .name = TYPE_GNW_H7B0_HASH,
    .version_id = 1,
    .minimum_version_id = 1,
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
