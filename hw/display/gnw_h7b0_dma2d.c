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
    case DMA2D_INPUT_A8:
        *bpp = 1;
        return true;
    default:
        return false;
    }
}

/*
 * fixed_colr is only consulted for A8 (real hardware pairs an A8 alpha
 * mask with that layer's FGCOLR/BGCOLR fixed 24-bit RGB register --
 * previously dead/unread registers in this file, now wired in for real).
 */
static uint32_t gnw_h7b0_dma2d_read_argb8888(uint32_t cm, hwaddr addr,
                                              hwaddr clut_addr,
                                              uint32_t fixed_colr)
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
    case DMA2D_INPUT_A8: {
        uint8_t alpha;

        cpu_physical_memory_read(addr, &alpha, 1);
        return ((uint32_t)alpha << 24) | (fixed_colr & 0x00FFFFFFU);
    }
    default:
        return 0xFF000000U;
    }
}

/*
 * Real fetch for the JPEG device's YCbCr planes (replaces the old
 * "raw pointer hack" -- see docs/plan for the real-DOR-register JPEG
 * change this pairs with). Firmware's own polling loop
 * (HAL_JPEG_Decode()/JPEG_Process()) drains JPEG's DOR register into a
 * guest-RAM buffer at a firmware-controlled address, laid out as
 * y_plane || cb_plane || cr_plane (each plane_w*plane_h bytes) -- that
 * guest address is FGMAR here, exactly like every other input format.
 * Converts via standard BT.601 YCbCr->RGB (inverse of the JPEG device's
 * RGB->YCbCr conversion).
 */
static uint32_t gnw_h7b0_dma2d_read_ycbcr(hwaddr fg_mar, uint32_t plane_w,
                                          uint32_t plane_h, uint32_t chroma_w,
                                          uint32_t chroma_h, uint32_t x, uint32_t y)
{
    /*
     * The guest buffer is tightly packed: a full-resolution Y plane
     * (plane_w*plane_h bytes) followed by Cb/Cr planes subsampled to
     * chroma_w*chroma_h bytes each (per the image's real SOF0 H/V
     * sampling factors -- matching real hardware/firmware's expected
     * output size; serving unsubsampled chroma made our DOR output much
     * larger than firmware's destination buffer, which then only
     * partially drained it, leaving stale bytes in the tail -- visible
     * as banded corruption). Reading past the real width/height (the
     * DMA2D transfer's own NLR geometry can exceed the actual decoded
     * image, e.g. a smaller placeholder "no cover" image) would walk
     * into the next row's bytes (no row padding to skip) -- treat
     * out-of-bounds columns/rows as fully transparent instead.
     */
    if (x >= plane_w || y >= plane_h || chroma_w == 0 || chroma_h == 0) {
        return 0x00000000U;
    }

    uint32_t y_size = plane_w * plane_h;
    uint32_t c_size = chroma_w * chroma_h;
    uint32_t y_off = y * plane_w + x;
    uint32_t cx = x * chroma_w / plane_w;
    uint32_t cy = y * chroma_h / plane_h;
    if (cx >= chroma_w) cx = chroma_w - 1;
    if (cy >= chroma_h) cy = chroma_h - 1;
    uint32_t c_off = cy * chroma_w + cx;
    uint8_t yv, cb, cr;

    cpu_physical_memory_read(fg_mar + y_off, &yv, 1);
    cpu_physical_memory_read(fg_mar + y_size + c_off, &cb, 1);
    cpu_physical_memory_read(fg_mar + (hwaddr)y_size + c_size + c_off, &cr, 1);

    int yi = yv;
    int cbi = (int)cb - 128;
    int cri = (int)cr - 128;
    int r = yi + (int)(1.402 * cri);
    int g = yi - (int)(0.344136 * cbi) - (int)(0.714136 * cri);
    int b = yi + (int)(1.772 * cbi);

    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;

    return 0xFF000000U | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
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

/*
 * Shared "over" compositing helper, generalized out of the M2M_BLEND
 * branch so M2M_BLEND/M2M_BLEND_FG/M2M_BLEND_BG can all call the same
 * math instead of duplicating it. fg/bg are ARGB8888 with alpha already
 * resolved via gnw_h7b0_dma2d_apply_alpha_mode(); fa/ba are those
 * resolved alpha values (0-255).
 */
static uint32_t gnw_h7b0_dma2d_blend_over(uint32_t fg, unsigned int fa,
                                           uint32_t bg, unsigned int ba)
{
    unsigned int fr = (fg >> 16) & 0xFF, fgg = (fg >> 8) & 0xFF,
                 fb = fg & 0xFF;
    unsigned int br = (bg >> 16) & 0xFF, bgg = (bg >> 8) & 0xFF,
                 bb = bg & 0xFF;
    unsigned int r = (fr * fa + br * (255 - fa)) / 255U;
    unsigned int g = (fgg * fa + bgg * (255 - fa)) / 255U;
    unsigned int b = (fb * fa + bb * (255 - fa)) / 255U;
    unsigned int a = fa + (ba * (255 - fa)) / 255U;

    return (a << 24) | (r << 16) | (g << 8) | b;
}

static void gnw_h7b0_dma2d_update_irq(GnwH7B0Dma2dState *s)
{
    /*
     * All six ISR flags gate the one DMA2D interrupt line: ISR bits 0-5
     * (TEIF/TCIF/TWIF/CAEIF/CTCIF/CEIF) pair positionally with CR's IE
     * bits 8-13. Originally only TCIF/TEIF were checked, which left
     * stock Zelda's palette-fade transitions hung forever: its display
     * state machine starts a background CLUT load (BGPFCCR.START) with
     * CR.CTCIE enabled and sleeps until the CLUT-transfer-complete
     * interrupt -- CTCIF was set but never raised the line.
     */
    uint32_t isr = s->regs[GNW_H7B0_DMA2D_ISR >> 2];
    uint32_t cr = s->regs[GNW_H7B0_DMA2D_CR >> 2];
    bool pending = (isr & 0x3F & (cr >> 8)) != 0;

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
    uint32_t fg_colr = s->regs[GNW_H7B0_DMA2D_FGCOLR >> 2];
    int fg_bpp = 0;

    bool is_jpeg_ycbcr = false;
    uint32_t jpeg_w = 0, jpeg_h = 0;
    uint32_t jpeg_cw = 0, jpeg_ch = 0;

    if (fg_cm == 0xB) { /* DMA2D_INPUT_YCBCR */
        gnw_h7b0_jpeg_get_last_decoded_size(&jpeg_w, &jpeg_h);
        gnw_h7b0_jpeg_get_last_chroma_size(&jpeg_cw, &jpeg_ch);
        if (jpeg_w > 0 && jpeg_h > 0) {
            is_jpeg_ycbcr = true;
            fg_bpp = 1; /* One byte per plane per pixel; planes read separately. */
        } else {
            qemu_log_mask(LOG_UNIMP,
                          "gnw_h7b0_dma2d: YCbCr foreground requested but no decoded JPEG image available\n");
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

    hwaddr fg_stride = (hwaddr)(pixels_per_line + fg_or) * fg_bpp;
    hwaddr out_stride = (hwaddr)(pixels_per_line + oor) * out_bpp;

    if (mode == DMA2D_MODE_M2M_PFC) {
        for (uint32_t y = 0; y < lines; y++) {
            hwaddr fg_row = fg_mar + (hwaddr)y * fg_stride;
            hwaddr out_row = omar + (hwaddr)y * out_stride;

            for (uint32_t x = 0; x < pixels_per_line; x++) {
                uint32_t argb;
                if (is_jpeg_ycbcr) {
                    argb = gnw_h7b0_dma2d_read_ycbcr(fg_mar, jpeg_w, jpeg_h, jpeg_cw, jpeg_ch, x, y);
                } else {
                    argb = gnw_h7b0_dma2d_read_argb8888(
                        fg_cm, fg_row + (hwaddr)x * fg_bpp, fg_clut, fg_colr);
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

    /*
     * M2M_BLEND blends two real fetched buffers (FG+BG). M2M_BLEND_FG and
     * M2M_BLEND_BG are H7-specific "blend with fixed color" modes (see
     * sdk/.../stm32h7xx_hal_dma2d.h: "DMA2D memory to memory with blending
     * transfer mode and fixed color FG/BG") -- one side is a CONSTANT
     * 24-bit color from FGCOLR/BGCOLR, not a fetched buffer at all. Real
     * firmware (hw_jpeg_decoder.c's cover-art blend) uses BLEND_BG with
     * only FGPFCCR/FGMAR configured for the real YCbCr buffer; BGMAR is
     * never a valid pixel buffer in this mode on real hardware, so it must
     * not be read as one here -- doing so was reading uninitialized/stale
     * guest memory as the background on every transfer, producing garbage
     * pixels and severe flicker.
     */
    if (mode == DMA2D_MODE_M2M_BLEND || mode == DMA2D_MODE_M2M_BLEND_FG ||
        mode == DMA2D_MODE_M2M_BLEND_BG) {
        uint32_t bg_pfccr = s->regs[GNW_H7B0_DMA2D_BGPFCCR >> 2];
        uint32_t bg_cm = bg_pfccr & DMA2D_PFCCR_CM_MASK;
        hwaddr bg_mar = s->regs[GNW_H7B0_DMA2D_BGMAR >> 2];
        uint32_t bg_or = s->regs[GNW_H7B0_DMA2D_BGOR >> 2];
        hwaddr bg_clut = s->regs[GNW_H7B0_DMA2D_BGCMAR >> 2];
        uint32_t bg_colr = s->regs[GNW_H7B0_DMA2D_BGCOLR >> 2];
        uint32_t fg_colr_fixed = s->regs[GNW_H7B0_DMA2D_FGCOLR >> 2];
        bool fg_is_fixed = (mode == DMA2D_MODE_M2M_BLEND_FG);
        bool bg_is_fixed = (mode == DMA2D_MODE_M2M_BLEND_BG);
        int bg_bpp = 0;

        if (!bg_is_fixed && !gnw_h7b0_dma2d_format_bpp(bg_cm, &bg_bpp)) {
            qemu_log_mask(LOG_UNIMP,
                          "gnw_h7b0_dma2d: unsupported background color "
                          "mode %u, not transferring\n", bg_cm);
            return;
        }

        hwaddr bg_stride = bg_is_fixed ? 0
            : (hwaddr)(pixels_per_line + bg_or) * bg_bpp;

        for (uint32_t y = 0; y < lines; y++) {
            hwaddr fg_row = fg_mar + (hwaddr)y * fg_stride;
            hwaddr bg_row = bg_is_fixed ? 0 : bg_mar + (hwaddr)y * bg_stride;
            hwaddr out_row = omar + (hwaddr)y * out_stride;

            for (uint32_t x = 0; x < pixels_per_line; x++) {
                uint32_t fg;
                if (fg_is_fixed) {
                    fg = 0xFF000000U | (fg_colr_fixed & 0x00FFFFFFU);
                } else if (is_jpeg_ycbcr) {
                    fg = gnw_h7b0_dma2d_read_ycbcr(fg_mar, jpeg_w, jpeg_h, jpeg_cw, jpeg_ch, x, y);
                } else {
                    fg = gnw_h7b0_dma2d_read_argb8888(
                        fg_cm, fg_row + (hwaddr)x * fg_bpp, fg_clut, fg_colr);
                }
                uint32_t bg = bg_is_fixed
                    ? (0xFF000000U | (bg_colr & 0x00FFFFFFU))
                    : gnw_h7b0_dma2d_read_argb8888(
                          bg_cm, bg_row + (hwaddr)x * bg_bpp, bg_clut, bg_colr);
                unsigned int fa = gnw_h7b0_dma2d_apply_alpha_mode(
                    fg_pfccr, (fg >> 24) & 0xFF);
                unsigned int ba = gnw_h7b0_dma2d_apply_alpha_mode(
                    bg_pfccr, (bg >> 24) & 0xFF);
                uint32_t argb = gnw_h7b0_dma2d_blend_over(fg, fa, bg, ba);

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

static void gnw_h7b0_dma2d_class_init(ObjectClass *klass, const void *data)
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
