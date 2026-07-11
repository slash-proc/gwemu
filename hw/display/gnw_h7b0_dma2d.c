/*
 * STM32H7B0 DMA2D (Chrom-ART Accelerator) device model (Nintendo Game & Watch)
 *
 * See gnw_h7b0_dma2d.h for scope/rationale/register-source notes.
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
#include "hw/display/gnw_h7b0_dma2d.h"
#include "hw/misc/gnw_h7b0_jpeg.h"

/*
 * Pixel-format conversion helpers. Everything funnels through a common
 * 32-bit ARGB8888 intermediate (8 bits/channel, alpha in bits 24-31) --
 * clean-room implementations of the standard truncate/replicate
 * expansion math for each STM32H7 DMA2D color mode, not copied from any
 * third-party source (see file header / gnw_h7b0_dma2d.h).
 */

static bool gnw_h7b0_dma2d_format_bpp(uint32_t cm, int *bpp)
{
    switch (cm) {
    case DMA2D_INPUT_ARGB8888:
        *bpp = 4;
        return true;
    case DMA2D_INPUT_RGB565:
    case DMA2D_INPUT_ARGB1555:
    case DMA2D_INPUT_ARGB4444:
        *bpp = 2;
        return true;
    case DMA2D_INPUT_L8:
        *bpp = 1;
        return true;
    default:
        return false;
    }
}

static uint32_t gnw_h7b0_dma2d_read_argb8888(uint32_t cm, hwaddr addr,
                                              hwaddr clut_addr)
{
    switch (cm) {
    case DMA2D_INPUT_ARGB8888: {
        uint8_t buf[4];
        cpu_physical_memory_read(addr, buf, 4);
        return ldl_le_p(buf);
    }
    case DMA2D_INPUT_RGB565: {
        uint8_t buf[2];
        uint16_t px;
        unsigned int r5, g6, b5, r8, g8, b8;

        cpu_physical_memory_read(addr, buf, 2);
        px = lduw_le_p(buf);
        r5 = (px >> 11) & 0x1F;
        g6 = (px >> 5) & 0x3F;
        b5 = px & 0x1F;
        r8 = (r5 << 3) | (r5 >> 2);
        g8 = (g6 << 2) | (g6 >> 4);
        b8 = (b5 << 3) | (b5 >> 2);
        return 0xFF000000U | (r8 << 16) | (g8 << 8) | b8;
    }
    case DMA2D_INPUT_ARGB1555: {
        uint8_t buf[2];
        uint16_t px;
        unsigned int a1, r5, g5, b5, r8, g8, b8;

        cpu_physical_memory_read(addr, buf, 2);
        px = lduw_le_p(buf);
        a1 = (px >> 15) & 0x1;
        r5 = (px >> 10) & 0x1F;
        g5 = (px >> 5) & 0x1F;
        b5 = px & 0x1F;
        r8 = (r5 << 3) | (r5 >> 2);
        g8 = (g5 << 3) | (g5 >> 2);
        b8 = (b5 << 3) | (b5 >> 2);
        return (a1 ? 0xFF000000U : 0U) | (r8 << 16) | (g8 << 8) | b8;
    }
    case DMA2D_INPUT_ARGB4444: {
        uint8_t buf[2];
        uint16_t px;
        unsigned int a4, r4, g4, b4;

        cpu_physical_memory_read(addr, buf, 2);
        px = lduw_le_p(buf);
        a4 = (px >> 12) & 0xF;
        r4 = (px >> 8) & 0xF;
        g4 = (px >> 4) & 0xF;
        b4 = px & 0xF;
        return ((a4 * 0x11U) << 24) | ((r4 * 0x11U) << 16) |
               ((g4 * 0x11U) << 8) | (b4 * 0x11U);
    }
    case DMA2D_INPUT_L8: {
        uint8_t idx;

        cpu_physical_memory_read(addr, &idx, 1);
        if (clut_addr == 0) {
            /* No CLUT loaded: treat as opaque grayscale, closest
             * sane fallback rather than reading garbage memory. */
            return 0xFF000000U | (idx << 16) | (idx << 8) | idx;
        } else {
            uint8_t buf[4];

            cpu_physical_memory_read(clut_addr + (hwaddr)idx * 4, buf, 4);
            return ldl_le_p(buf);
        }
    }
    default:
        return 0xFF000000U;
    }
}

static void gnw_h7b0_dma2d_write_output(uint32_t cm, hwaddr addr,
                                         uint32_t argb8888)
{
    unsigned int a = (argb8888 >> 24) & 0xFF;
    unsigned int r = (argb8888 >> 16) & 0xFF;
    unsigned int g = (argb8888 >> 8) & 0xFF;
    unsigned int b = argb8888 & 0xFF;

    switch (cm) {
    case DMA2D_OUTPUT_ARGB8888: {
        uint8_t buf[4];

        stl_le_p(buf, argb8888);
        cpu_physical_memory_write(addr, buf, 4);
        break;
    }
    case DMA2D_OUTPUT_RGB565: {
        uint16_t px = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) |
                                  (b >> 3));
        uint8_t buf[2];

        stw_le_p(buf, px);
        cpu_physical_memory_write(addr, buf, 2);
        break;
    }
    case DMA2D_OUTPUT_ARGB1555: {
        uint16_t px = (uint16_t)(((a & 0x80) << 8) | ((r & 0xF8) << 7) |
                                  ((g & 0xF8) << 2) | (b >> 3));
        uint8_t buf[2];

        stw_le_p(buf, px);
        cpu_physical_memory_write(addr, buf, 2);
        break;
    }
    case DMA2D_OUTPUT_ARGB4444: {
        uint16_t px = (uint16_t)(((a & 0xF0) << 8) | ((r & 0xF0) << 4) |
                                  (g & 0xF0) | (b >> 4));
        uint8_t buf[2];

        stw_le_p(buf, px);
        cpu_physical_memory_write(addr, buf, 2);
        break;
    }
    default:
        qemu_log_mask(LOG_UNIMP,
                      "gnw_h7b0_dma2d: unsupported output color mode %u\n",
                      cm);
        break;
    }
}

static int gnw_h7b0_dma2d_output_bpp(uint32_t cm)
{
    switch (cm) {
    case DMA2D_OUTPUT_ARGB8888:
        return 4;
    case DMA2D_OUTPUT_RGB565:
    case DMA2D_OUTPUT_ARGB1555:
    case DMA2D_OUTPUT_ARGB4444:
        return 2;
    default:
        return 0;
    }
}

/* Applies FGPFCCR/BGPFCCR's AM (alpha mode) + ALPHA fields to a pixel's
 * native alpha channel: NO_MODIF keeps it, REPLACE overrides it outright,
 * MULTIPLY scales it by ALPHA/255 -- matches
 * stm32h7xx_hal_dma2d.h's DMA2D_ALPHA_MODE_* semantics. */
static unsigned int gnw_h7b0_dma2d_apply_alpha_mode(uint32_t pfccr,
                                                     unsigned int native_a)
{
    uint32_t am = (pfccr & DMA2D_PFCCR_AM_MASK) >> DMA2D_PFCCR_AM_SHIFT;
    unsigned int alpha_field = (pfccr & DMA2D_PFCCR_ALPHA_MASK) >>
                                DMA2D_PFCCR_ALPHA_SHIFT;

    switch (am) {
    case DMA2D_AM_REPLACE:
        return alpha_field;
    case DMA2D_AM_MULTIPLY:
        return (native_a * alpha_field) / 255U;
    case DMA2D_AM_NO_MODIF:
    default:
        return native_a;
    }
}

static void gnw_h7b0_dma2d_update_irq(GnwH7B0Dma2dState *s)
{
    bool pending = ((s->regs[GNW_H7B0_DMA2D_ISR >> 2] & DMA2D_ISR_TCIF) &&
                    (s->regs[GNW_H7B0_DMA2D_CR >> 2] & DMA2D_CR_TCIE)) ||
                   ((s->regs[GNW_H7B0_DMA2D_ISR >> 2] & DMA2D_ISR_TEIF) &&
                    (s->regs[GNW_H7B0_DMA2D_CR >> 2] & DMA2D_CR_TEIE));

    qemu_set_irq(s->irq, pending);
}

/*
 * Performs the actual pixel transfer for whatever CR.MODE is currently
 * configured. Runs synchronously (no timer/latency modeling, same choice
 * gnw_h7b0_jpeg.c made for its fake-instant decode) -- real firmware
 * polls ISR.TCIF right after setting CR.START, so by the time it reads
 * back the flag must already be set.
 */
static void gnw_h7b0_dma2d_do_transfer(GnwH7B0Dma2dState *s)
{
    uint32_t cr = s->regs[GNW_H7B0_DMA2D_CR >> 2];
    uint32_t mode = (cr & DMA2D_CR_MODE_MASK) >> DMA2D_CR_MODE_SHIFT;
    uint32_t nlr = s->regs[GNW_H7B0_DMA2D_NLR >> 2];
    uint32_t lines = nlr & DMA2D_NLR_NL_MASK;
    uint32_t pixels_per_line = (nlr & DMA2D_NLR_PL_MASK) >> DMA2D_NLR_PL_SHIFT;
    uint32_t out_cm = s->regs[GNW_H7B0_DMA2D_OPFCCR >> 2] & DMA2D_OPFCCR_CM_MASK;
    int out_bpp = gnw_h7b0_dma2d_output_bpp(out_cm);
    hwaddr omar = s->regs[GNW_H7B0_DMA2D_OMAR >> 2];
    uint32_t oor = s->regs[GNW_H7B0_DMA2D_OOR >> 2];

    if (lines == 0 || pixels_per_line == 0) {
        return;
    }

    if (out_bpp == 0) {
        qemu_log_mask(LOG_UNIMP,
                      "gnw_h7b0_dma2d: unsupported output color mode %u, "
                      "not transferring\n", out_cm);
        return;
    }

    if (mode == DMA2D_MODE_R2M) {
        uint32_t ocolr = s->regs[GNW_H7B0_DMA2D_OCOLR >> 2];
        hwaddr out_stride = (hwaddr)(pixels_per_line + oor) * out_bpp;

        for (uint32_t y = 0; y < lines; y++) {
            hwaddr row = omar + (hwaddr)y * out_stride;

            for (uint32_t x = 0; x < pixels_per_line; x++) {
                gnw_h7b0_dma2d_write_output(out_cm, row + (hwaddr)x * out_bpp,
                                            ocolr);
            }
        }
        return;
    }

    /* M2M / M2M_PFC / M2M_BLEND all read a foreground buffer. */
    uint32_t fg_pfccr = s->regs[GNW_H7B0_DMA2D_FGPFCCR >> 2];
    uint32_t fg_cm = fg_pfccr & DMA2D_PFCCR_CM_MASK;
    hwaddr fg_mar = s->regs[GNW_H7B0_DMA2D_FGMAR >> 2];
    uint32_t fg_or = s->regs[GNW_H7B0_DMA2D_FGOR >> 2];
    hwaddr fg_clut = s->regs[GNW_H7B0_DMA2D_FGCMAR >> 2];
    int fg_bpp = 0;

    bool is_jpeg_hack = false;
    uint8_t *hack_rgb_buf = NULL;
    int hack_w = 0, hack_h = 0;

    if (fg_cm == 0xB) { /* DMA2D_INPUT_YCBCR */
        hack_rgb_buf = gnw_h7b0_jpeg_get_hack_buffer(&hack_w, &hack_h);
        if (hack_rgb_buf) {
            is_jpeg_hack = true;
            fg_bpp = 2; /* Treat as RGB565 */
        } else {
            qemu_log_mask(LOG_UNIMP,
                          "gnw_h7b0_dma2d: YCbCr foreground requested but no JPEG hack buffer available\n");
            return;
        }
    } else {
        if (!gnw_h7b0_dma2d_format_bpp(fg_cm, &fg_bpp)) {
            qemu_log_mask(LOG_UNIMP,
                          "gnw_h7b0_dma2d: unsupported foreground color mode "
                          "%u, not transferring\n", fg_cm);
            return;
        }
    }

    if (mode == DMA2D_MODE_M2M) {
        /*
         * Plain memory-to-memory, no pixel-format conversion: real
         * hardware just moves raw bytes (source and dest share the same
         * implicit format/bpp -- OPFCCR.CM is ignored in this mode per
         * the reference manual). Use the foreground bpp for both sides.
         */
        hwaddr fg_stride = (hwaddr)(pixels_per_line + fg_or) * fg_bpp;
        hwaddr out_stride = (hwaddr)(pixels_per_line + oor) * fg_bpp;
        hwaddr row_bytes = (hwaddr)pixels_per_line * fg_bpp;
        g_autofree uint8_t *linebuf = g_malloc(row_bytes);

        for (uint32_t y = 0; y < lines; y++) {
            cpu_physical_memory_read(fg_mar + (hwaddr)y * fg_stride, linebuf,
                                     row_bytes);
            cpu_physical_memory_write(omar + (hwaddr)y * out_stride, linebuf,
                                      row_bytes);
        }
        return;
    }

    hwaddr fg_stride = is_jpeg_hack ? (hwaddr)hack_w * 2 : (hwaddr)(pixels_per_line + fg_or) * fg_bpp;
    hwaddr out_stride = (hwaddr)(pixels_per_line + oor) * out_bpp;

    if (mode == DMA2D_MODE_M2M_PFC) {
        for (uint32_t y = 0; y < lines; y++) {
            hwaddr fg_row = fg_mar + (hwaddr)y * fg_stride;
            hwaddr out_row = omar + (hwaddr)y * out_stride;

            for (uint32_t x = 0; x < pixels_per_line; x++) {
                uint32_t argb;
                if (is_jpeg_hack) {
                    uint16_t px565 = lduw_le_p(hack_rgb_buf + y * fg_stride + x * 2);
                    unsigned int r5 = (px565 >> 11) & 0x1F;
                    unsigned int g6 = (px565 >> 5) & 0x3F;
                    unsigned int b5 = px565 & 0x1F;
                    unsigned int r8 = (r5 << 3) | (r5 >> 2);
                    unsigned int g8 = (g6 << 2) | (g6 >> 4);
                    unsigned int b8 = (b5 << 3) | (b5 >> 2);
                    argb = 0xFF000000U | (r8 << 16) | (g8 << 8) | b8;
                } else {
                    argb = gnw_h7b0_dma2d_read_argb8888(
                        fg_cm, fg_row + (hwaddr)x * fg_bpp, fg_clut);
                }
                unsigned int a = gnw_h7b0_dma2d_apply_alpha_mode(
                    fg_pfccr, (argb >> 24) & 0xFF);

                argb = (argb & 0x00FFFFFFU) | (a << 24);
                gnw_h7b0_dma2d_write_output(
                    out_cm, out_row + (hwaddr)x * out_bpp, argb);
            }
        }
        return;
    }

    if (mode == DMA2D_MODE_M2M_BLEND) {
        uint32_t bg_pfccr = s->regs[GNW_H7B0_DMA2D_BGPFCCR >> 2];
        uint32_t bg_cm = bg_pfccr & DMA2D_PFCCR_CM_MASK;
        hwaddr bg_mar = s->regs[GNW_H7B0_DMA2D_BGMAR >> 2];
        uint32_t bg_or = s->regs[GNW_H7B0_DMA2D_BGOR >> 2];
        hwaddr bg_clut = s->regs[GNW_H7B0_DMA2D_BGCMAR >> 2];
        int bg_bpp;

        if (!gnw_h7b0_dma2d_format_bpp(bg_cm, &bg_bpp)) {
            qemu_log_mask(LOG_UNIMP,
                          "gnw_h7b0_dma2d: unsupported background color "
                          "mode %u, not transferring\n", bg_cm);
            return;
        }

        hwaddr bg_stride = (hwaddr)(pixels_per_line + bg_or) * bg_bpp;

        for (uint32_t y = 0; y < lines; y++) {
            hwaddr fg_row = fg_mar + (hwaddr)y * fg_stride;
            hwaddr bg_row = bg_mar + (hwaddr)y * bg_stride;
            hwaddr out_row = omar + (hwaddr)y * out_stride;

            for (uint32_t x = 0; x < pixels_per_line; x++) {
                uint32_t fg;
                if (is_jpeg_hack) {
                    uint16_t px565 = lduw_le_p(hack_rgb_buf + y * fg_stride + x * 2);
                    unsigned int r5 = (px565 >> 11) & 0x1F;
                    unsigned int g6 = (px565 >> 5) & 0x3F;
                    unsigned int b5 = px565 & 0x1F;
                    unsigned int r8 = (r5 << 3) | (r5 >> 2);
                    unsigned int g8 = (g6 << 2) | (g6 >> 4);
                    unsigned int b8 = (b5 << 3) | (b5 >> 2);
                    fg = 0xFF000000U | (r8 << 16) | (g8 << 8) | b8;
                } else {
                    fg = gnw_h7b0_dma2d_read_argb8888(
                        fg_cm, fg_row + (hwaddr)x * fg_bpp, fg_clut);
                }
                uint32_t bg = gnw_h7b0_dma2d_read_argb8888(
                    bg_cm, bg_row + (hwaddr)x * bg_bpp, bg_clut);
                unsigned int fa = gnw_h7b0_dma2d_apply_alpha_mode(
                    fg_pfccr, (fg >> 24) & 0xFF);
                unsigned int ba = gnw_h7b0_dma2d_apply_alpha_mode(
                    bg_pfccr, (bg >> 24) & 0xFF);
                unsigned int fr = (fg >> 16) & 0xFF, fg_g = (fg >> 8) & 0xFF,
                             fb = fg & 0xFF;
                unsigned int br = (bg >> 16) & 0xFF, bg_g = (bg >> 8) & 0xFF,
                             bb = bg & 0xFF;
                /* Standard "over" compositing: out = fg*fa + bg*(1-fa),
                 * out_a = fa + ba*(1-fa). */
                unsigned int r = (fr * fa + br * (255 - fa)) / 255U;
                unsigned int g = (fg_g * fa + bg_g * (255 - fa)) / 255U;
                unsigned int b = (fb * fa + bb * (255 - fa)) / 255U;
                unsigned int a = fa + (ba * (255 - fa)) / 255U;
                uint32_t argb = (a << 24) | (r << 16) | (g << 8) | b;

                gnw_h7b0_dma2d_write_output(
                    out_cm, out_row + (hwaddr)x * out_bpp, argb);
            }
        }
        return;
    }
}

static void gnw_h7b0_dma2d_reset(DeviceState *dev)
{
    GnwH7B0Dma2dState *s = GNW_H7B0_DMA2D(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static uint64_t gnw_h7b0_dma2d_read(void *opaque, hwaddr addr,
                                     unsigned int size)
{
    GnwH7B0Dma2dState *s = GNW_H7B0_DMA2D(opaque);

    if (addr >= GNW_H7B0_DMA2D_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_dma2d_write(void *opaque, hwaddr addr, uint64_t val64,
                                  unsigned int size)
{
    GnwH7B0Dma2dState *s = GNW_H7B0_DMA2D(opaque);
    uint32_t value = val64;

    if (addr >= GNW_H7B0_DMA2D_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    switch (addr) {
    case GNW_H7B0_DMA2D_ISR:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: ISR is read-only on real hardware (use IFCR to "
                      "clear flags)\n", __func__);
        return;
    case GNW_H7B0_DMA2D_IFCR:
        /* Write-1-to-clear; IFCR bit positions mirror ISR's exactly. */
        s->regs[GNW_H7B0_DMA2D_ISR >> 2] &= ~value;
        gnw_h7b0_dma2d_update_irq(s);
        return;
    case GNW_H7B0_DMA2D_FGPFCCR:
    case GNW_H7B0_DMA2D_BGPFCCR:
        s->regs[addr >> 2] = value;
        if (value & DMA2D_PFCCR_START) {
            /*
             * CLUT "load" trigger: our model reads the CLUT directly out
             * of guest memory at transfer time (via FGCMAR/BGCMAR), so
             * there's nothing to actually copy -- just clear START
             * immediately so the driver's post-write poll
             * (HAL_DMA2D_CLUTLoad()'s companion poll) sees completion on
             * its first read, and raise CTCIF the way real hardware
             * signals CLUT-load-complete.
             */
            s->regs[addr >> 2] &= ~DMA2D_PFCCR_START;
            s->regs[GNW_H7B0_DMA2D_ISR >> 2] |= DMA2D_ISR_CTCIF;
            gnw_h7b0_dma2d_update_irq(s);
        }
        return;
    case GNW_H7B0_DMA2D_CR:
        s->regs[addr >> 2] = value & ~DMA2D_CR_START;
        if (value & DMA2D_CR_START) {
            gnw_h7b0_dma2d_do_transfer(s);
            s->regs[GNW_H7B0_DMA2D_ISR >> 2] |= DMA2D_ISR_TCIF;
            gnw_h7b0_dma2d_update_irq(s);
        }
        return;
    default:
        s->regs[addr >> 2] = value;
    }
}

static const MemoryRegionOps gnw_h7b0_dma2d_ops = {
    .read = gnw_h7b0_dma2d_read,
    .write = gnw_h7b0_dma2d_write,
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

static void gnw_h7b0_dma2d_init(Object *obj)
{
    GnwH7B0Dma2dState *s = GNW_H7B0_DMA2D(obj);

    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_dma2d_ops, s,
                           TYPE_GNW_H7B0_DMA2D, GNW_H7B0_DMA2D_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription vmstate_gnw_h7b0_dma2d = {
    .name = TYPE_GNW_H7B0_DMA2D,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0Dma2dState, GNW_H7B0_DMA2D_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_dma2d_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_dma2d;
    device_class_set_legacy_reset(dc, gnw_h7b0_dma2d_reset);
}

static const TypeInfo gnw_h7b0_dma2d_info = {
    .name          = TYPE_GNW_H7B0_DMA2D,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0Dma2dState),
    .instance_init = gnw_h7b0_dma2d_init,
    .class_init    = gnw_h7b0_dma2d_class_init,
};

static void gnw_h7b0_dma2d_register_types(void)
{
    type_register_static(&gnw_h7b0_dma2d_info);
}

type_init(gnw_h7b0_dma2d_register_types)
