/* Auto-generated stub for JPEG */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/misc/gnw_h7b0_jpeg.h"
#include "hw/misc/gnw_h7b0_regs_jpeg.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ONLY_JPEG

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"
#pragma GCC diagnostic pop

static GnwH7B0JpegState *global_jpeg_state = NULL;

void gnw_h7b0_jpeg_get_last_decoded_size(uint32_t *width, uint32_t *height)
{
    if (global_jpeg_state && global_jpeg_state->y_plane) {
        *width = (uint32_t)global_jpeg_state->plane_width;
        *height = (uint32_t)global_jpeg_state->plane_height;
        return;
    }
    *width = 0;
    *height = 0;
}

void gnw_h7b0_jpeg_get_last_chroma_size(uint32_t *width, uint32_t *height)
{
    if (global_jpeg_state && global_jpeg_state->y_plane) {
        *width = (uint32_t)global_jpeg_state->chroma_width;
        *height = (uint32_t)global_jpeg_state->chroma_height;
        return;
    }
    *width = 0;
    *height = 0;
}

/* Clamp a fixed-point-converted sample to a valid byte. */
static inline uint8_t gnw_h7b0_jpeg_clamp_u8(int v)
{
    if (v < 0) {
        return 0;
    }
    if (v > 255) {
        return 255;
    }
    return (uint8_t)v;
}

/*
 * Hand-parse the SOF0 marker (FF C0) out of the buffered input JPEG bytes
 * to recover each component's real H/V sampling-factor nibble. stb_image's
 * public API doesn't expose these, but real hardware's CONFRN1's VSF/HSF
 * fields need them for register-read fidelity (see plan doc). Returns true
 * and fills h_out/v_out with component 1's (luma) sampling factors if a
 * SOF0 marker was found and looked well-formed; false otherwise (caller
 * should fall back to its previous approximation).
 */
static bool gnw_h7b0_jpeg_parse_sof0_luma_sampling(const uint8_t *buf, size_t len,
                                                    int *h_out, int *v_out)
{
    size_t i = 0;

    while (i + 4 <= len) {
        if (buf[i] != 0xFF) {
            i++;
            continue;
        }
        uint8_t marker = buf[i + 1];
        /* Skip fill bytes / standalone markers with no length field. */
        if (marker == 0xFF) {
            i++;
            continue;
        }
        if (marker == 0xD8 || marker == 0xD9 ||
            (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
            i += 2;
            continue;
        }
        if (i + 4 > len) {
            break;
        }
        uint32_t seg_len = ((uint32_t)buf[i + 2] << 8) | buf[i + 3];
        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
            /* SOF0/1/2: marker(2)+len(2)+precision(1)+height(2)+width(2)+
             * num_components(1)+per-component(id,samp,qt)*3. */
            size_t sof_off = i + 4;
            if (sof_off + 6 > len) {
                return false;
            }
            uint8_t num_comp = buf[sof_off + 5];
            size_t comp0_off = sof_off + 6;
            if (num_comp < 1 || comp0_off + 3 > len) {
                return false;
            }
            uint8_t samp = buf[comp0_off + 1];
            *h_out = (samp >> 4) & 0xF;
            *v_out = samp & 0xF;
            return true;
        }
        if (seg_len < 2 || i + 2 + seg_len > len) {
            break;
        }
        i += 2 + seg_len;
    }
    return false;
}

static void gnw_h7b0_jpeg_reset(DeviceState *dev)
{
    GnwH7B0JpegState *s = GNW_H7B0_JPEG(dev);
    for (int i = 0; i < (GNW_H7B0_JPEG_SIZE / 4); i++) {
        s->regs[i] = get_jpeg_reset_value(i * 4);
    }
    if (s->inbuf) {
        g_byte_array_set_size(s->inbuf, 0);
    }
    g_free(s->y_plane);
    g_free(s->cb_plane);
    g_free(s->cr_plane);
    s->y_plane = s->cb_plane = s->cr_plane = NULL;
    s->plane_width = s->plane_height = 0;
    s->chroma_width = s->chroma_height = 0;
    s->dor_cursor = 0;
}

static uint64_t gnw_h7b0_jpeg_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0JpegState *s = GNW_H7B0_JPEG(opaque);
    if (addr >= GNW_H7B0_JPEG_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return 0;
    }
    if (addr == GNW_H7B0_JPEG_DOR_OFFSET) {
        /* Real DOR: pop the next 4 bytes from a flat cursor over
         * y_plane||cb_plane||cr_plane, little-endian-packed like the
         * existing DIR write path. Entirely synchronous, no timers --
         * mirrors the DIR register's own instant-completion style. */
        uint32_t y_size = (uint32_t)s->plane_width * (uint32_t)s->plane_height;
        uint32_t c_size = (uint32_t)s->chroma_width * (uint32_t)s->chroma_height;
        uint32_t total = y_size + 2 * c_size;
        uint32_t v = 0;

        if (total == 0 || !s->y_plane) {
            return 0;
        }
        for (int i = 0; i < 4; i++) {
            uint32_t pos = s->dor_cursor + i;
            uint8_t byte = 0;
            if (pos < total) {
                if (pos < y_size) {
                    byte = s->y_plane[pos];
                } else if (pos < y_size + c_size) {
                    byte = s->cb_plane[pos - y_size];
                } else {
                    byte = s->cr_plane[pos - y_size - c_size];
                }
            }
            v |= ((uint32_t)byte) << (8 * i);
        }
        if (s->dor_cursor < total) {
            s->dor_cursor += 4;
        }
        /*
         * Real hardware's output FIFO threshold (JPEG_FIFO_TH_SIZE = 8
         * words = 32 bytes, stm32h7xx_hal_jpeg.c) lets firmware's polling
         * loop drain 8 words per outer-loop iteration via OFTF instead of
         * checking flags and reading DOR one word at a time via OFNEF.
         * The total number of DOR reads is identical either way (real
         * HAL's JPEG_StoreOutputData() still reads DOR in a plain word-at-
         * a-time loop internally regardless of threshold) -- but never
         * setting OFTF here forced every single decode into the ~8x-more-
         * outer-loop-iterations OFNEF path, each iteration paying its own
         * separate SR-flag-check MMIO round trip on top of the DOR read
         * itself. Real cover-art-heavy screens (coverflow/menus) do this
         * thousands of times per frame, so that per-iteration MMIO/BQL
         * overhead was a genuine, measurable performance bug (perf showed
         * >16% total CPU in QEMU's own MMIO dispatch/lock path during a
         * menu scroll-loop) -- not just a cosmetic fidelity gap.
         */
        uint32_t remaining = (s->dor_cursor < total) ? (total - s->dor_cursor) : 0;
        if (remaining >= 32) {
            s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] |= JPEG_SR_OFTF;
        } else {
            s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] &= ~JPEG_SR_OFTF;
        }
        if (remaining == 0) {
            s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] &= ~JPEG_SR_OFNEF;
        }
        return v;
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_jpeg_write(void *opaque, hwaddr addr, uint64_t val64, unsigned int size)
{
    GnwH7B0JpegState *s = GNW_H7B0_JPEG(opaque);
    if (addr >= GNW_H7B0_JPEG_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return;
    }
    uint32_t mask = get_jpeg_write_mask(addr);
    s->regs[addr >> 2] = (s->regs[addr >> 2] & ~mask) | ((uint32_t)val64 & mask);

    /*
     * SR/CFR (0x34/0x38) are otherwise a plain shadow with no decode/encode
     * side effects (this is a dumb RAM-backed stub, not a real JPEG codec).
     * Real firmware's decode-completion poll loop (HAL's JPEG_Process(), or
     * a hand-rolled equivalent) waits on SR.EOCF going high and SR.OFNEF/
     * OFTF going low to know the operation is done. With EOCF permanently
     * stuck at 0 (its reset value) that loop -- and any "drain the output
     * FIFO while OFNEF is set" inner loop alongside it -- never terminates,
     * walking an output pointer straight off the end of whatever SRAM
     * buffer it's writing into. Found via retro-go's cover-art JPEG decode:
     * a real BusFault reading one byte past the very end of AXISRAM1-3
     * (0x24100000), with the fault's LR sitting on JPEG's own MMIO base
     * address.
     *
     * The actual "begin this operation" trigger is CONFR0.START (bit 0),
     * NOT CR -- CR is codec-core-enable (JCEN) plus interrupt-enable bits,
     * set once up front and left alone; CONFR0.START is written low->high
     * per operation and cleared again by JPEG_Process() itself once it
     * observes completion (see stm32h7xx_hal_jpeg.c's
     * "hjpeg->Instance->CONFR0 &= ~JPEG_CONFR0_START"). Faking an
     * immediate, empty "done" completion whenever START goes high (real
     * EOCF semantics, just instant rather than after real codec work) is
     * enough for any real polling loop to see the process as finished and
     * stop -- the decoded output content will be all-zero garbage (there's
     * no real codec here), but that's a correctness shortfall for the
     * eventual real JPEG model, not a hang or a crash.
     *
     * SR's IFTF/IFNFF ("input FIFO has room") are also both set in its
     * reset value (0x6) and nothing ever clears them -- real hardware's
     * input FIFO exerts real backpressure once full, clearing these, but
     * ours never does. That alone (independent of EOCF/output handling
     * above) makes JPEG_Process()'s input-refill branch
     * (JPEG_ReadInputData()) fire on every single poll forever, walking
     * its source buffer pointer off the end of AXISRAM once the driver's
     * own end-of-data bookkeeping gets out of sync with a peripheral that
     * never stops asking for more. Clearing them alongside EOCF -- "done,
     * not accepting more input either" -- closes that off the same way.
     */
    if (addr == GNW_H7B0_JPEG_CONFR0_OFFSET && (val64 & JPEG_CONFR0_START)) {
        if (s->inbuf) {
            g_byte_array_set_size(s->inbuf, 0);
        }
        s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] &= ~JPEG_SR_EOCF;
        s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] |= (JPEG_SR_IFTF | JPEG_SR_IFNFF);
    } else if (addr == GNW_H7B0_JPEG_CR_OFFSET) {
        /* IFF/OFF (input/output FIFO flush) are real pulse bits -- the SVD
         * documents them as "always read as 0". Mirror the self-clearing
         * pattern used elsewhere in this codebase (e.g. gnw_h7b0_crc.c's
         * RESET bit) by immediately clearing them back out of the shadow
         * register right after the generic masked write above latches them. */
        s->regs[GNW_H7B0_JPEG_CR_OFFSET >> 2] &= ~(uint32_t)JPEG_CR_FLUSH_PULSE_MASK;
    } else if (addr == GNW_H7B0_JPEG_CFR_OFFSET) {
        /* CFR is a real write-1-to-clear pulse register (its bit positions
         * mirror SR's exactly): clear the matching SR bits, then reset CFR
         * itself back to 0 rather than leaving the generic shadow-write
         * value latched (real hardware doesn't persist it either). */
        s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] &= ~(uint32_t)val64;
        s->regs[GNW_H7B0_JPEG_CFR_OFFSET >> 2] = 0;
    } else if (addr == GNW_H7B0_JPEG_DIR_OFFSET) {
        if (s->inbuf) {
            uint32_t v = (uint32_t)val64;
            g_byte_array_append(s->inbuf, (const guint8 *)&v, 4);

            if (!(s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] & (1U << 6))) {
                int w, h, comp;
                if (stbi_info_from_memory(s->inbuf->data, s->inbuf->len, &w, &h, &comp)) {
                    s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] |= (1U << 6); /* HPDF */

                    /* NF = number of color components - 1 (CONFR1 field
                     * semantics per the SVD), derived from the real decoded
                     * component count instead of a hardcoded "always 3
                     * components" placeholder. JPEG components are 1
                     * (grayscale) or 3 (YCbCr) in the overwhelming common
                     * case; clamp defensively for the rare/invalid case. */
                    uint32_t nf = (comp >= 1 && comp <= 4) ? (uint32_t)(comp - 1) : 2;
                    uint32_t c1 = s->regs[GNW_H7B0_JPEG_CONFR1_OFFSET >> 2];
                    s->regs[GNW_H7B0_JPEG_CONFR1_OFFSET >> 2] =
                        (c1 & ~(JPEG_CONFR1_YSIZE_MASK | JPEG_CONFR1_NF_MASK)) |
                        ((uint32_t)h << JPEG_CONFR1_YSIZE_SHIFT) | nf;

                    uint32_t c3 = s->regs[GNW_H7B0_JPEG_CONFR3_OFFSET >> 2];
                    s->regs[GNW_H7B0_JPEG_CONFR3_OFFSET >> 2] =
                        (c3 & ~JPEG_CONFR3_XSIZE_MASK) | ((uint32_t)w << JPEG_CONFR3_XSIZE_SHIFT);

                    /* CONFRN1 (component 1 / luma) NB/HSF/VSF fields: real
                     * per-image sampling factors, hand-parsed from the SOF0
                     * marker (stbi_info_from_memory() doesn't expose these).
                     * NB (data units - 1 per MCU) = H*V - 1 for luma. Falls
                     * back to the previous approximation (comp==1: no
                     * subsampling; comp==3: assume common 4:2:0 => NB=3) if
                     * SOF0 parsing fails for any reason. */
                    int sof_h = 0, sof_v = 0;
                    uint32_t confrn1;
                    if (gnw_h7b0_jpeg_parse_sof0_luma_sampling(s->inbuf->data,
                                                                s->inbuf->len,
                                                                &sof_h, &sof_v) &&
                        sof_h >= 1 && sof_h <= 4 && sof_v >= 1 && sof_v <= 4) {
                        uint32_t nb = (uint32_t)(sof_h * sof_v - 1);
                        confrn1 = (nb << JPEG_CONFRN_NB_SHIFT) |
                                  ((uint32_t)sof_v << JPEG_CONFRN_VSF_SHIFT) |
                                  ((uint32_t)sof_h << JPEG_CONFRN_HSF_SHIFT);
                    } else {
                        confrn1 = (comp == 1) ? 0 : (3U << JPEG_CONFRN_NB_SHIFT);
                    }
                    s->regs[GNW_H7B0_JPEG_CONFRN1_OFFSET >> 2] = confrn1;
                }
            }

            if (s->inbuf->len >= 2) {
                bool found_eoi = false;
                for (int i = 0; i < MIN(s->inbuf->len - 1, 4); i++) {
                    if (s->inbuf->data[s->inbuf->len - 2 - i] == 0xFF &&
                        s->inbuf->data[s->inbuf->len - 1 - i] == 0xD9) {
                        found_eoi = true;
                        break;
                    }
                }
                if (found_eoi) {
                    int w, h, comp;
                    stbi_uc *rgb = stbi_load_from_memory(s->inbuf->data, s->inbuf->len, &w, &h, &comp, 3);
                    if (rgb) {
                        /*
                         * Real hardware/firmware expects chroma-subsampled
                         * output sized per the image's real SOF0 H/V
                         * sampling factors (e.g. 4:2:0 => 1.5*w*h total, not
                         * 3*w*h) -- serving full-resolution chroma made our
                         * DOR output ~2x the size firmware's own destination
                         * buffer expects, so firmware's polling loop only
                         * drained part of it, leaving the buffer's tail
                         * stale (visible as banded corruption). Derive the
                         * real H/V factors here (same parse used for
                         * CONFRN1 above) and subsample Cb/Cr to match.
                         */
                        int sof_h2 = 0, sof_v2 = 0;
                        if (!gnw_h7b0_jpeg_parse_sof0_luma_sampling(
                                s->inbuf->data, s->inbuf->len, &sof_h2, &sof_v2) ||
                            sof_h2 < 1 || sof_h2 > 4 || sof_v2 < 1 || sof_v2 > 4) {
                            sof_h2 = 1;
                            sof_v2 = 1;
                        }
                        if (comp == 1) {
                            sof_h2 = 1;
                            sof_v2 = 1;
                        }
                        int chroma_w = (w + sof_h2 - 1) / sof_h2;
                        int chroma_h = (h + sof_v2 - 1) / sof_v2;

                        g_free(s->y_plane);
                        g_free(s->cb_plane);
                        g_free(s->cr_plane);
                        s->y_plane = g_malloc(w * h);
                        s->cb_plane = g_malloc(chroma_w * chroma_h);
                        s->cr_plane = g_malloc(chroma_w * chroma_h);
                        s->plane_width = w;
                        s->plane_height = h;
                        s->chroma_width = chroma_w;
                        s->chroma_height = chroma_h;
                        s->dor_cursor = 0;

                        for (int i = 0; i < w * h; i++) {
                            int r = rgb[i * 3 + 0];
                            int g = rgb[i * 3 + 1];
                            int b = rgb[i * 3 + 2];
                            /* BT.601 RGB->YCbCr, fixed-point (Q16)
                             * approximation of the standard coefficients. */
                            int y = (299 * r + 587 * g + 114 * b) / 1000;
                            s->y_plane[i] = gnw_h7b0_jpeg_clamp_u8(y);
                        }
                        for (int cy = 0; cy < chroma_h; cy++) {
                            for (int cx = 0; cx < chroma_w; cx++) {
                                /* Nearest-sample the source pixel at this
                                 * chroma cell's top-left corner. */
                                int sx = cx * sof_h2;
                                int sy = cy * sof_v2;
                                if (sx >= w) sx = w - 1;
                                if (sy >= h) sy = h - 1;
                                int idx = sy * w + sx;
                                int r = rgb[idx * 3 + 0];
                                int g = rgb[idx * 3 + 1];
                                int b = rgb[idx * 3 + 2];
                                int cb = (-168736 * r - 331264 * g + 500000 * b) / 1000000 + 128;
                                int cr = (500000 * r - 418688 * g - 81312 * b) / 1000000 + 128;
                                s->cb_plane[cy * chroma_w + cx] = gnw_h7b0_jpeg_clamp_u8(cb);
                                s->cr_plane[cy * chroma_w + cx] = gnw_h7b0_jpeg_clamp_u8(cr);
                            }
                        }
                        stbi_image_free(rgb);
                    }
                    s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] |= JPEG_SR_EOCF;
                    s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] &=
                        ~(JPEG_SR_OFTF | JPEG_SR_COF |
                          JPEG_SR_IFTF | JPEG_SR_IFNFF);
                    /* Real output data is now available in y/cb/cr_plane
                     * (DOR-readable) -- set OFNEF ("output FIFO not empty")
                     * so a real polling loop (JPEG_Process()) drains DOR,
                     * and OFTF too (real hardware's output FIFO threshold)
                     * whenever at least a full 8-word/32-byte chunk is
                     * available, so firmware takes the bulk-drain path
                     * instead of checking flags once per single word --
                     * see the DOR read handler's comment for why this
                     * matters for performance, not just fidelity. Both
                     * flags are kept in sync as the DOR read handler
                     * drains the cursor. */
                    if (s->y_plane) {
                        uint32_t total0 = (uint32_t)s->plane_width * (uint32_t)s->plane_height +
                                          2 * (uint32_t)s->chroma_width * (uint32_t)s->chroma_height;
                        s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] |= JPEG_SR_OFNEF;
                        if (total0 >= 32) {
                            s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] |= JPEG_SR_OFTF;
                        }
                    }
                }
            }
        }
    }
}

static const MemoryRegionOps gnw_h7b0_jpeg_ops = {
    .read = gnw_h7b0_jpeg_read,
    .write = gnw_h7b0_jpeg_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void gnw_h7b0_jpeg_init(Object *obj)
{
    GnwH7B0JpegState *s = GNW_H7B0_JPEG(obj);
    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_jpeg_ops, s, TYPE_GNW_H7B0_JPEG, GNW_H7B0_JPEG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    s->inbuf = g_byte_array_new();
    global_jpeg_state = s;
}

static const VMStateDescription vmstate_gnw_h7b0_jpeg = {
    .name = TYPE_GNW_H7B0_JPEG,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0JpegState, GNW_H7B0_JPEG_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_jpeg_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &vmstate_gnw_h7b0_jpeg;
    device_class_set_legacy_reset(dc, gnw_h7b0_jpeg_reset);
}

static const TypeInfo gnw_h7b0_jpeg_info = {
    .name          = TYPE_GNW_H7B0_JPEG,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0JpegState),
    .instance_init = gnw_h7b0_jpeg_init,
    .class_init    = gnw_h7b0_jpeg_class_init,
};

static void gnw_h7b0_jpeg_register_types(void)
{
    type_register_static(&gnw_h7b0_jpeg_info);
}
type_init(gnw_h7b0_jpeg_register_types)
