/*
 * STM32H7B0 HASH processor (Nintendo Game & Watch)
 *
 * Real MD5/SHA-1/SHA-224/SHA-256 engine (backed by QEMU's own
 * crypto/hash.h, not hand-rolled), not the generic
 * create_unimplemented_device() stub this replaced -- gnwmanager's RAM
 * flash util calls HAL_HASHEx_SHA256_Start(..., HAL_MAX_DELAY) after
 * every flash write to verify it (gnwmanager.c's
 * GNWMANAGER_CHECK_HASH_FLASH state). HAL_MAX_DELAY means NO timeout:
 * the driver polls the digest-complete flag (SR.DCIS) forever, and an
 * unimplemented-device stub's reads always return 0, so that flag
 * never sets -- every single internal-flash write permanently wedges
 * the guest CPU in that poll loop. See gnw_h7b0_hash.c for the
 * register-level accumulation logic.
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

#ifndef HW_MISC_GNW_H7B0_HASH_H
#define HW_MISC_GNW_H7B0_HASH_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "hw/misc/gnw_h7b0_dma.h"

#define TYPE_GNW_H7B0_HASH "gnw-h7b0-hash"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0HashState, GNW_H7B0_HASH)

/* DMAMUX1 DMAREQ_ID for HASH_IN (see stm32h7xx_hal_dma.h's
 * DMA_REQUEST_HASH_IN) -- the stream is resolved at runtime from
 * firmware's DMAMUX routing, same pattern as SAI1 (see
 * gnw_h7b0_dma_set_request_notifier()). */
#define GNW_H7B0_HASH_DMA_REQUEST 78

/* Register offsets below are from STM32H7B0.svd's HASH peripheral. */
#define GNW_H7B0_HASH_SIZE  0x400

#define HASH_CR           0x000
#define HASH_DIN          0x004
#define HASH_STR          0x008
#define HASH_HR0          0x00c
#define HASH_HR1          0x010
#define HASH_HR2          0x014
#define HASH_HR3          0x018
#define HASH_HR4          0x01c
#define HASH_IMR          0x020
#define HASH_SR           0x024
#define HASH_CSR_BASE     0x0f8
#define HASH_CSR_END      0x1cc /* inclusive, 54 words */
#define HASH_DIGEST_BASE  0x310 /* HASH_HR0..HASH_HR7, extended digest */
#define HASH_DIGEST_END   0x32c /* inclusive, 8 words */

#define HASH_CR_INIT            (1U << 2)
#define HASH_CR_DMAE            (1U << 3)
#define HASH_CR_DATATYPE_SHIFT  4
#define HASH_CR_DATATYPE_MASK   (0x3U << HASH_CR_DATATYPE_SHIFT)
#define HASH_CR_MODE            (1U << 6)
#define HASH_CR_ALGO0           (1U << 7)
#define HASH_CR_ALGO1           (1U << 18)
#define HASH_CR_LKEY            (1U << 16)

#define HASH_STR_NBLW_MASK  0x1fU
#define HASH_STR_DCAL       (1U << 8)

#define HASH_SR_DINIS  (1U << 0)
#define HASH_SR_DCIS   (1U << 1)
#define HASH_SR_DMAS   (1U << 2)
#define HASH_SR_BUSY   (1U << 3)
#define HASH_SR_RESET_VALUE  HASH_SR_DINIS

#define HASH_IMR_DINIE  (1U << 0)
#define HASH_IMR_DCIE   (1U << 1)

/* Max message size gnwmanager ever hashes in one call: a full 256 KiB
 * internal-flash program context buffer (see gnw-flasher's
 * CONTEXT_BUFFER_SIZE) -- generously rounded up. */
#define HASH_MAX_MESSAGE_BYTES  (1 << 20)

/* Max HMAC key size any real caller uses -- diag's HMAC cases use a
 * 32-byte key; generously rounded up (real hardware's long-key/LKEY
 * mode supports up to 256 bytes). */
#define HASH_MAX_KEY_BYTES  1024

struct GnwH7B0HashState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;

    uint32_t cr;
    uint32_t str;
    uint32_t imr;
    uint32_t sr;
    uint32_t digest[8]; /* HR0-4 (0xC-0x1C) + extended HASH_HR5-7 (0x318-0x32C aliasing HR5-7) */
    uint32_t csr[(HASH_CSR_END - HASH_CSR_BASE) / 4 + 1]; /* inert scratch -- gnwmanager never suspends/resumes */

    uint8_t message[HASH_MAX_MESSAGE_BYTES];
    uint32_t message_len; /* bytes accumulated via DIN since the last INIT
                            * (plain-hash message, or the HMAC step-2
                            * message specifically -- see hmac_phase) */

    /* Real HASH hardware computes HMAC via a genuine 3-phase sequence
     * (key / message / key again -- see HMAC_Processing() in
     * stm32h7xx_hal_hash.c): hmac_phase tracks which of those 3 phases
     * is currently accumulating DIN writes when CR.MODE is set. 0 means
     * "not in an active HMAC sequence" (either plain-hash mode, or HMAC
     * just finished). Phase 1 writes land in key[]/key_len instead of
     * message[]/message_len; phase 3's re-fed key bytes are discarded
     * (the real key is already captured from phase 1). */
    uint32_t hmac_phase;
    uint8_t key[HASH_MAX_KEY_BYTES];
    uint32_t key_len;
};

void gnw_h7b0_hash_set_dma(GnwH7B0HashState *s, GnwH7B0DmaState *dma);

#endif
