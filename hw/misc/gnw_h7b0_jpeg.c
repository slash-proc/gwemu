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

uint8_t *gnw_h7b0_jpeg_get_hack_buffer(int *width, int *height)
{
    if (global_jpeg_state && global_jpeg_state->rgb_buffer) {
        *width = global_jpeg_state->rgb_width;
        *height = global_jpeg_state->rgb_height;
        return global_jpeg_state->rgb_buffer;
    }
    *width = 0;
    *height = 0;
    return NULL;
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
}

static uint64_t gnw_h7b0_jpeg_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0JpegState *s = GNW_H7B0_JPEG(opaque);
    if (addr >= GNW_H7B0_JPEG_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return 0;
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
                    uint32_t c1 = s->regs[GNW_H7B0_JPEG_CONFR1_OFFSET >> 2];
                    s->regs[GNW_H7B0_JPEG_CONFR1_OFFSET >> 2] = (c1 & ~0xFFFF0003U) | (h << 16) | 2;
                    uint32_t c3 = s->regs[GNW_H7B0_JPEG_CONFR3_OFFSET >> 2];
                    s->regs[GNW_H7B0_JPEG_CONFR3_OFFSET >> 2] = (c3 & ~0xFFFF0000U) | (w << 16);
                    s->regs[GNW_H7B0_JPEG_CONFRN1_OFFSET >> 2] = (3 << 4);
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
                        if (s->rgb_buffer) {
                            g_free(s->rgb_buffer);
                        }
                        s->rgb_buffer = g_malloc(w * h * 2);
                        s->rgb_width = w;
                        s->rgb_height = h;
                        for (int i = 0; i < w * h; i++) {
                            uint16_t r5 = rgb[i * 3 + 0] >> 3;
                            uint16_t g6 = rgb[i * 3 + 1] >> 2;
                            uint16_t b5 = rgb[i * 3 + 2] >> 3;
                            uint16_t rgb565 = (r5 << 11) | (g6 << 5) | b5;
                            stw_le_p(s->rgb_buffer + i * 2, rgb565);
                        }
                        stbi_image_free(rgb);
                    }
                    s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] |= JPEG_SR_EOCF;
                    s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] &=
                        ~(JPEG_SR_OFNEF | JPEG_SR_OFTF | JPEG_SR_COF |
                          JPEG_SR_IFTF | JPEG_SR_IFNFF);
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
