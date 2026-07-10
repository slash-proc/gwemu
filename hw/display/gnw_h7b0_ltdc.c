/*
 * STM32H7B0 LTDC minimal stub (Nintendo Game & Watch)
 *
 * See gnw_h7b0_ltdc.h for scope/rationale. Structural template:
 * hw/display/pl110.c.
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
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "exec/cpu-common.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "hw/display/gnw_h7b0_ltdc.h"

/*
 * Integer nearest-neighbor upscale factor for the host window. The G&W
 * LCD is a tiny 320x240 panel; displayed 1:1 that's uncomfortably small
 * on a modern monitor, so every frame is blitted at SCALE x its native
 * size instead of relying on host-side window scaling (not uniformly
 * supported/discoverable across QEMU's -display backends).
 */
#define GNW_H7B0_LTDC_SCALE 2

static void gnw_h7b0_ltdc_reset(DeviceState *dev)
{
    GnwH7B0LtdcState *s = GNW_H7B0_LTDC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->invalidate = 1;
    s->pf_warned = false;
}

static bool gnw_h7b0_ltdc_enabled(GnwH7B0LtdcState *s)
{
    return (s->regs[GNW_H7B0_LTDC_GCR >> 2] & LTDC_GCR_LTDCEN) &&
           (s->regs[GNW_H7B0_LTDC_L1CR >> 2] & LTDC_LxCR_LEN);
}

static inline uint32_t gnw_h7b0_ltdc_rgb565_to_pixel32(uint16_t px)
{
    unsigned int r = ((px >> 11) & 0x1f) << 3;
    unsigned int g = ((px >> 5) & 0x3f) << 2;
    unsigned int b = (px & 0x1f) << 3;

    return rgb_to_pixel32(r, g, b);
}

/*
 * Full-frame read-and-blit, not the framebuffer.h dirty-tracking helper:
 * simplest way to also do the SCALE upscale (that helper draws one
 * source row into one destination row, no row/column duplication).
 * Correctness-focused MVP, not optimized for redraw cost.
 */
static void gnw_h7b0_ltdc_update_display(void *opaque)
{
    GnwH7B0LtdcState *s = GNW_H7B0_LTDC(opaque);
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t cfbar, cfblr, cfblnr, pfcr;
    int cols, rows, src_width;
    g_autofree uint8_t *linebuf = NULL;

    if (!gnw_h7b0_ltdc_enabled(s)) {
        return;
    }

    pfcr = s->regs[GNW_H7B0_LTDC_L1PFCR >> 2] & LTDC_LxPFCR_PF_MASK;
    if (pfcr != LTDC_PF_RGB565) {
        if (!s->pf_warned) {
            qemu_log_mask(LOG_UNIMP,
                          "gnw_h7b0_ltdc: only RGB565 is drawn, Layer1 PFCR "
                          "requested format %u -- nothing will be shown\n",
                          pfcr);
            s->pf_warned = true;
        }
        return;
    }

    if (surface_bits_per_pixel(surface) != 32) {
        qemu_log_mask(LOG_UNIMP,
                      "gnw_h7b0_ltdc: only a 32bpp host surface is "
                      "supported\n");
        return;
    }

    cfbar = s->regs[GNW_H7B0_LTDC_L1CFBAR >> 2];
    cfblr = s->regs[GNW_H7B0_LTDC_L1CFBLR >> 2];
    cfblnr = s->regs[GNW_H7B0_LTDC_L1CFBLNR >> 2] & 0x7FFU;

    /*
     * CFBP (bits 16-28) is the real row-to-row pitch in bytes; CFBLL
     * (bits 0-12) is "active_width * bpp + 3" and is *not* the same as
     * the pitch when there's row padding (found live: CFBP=640,
     * CFBLL=647 -- (647-3)/2=322, off by two pixels from the real
     * 320-wide framebuffer CFBP/2 gives correctly). Use CFBP.
     */
    src_width = (int)((cfblr >> LTDC_LxCFBLR_CFBP_SHIFT) &
                       LTDC_LxCFBLR_CFBP_MASK);
    cols = src_width / 2;
    rows = (int)cfblnr;

    if (cols <= 0 || rows <= 0) {
        return;
    }

    if (s->invalidate ||
        cols * GNW_H7B0_LTDC_SCALE != surface_width(surface) ||
        rows * GNW_H7B0_LTDC_SCALE != surface_height(surface)) {
        qemu_console_resize(s->con, cols * GNW_H7B0_LTDC_SCALE,
                            rows * GNW_H7B0_LTDC_SCALE);
        surface = qemu_console_surface(s->con);
    }

    linebuf = g_malloc(src_width);

    for (int y = 0; y < rows; y++) {
        uint8_t *drow0 = surface_data(surface) +
                          (hwaddr)y * GNW_H7B0_LTDC_SCALE * surface_stride(surface);

        cpu_physical_memory_read(cfbar + (hwaddr)y * src_width, linebuf,
                                 src_width);

        for (int x = 0; x < cols; x++) {
            uint32_t px = gnw_h7b0_ltdc_rgb565_to_pixel32(
                lduw_le_p(linebuf + x * 2));

            for (int sy = 0; sy < GNW_H7B0_LTDC_SCALE; sy++) {
                uint32_t *drow = (uint32_t *)(drow0 +
                                              (hwaddr)sy * surface_stride(surface));
                for (int sx = 0; sx < GNW_H7B0_LTDC_SCALE; sx++) {
                    drow[x * GNW_H7B0_LTDC_SCALE + sx] = px;
                }
            }
        }
    }

    dpy_gfx_update(s->con, 0, 0, cols * GNW_H7B0_LTDC_SCALE,
                   rows * GNW_H7B0_LTDC_SCALE);
    s->invalidate = 0;
}

static void gnw_h7b0_ltdc_invalidate_display(void *opaque)
{
    GnwH7B0LtdcState *s = GNW_H7B0_LTDC(opaque);

    s->invalidate = 1;
}

static uint64_t gnw_h7b0_ltdc_read(void *opaque, hwaddr addr,
                                    unsigned int size)
{
    GnwH7B0LtdcState *s = GNW_H7B0_LTDC(opaque);

    if (addr >= GNW_H7B0_LTDC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_ltdc_write(void *opaque, hwaddr addr,
                                 uint64_t val64, unsigned int size)
{
    GnwH7B0LtdcState *s = GNW_H7B0_LTDC(opaque);
    uint32_t value = val64;

    if (addr >= GNW_H7B0_LTDC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    switch (addr) {
    case GNW_H7B0_LTDC_SRCR:
        /*
         * Real hardware copies shadow registers into the active set on
         * a reload event, clearing IMR/VBR once done; we treat every
         * register as always-active (no separate shadow set), so this
         * "reload" is instantaneous -- just clear the request bits so
         * firmware's post-write poll doesn't hang.
         */
        s->regs[addr >> 2] = value & ~(LTDC_SRCR_IMR | LTDC_SRCR_VBR);
        return;
    default:
        s->regs[addr >> 2] = value;
    }
}

static const MemoryRegionOps gnw_h7b0_ltdc_ops = {
    .read = gnw_h7b0_ltdc_read,
    .write = gnw_h7b0_ltdc_write,
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

static const GraphicHwOps gnw_h7b0_ltdc_gfx_ops = {
    .invalidate  = gnw_h7b0_ltdc_invalidate_display,
    .gfx_update  = gnw_h7b0_ltdc_update_display,
};

static void gnw_h7b0_ltdc_realize(DeviceState *dev, Error **errp)
{
    GnwH7B0LtdcState *s = GNW_H7B0_LTDC(dev);

    memory_region_init_io(&s->mmio, OBJECT(s), &gnw_h7b0_ltdc_ops, s,
                           TYPE_GNW_H7B0_LTDC, GNW_H7B0_LTDC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
    s->con = graphic_console_init(dev, 0, &gnw_h7b0_ltdc_gfx_ops, s);
}

static const VMStateDescription vmstate_gnw_h7b0_ltdc = {
    .name = TYPE_GNW_H7B0_LTDC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0LtdcState, GNW_H7B0_LTDC_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_ltdc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = gnw_h7b0_ltdc_realize;
    dc->vmsd = &vmstate_gnw_h7b0_ltdc;
    device_class_set_legacy_reset(dc, gnw_h7b0_ltdc_reset);
}

static const TypeInfo gnw_h7b0_ltdc_info = {
    .name          = TYPE_GNW_H7B0_LTDC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0LtdcState),
    .class_init    = gnw_h7b0_ltdc_class_init,
};

static void gnw_h7b0_ltdc_register_types(void)
{
    type_register_static(&gnw_h7b0_ltdc_info);
}

type_init(gnw_h7b0_ltdc_register_types)
