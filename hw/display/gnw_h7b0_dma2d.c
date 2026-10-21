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
#include "qapi/visitor.h"
#include "migration/vmstate.h"
#include "exec/cpu-common.h"
#include "exec/gnw-cycle-model.h"
#include "hw/core/qdev-properties.h"
#include "hw/display/gnw_h7b0_dma2d.h"
#include "hw/misc/gnw_h7b0_jpeg.h"

/* The default functional path below remains synchronous.  Timed execution
 * requires an explicit, versioned service profile.  This file ships only a
 * hidden deterministic qtest profile; it makes no hardware throughput claim.
 */
#define DMA2D_TEST_PROFILE_ID UINT32_C(0x44324401)
#define DMA_ENGINE_R1_RECEIPT_SHA256 \
    "a4cbac646ba40c21f0e345f5f83124cac638e5c42dcfbc782ec54559bddfeb8e"
#define DMA2D_DMA_ENGINE_R1_PROFILE_ID UINT32_C(0x44325201)
/* Explicit candidate bound to dma2d-poll-fit-holdout-r4's physical score.
 * The score itself keeps runtime_authorized=false, so the SoC never enables
 * this profile automatically.  A caller must opt in with both properties;
 * even then, the descriptor guard below accepts only the exact measured
 * clock, memory placement, formats and FIT/HOLDOUT geometries. */
#define DMA2D_POLL_R4_PROFILE_ID UINT32_C(0x44325204)
#define DMA2D_POLL_R4_RECEIPT_SHA256 \
    "6570f12a249b72e512f859285c4dcd4c20b96ed48ed5ff4d55f4d240655ec481"
#define DMA2D_POLL_R4_REFERENCE_HZ UINT32_C(280000000)
#define DMA2D_POLL_R4_SETUP_NUM UINT64_C(7339840)
#define DMA2D_POLL_R4_PIXEL_NUM UINT32_C(65285)
#define DMA2D_POLL_R4_DEN UINT32_C(65216)
#define DMA2D_POLL_R4_SOURCE UINT32_C(0x24020000)
#define DMA2D_POLL_R4_DESTINATION UINT32_C(0x24040000)
#define DMA2D_POLL_R6_PROFILE_ID UINT32_C(0x44325206)
#define DMA2D_POLL_R6_SCOPE_SHA256 \
    "1d7d4972b2ea367a0c08abe391fedb0468f28075ea18799d8855dcc255fd67d2"
#define DMA2D_SETTLE_BUDGET 64
#define DMA2D_LWR_LINE_MASK 0xffffU
#define AXI_SRAM_BASE UINT64_C(0x24000000)
#define AXI_SRAM_END  UINT64_C(0x24100000)

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
 * Buffer-based counterpart of the per-pixel physical-memory pixel decoder
 * this replaced (see git history if the old cpu_physical_memory_read()-per-
 * pixel version is ever needed for reference) -- decodes one pixel already
 * sitting in a row buffer the caller batch-fetched with a
 * single cpu_physical_memory_read() per row, instead of this function doing
 * its own cpu_physical_memory_read() per pixel. clut256, if non-NULL, is a
 * 256-entry ARGB8888 table pre-loaded once per transfer (not once per pixel)
 * by gnw_h7b0_dma2d_load_clut() -- see gnw_h7b0_dma2d_do_transfer()'s row
 * loops for why (perf profiling during 2026-07-13's LTDC investigation
 * found the same per-pixel-cpu_physical_memory_read pattern to be a large,
 * avoidable chunk of total emulation CPU time; DMA2D had the identical
 * pattern, worse here since L8's CLUT lookup was a *second* physical read
 * per pixel on top of the pixel data itself).
 */
static uint32_t gnw_h7b0_dma2d_read_argb8888_buf(uint32_t cm,
                                                  const uint8_t *buf,
                                                  const uint32_t *clut256,
                                                  uint32_t fixed_colr)
{
    switch (cm) {
    case DMA2D_INPUT_ARGB8888:
        return ldl_le_p(buf);
    case DMA2D_INPUT_RGB565: {
        uint16_t px = lduw_le_p(buf);
        unsigned int r5 = (px >> 11) & 0x1F, g6 = (px >> 5) & 0x3F,
                     b5 = px & 0x1F;
        unsigned int r8 = (r5 << 3) | (r5 >> 2);
        unsigned int g8 = (g6 << 2) | (g6 >> 4);
        unsigned int b8 = (b5 << 3) | (b5 >> 2);
        return 0xFF000000U | (r8 << 16) | (g8 << 8) | b8;
    }
    case DMA2D_INPUT_ARGB1555: {
        uint16_t px = lduw_le_p(buf);
        unsigned int a1 = (px >> 15) & 0x1;
        unsigned int r5 = (px >> 10) & 0x1F, g5 = (px >> 5) & 0x1F,
                     b5 = px & 0x1F;
        unsigned int r8 = (r5 << 3) | (r5 >> 2);
        unsigned int g8 = (g5 << 3) | (g5 >> 2);
        unsigned int b8 = (b5 << 3) | (b5 >> 2);
        return (a1 ? 0xFF000000U : 0U) | (r8 << 16) | (g8 << 8) | b8;
    }
    case DMA2D_INPUT_ARGB4444: {
        uint16_t px = lduw_le_p(buf);
        unsigned int a4 = (px >> 12) & 0xF, r4 = (px >> 8) & 0xF,
                     g4 = (px >> 4) & 0xF, b4 = px & 0xF;
        return ((a4 * 0x11U) << 24) | ((r4 * 0x11U) << 16) |
               ((g4 * 0x11U) << 8) | (b4 * 0x11U);
    }
    case DMA2D_INPUT_L8: {
        uint8_t idx = buf[0];

        if (!clut256) {
            return 0xFF000000U | (idx << 16) | (idx << 8) | idx;
        }
        return clut256[idx];
    }
    case DMA2D_INPUT_A8:
        return ((uint32_t)buf[0] << 24) | (fixed_colr & 0x00FFFFFFU);
    default:
        return 0xFF000000U;
    }
}

/*
 * Pre-loads a 256-entry ARGB8888 CLUT from guest memory in one bulk read,
 * instead of gnw_h7b0_dma2d_read_argb8888()'s previous per-pixel
 * cpu_physical_memory_read(clut_addr + idx*4). Returns false (nothing
 * loaded) if clut_addr is 0, matching the existing "no CLUT loaded" fallback
 * semantics.
 */
static bool gnw_h7b0_dma2d_load_clut(hwaddr clut_addr, uint32_t *clut256_out)
{
    if (clut_addr == 0) {
        return false;
    }

    g_autofree uint8_t *raw = g_malloc(256 * 4);
    cpu_physical_memory_read(clut_addr, raw, 256 * 4);
    for (int i = 0; i < 256; i++) {
        clut256_out[i] = ldl_le_p(raw + i * 4);
    }
    return true;
}

/*
 * Real fetch for the JPEG device's YCbCr planes: firmware's own polling
 * loop (HAL_JPEG_Decode()/JPEG_Process()) drains JPEG's DOR register into a
 * guest-RAM buffer at a firmware-controlled address, laid out as
 * y_plane || cb_plane || cr_plane (each plane_w*plane_h bytes, chroma
 * planes subsampled to chroma_w*chroma_h per the image's real SOF0 H/V
 * sampling factors) -- that guest address is FGMAR, exactly like every
 * other input format. Converts via standard BT.601 YCbCr->RGB (inverse of
 * the JPEG device's RGB->YCbCr conversion).
 *
 * Buffer-based: decodes one
 * pixel from Y/Cb/Cr row buffers the caller batch-fetched once per output
 * row (one cpu_physical_memory_read() each), instead of three separate
 * per-pixel physical reads (this was the worst offender of the per-pixel
 * MMIO pattern: 3 calls/pixel instead of 1, directly on the coverflow
 * cover-art path STATUS.md already flags as the most expensive on-screen
 * content). x is the column within the row buffers (row buffers already
 * account for x >= plane_w via the caller not fetching/calling this at
 * all for out-of-range columns).
 */
static uint32_t gnw_h7b0_dma2d_read_ycbcr_buf(const uint8_t *y_row,
                                               const uint8_t *cb_row,
                                               const uint8_t *cr_row,
                                               uint32_t plane_w,
                                               uint32_t chroma_w, uint32_t x)
{
    uint32_t cx = x * chroma_w / plane_w;
    if (cx >= chroma_w) {
        cx = chroma_w - 1;
    }

    int yi = y_row[x];
    int cbi = (int)cb_row[cx] - 128;
    int cri = (int)cr_row[cx] - 128;
    /* Q16 fixed-point BT.601 constants, replaces float math for perf */
    const int32_t c_r = 91881, c_gb = 22553, c_gr = 46802, c_b = 116130;
    int r = yi + (c_r * cri >= 0 ? (c_r * cri) >> 16 : -((-(c_r * cri)) >> 16));
    int g = yi - (c_gb * cbi >= 0 ? (c_gb * cbi) >> 16 : -((-(c_gb * cbi)) >> 16))
               - (c_gr * cri >= 0 ? (c_gr * cri) >> 16 : -((-(c_gr * cri)) >> 16));
    int b = yi + (c_b * cbi >= 0 ? (c_b * cbi) >> 16 : -((-(c_b * cbi)) >> 16));

    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;

    return 0xFF000000U | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/*
 * Buffer-based output pixel encoder -- encodes one
 * pixel into an output row buffer the caller batch-writes once per row
 * (one cpu_physical_memory_write() each) instead of one physical write per
 * pixel.
 */
static void gnw_h7b0_dma2d_write_output_buf(uint32_t cm, uint8_t *buf,
                                             uint32_t argb8888)
{
    unsigned int a = (argb8888 >> 24) & 0xFF;
    unsigned int r = (argb8888 >> 16) & 0xFF;
    unsigned int g = (argb8888 >> 8) & 0xFF;
    unsigned int b = argb8888 & 0xFF;

    switch (cm) {
    case DMA2D_OUTPUT_ARGB8888:
        stl_le_p(buf, argb8888);
        break;
    case DMA2D_OUTPUT_RGB565:
        stw_le_p(buf, (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) |
                                  (b >> 3)));
        break;
    case DMA2D_OUTPUT_ARGB1555:
        stw_le_p(buf, (uint16_t)(((a & 0x80) << 8) | ((r & 0xF8) << 7) |
                                  ((g & 0xF8) << 2) | (b >> 3)));
        break;
    case DMA2D_OUTPUT_ARGB4444:
        stw_le_p(buf, (uint16_t)(((a & 0xF0) << 8) | ((r & 0xF0) << 4) |
                                  (g & 0xF0) | (b >> 4)));
        break;
    default:
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
        /*
         * OCOLR is NOT an ARGB8888 value here -- RM0455 18.5.15-18.5.18
         * documents OCOLR as a set of *alternate* register layouts, one
         * per output color mode selected by OPFCCR.CM: "The same register
         * is used to show the color values, with different formats
         * depending on the color mode" -- and 18.3.9 spells out that in
         * R2M mode "the configured output rectangle is filled by the
         * color specified in DMA2D_OCOLR which contains a fixed 32-, 24-,
         * or 16-bit value. The format is selected by CM[2:0] in
         * DMA2D_OPFCCR." I.e. real firmware (see
         * sdk/stm32h7xx-hal-driver/Src/stm32h7xx_hal_dma2d.c's
         * DMA2D_SetConfig()) pre-packs the fill color into the *output*
         * format's bit layout before writing OCOLR (e.g. for RGB565 it
         * writes a 16-bit RGB565 value into the low 16 bits), and real
         * hardware just copies OCOLR's raw low out_bpp bytes straight to
         * memory for every pixel -- no further ARGB8888->CM conversion
         * happens. Previously this code fed OCOLR into
         * gnw_h7b0_dma2d_write_output_buf() (which assumes an ARGB8888
         * input and re-encodes to the output format), applying that
         * conversion a second time and corrupting every R2M fill color
         * whose output format isn't ARGB8888.
         */
        uint32_t ocolr = s->regs[GNW_H7B0_DMA2D_OCOLR >> 2];
        hwaddr out_stride = (hwaddr)(pixels_per_line + oor) * out_bpp;
        hwaddr row_bytes = (hwaddr)pixels_per_line * out_bpp;
        /* Fill color is constant for the whole transfer -- build one row
         * of encoded pixels once and reuse it for every line, instead of
         * re-encoding (and separately physical-writing) the same pixel
         * pixels_per_line*lines times. */
        g_autofree uint8_t *rowbuf = g_malloc(row_bytes);

        for (uint32_t x = 0; x < pixels_per_line; x++) {
            uint8_t *dst = rowbuf + (hwaddr)x * out_bpp;
            if (out_bpp == 4) {
                stl_le_p(dst, ocolr);
            } else {
                stw_le_p(dst, (uint16_t)ocolr);
            }
        }
        for (uint32_t y = 0; y < lines; y++) {
            cpu_physical_memory_write(omar + (hwaddr)y * out_stride, rowbuf,
                                      row_bytes);
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
        uint32_t clut256[256];
        bool have_clut = !is_jpeg_ycbcr &&
                          gnw_h7b0_dma2d_load_clut(fg_clut, clut256);
        hwaddr out_row_bytes = (hwaddr)pixels_per_line * out_bpp;
        g_autofree uint8_t *fg_linebuf = is_jpeg_ycbcr ? NULL
            : g_malloc((hwaddr)pixels_per_line * fg_bpp);
        g_autofree uint8_t *out_linebuf = g_malloc(out_row_bytes);
        g_autofree uint8_t *y_row = is_jpeg_ycbcr ? g_malloc(jpeg_w) : NULL;
        g_autofree uint8_t *cb_row = is_jpeg_ycbcr ? g_malloc(jpeg_cw) : NULL;
        g_autofree uint8_t *cr_row = is_jpeg_ycbcr ? g_malloc(jpeg_cw) : NULL;

        for (uint32_t y = 0; y < lines; y++) {
            hwaddr out_row = omar + (hwaddr)y * out_stride;
            bool row_in_bounds = true;

            if (is_jpeg_ycbcr) {
                row_in_bounds = y < jpeg_h;
                if (row_in_bounds) {
                    uint32_t y_size = jpeg_w * jpeg_h;
                    uint32_t c_size = jpeg_cw * jpeg_ch;
                    uint32_t cy = y * jpeg_ch / jpeg_h;
                    if (cy >= jpeg_ch) {
                        cy = jpeg_ch - 1;
                    }
                    cpu_physical_memory_read(fg_mar + (hwaddr)y * jpeg_w,
                                             y_row, jpeg_w);
                    cpu_physical_memory_read(
                        fg_mar + y_size + (hwaddr)cy * jpeg_cw, cb_row,
                        jpeg_cw);
                    cpu_physical_memory_read(
                        fg_mar + (hwaddr)y_size + c_size + (hwaddr)cy * jpeg_cw,
                        cr_row, jpeg_cw);
                }
            } else {
                cpu_physical_memory_read(fg_mar + (hwaddr)y * fg_stride,
                                         fg_linebuf,
                                         (hwaddr)pixels_per_line * fg_bpp);
            }

            for (uint32_t x = 0; x < pixels_per_line; x++) {
                uint32_t argb;
                if (is_jpeg_ycbcr) {
                    argb = (row_in_bounds && x < jpeg_w)
                        ? gnw_h7b0_dma2d_read_ycbcr_buf(y_row, cb_row, cr_row,
                                                        jpeg_w, jpeg_cw, x)
                        : 0x00000000U;
                } else {
                    argb = gnw_h7b0_dma2d_read_argb8888_buf(
                        fg_cm, fg_linebuf + (hwaddr)x * fg_bpp,
                        have_clut ? clut256 : NULL, fg_colr);
                }
                unsigned int a = gnw_h7b0_dma2d_apply_alpha_mode(
                    fg_pfccr, (argb >> 24) & 0xFF);

                argb = (argb & 0x00FFFFFFU) | (a << 24);
                gnw_h7b0_dma2d_write_output_buf(
                    out_cm, out_linebuf + (hwaddr)x * out_bpp, argb);
            }
            cpu_physical_memory_write(out_row, out_linebuf, out_row_bytes);
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

        uint32_t fg_clut256[256], bg_clut256[256];
        bool have_fg_clut = !fg_is_fixed && !is_jpeg_ycbcr &&
                             gnw_h7b0_dma2d_load_clut(fg_clut, fg_clut256);
        bool have_bg_clut = !bg_is_fixed &&
                             gnw_h7b0_dma2d_load_clut(bg_clut, bg_clut256);
        hwaddr out_row_bytes = (hwaddr)pixels_per_line * out_bpp;
        g_autofree uint8_t *fg_linebuf =
            (fg_is_fixed || is_jpeg_ycbcr) ? NULL
                : g_malloc((hwaddr)pixels_per_line * fg_bpp);
        g_autofree uint8_t *bg_linebuf = bg_is_fixed ? NULL
            : g_malloc((hwaddr)pixels_per_line * bg_bpp);
        g_autofree uint8_t *out_linebuf = g_malloc(out_row_bytes);
        g_autofree uint8_t *y_row = is_jpeg_ycbcr ? g_malloc(jpeg_w) : NULL;
        g_autofree uint8_t *cb_row = is_jpeg_ycbcr ? g_malloc(jpeg_cw) : NULL;
        g_autofree uint8_t *cr_row = is_jpeg_ycbcr ? g_malloc(jpeg_cw) : NULL;

        for (uint32_t y = 0; y < lines; y++) {
            hwaddr out_row = omar + (hwaddr)y * out_stride;
            bool row_in_bounds = true;

            if (is_jpeg_ycbcr) {
                row_in_bounds = y < jpeg_h;
                if (row_in_bounds) {
                    uint32_t y_size = jpeg_w * jpeg_h;
                    uint32_t c_size = jpeg_cw * jpeg_ch;
                    uint32_t cy = y * jpeg_ch / jpeg_h;
                    if (cy >= jpeg_ch) {
                        cy = jpeg_ch - 1;
                    }
                    cpu_physical_memory_read(fg_mar + (hwaddr)y * jpeg_w,
                                             y_row, jpeg_w);
                    cpu_physical_memory_read(
                        fg_mar + y_size + (hwaddr)cy * jpeg_cw, cb_row,
                        jpeg_cw);
                    cpu_physical_memory_read(
                        fg_mar + (hwaddr)y_size + c_size + (hwaddr)cy * jpeg_cw,
                        cr_row, jpeg_cw);
                }
            } else if (!fg_is_fixed) {
                cpu_physical_memory_read(fg_mar + (hwaddr)y * fg_stride,
                                         fg_linebuf,
                                         (hwaddr)pixels_per_line * fg_bpp);
            }
            if (!bg_is_fixed) {
                cpu_physical_memory_read(bg_mar + (hwaddr)y * bg_stride,
                                         bg_linebuf,
                                         (hwaddr)pixels_per_line * bg_bpp);
            }

            for (uint32_t x = 0; x < pixels_per_line; x++) {
                uint32_t fg;
                if (fg_is_fixed) {
                    fg = 0xFF000000U | (fg_colr_fixed & 0x00FFFFFFU);
                } else if (is_jpeg_ycbcr) {
                    fg = (row_in_bounds && x < jpeg_w)
                        ? gnw_h7b0_dma2d_read_ycbcr_buf(y_row, cb_row, cr_row,
                                                        jpeg_w, jpeg_cw, x)
                        : 0x00000000U;
                } else {
                    fg = gnw_h7b0_dma2d_read_argb8888_buf(
                        fg_cm, fg_linebuf + (hwaddr)x * fg_bpp,
                        have_fg_clut ? fg_clut256 : NULL, fg_colr);
                }
                uint32_t bg = bg_is_fixed
                    ? (0xFF000000U | (bg_colr & 0x00FFFFFFU))
                    : gnw_h7b0_dma2d_read_argb8888_buf(
                          bg_cm, bg_linebuf + (hwaddr)x * bg_bpp,
                          have_bg_clut ? bg_clut256 : NULL, bg_colr);
                unsigned int fa = gnw_h7b0_dma2d_apply_alpha_mode(
                    fg_pfccr, (fg >> 24) & 0xFF);
                unsigned int ba = gnw_h7b0_dma2d_apply_alpha_mode(
                    bg_pfccr, (bg >> 24) & 0xFF);
                uint32_t argb = gnw_h7b0_dma2d_blend_over(fg, fa, bg, ba);

                gnw_h7b0_dma2d_write_output_buf(
                    out_cm, out_linebuf + (hwaddr)x * out_bpp, argb);
            }
            cpu_physical_memory_write(out_row, out_linebuf, out_row_bytes);
        }
        return;
    }
}

static void gnw_h7b0_dma2d_arm(GnwH7B0Dma2dState *s);

uint32_t gnw_h7b0_dma2d_contends_for_range(void *opaque, uint64_t address,
                                           unsigned size)
{
    GnwH7B0Dma2dState *s = opaque;
    bool source_axi, destination_axi;

    if (!s->active || !size || address < AXI_SRAM_BASE ||
        address >= AXI_SRAM_END || size > AXI_SRAM_END - address) {
        return 0;
    }
    source_axi = s->active_mode == DMA2D_MODE_M2M &&
                 s->source >= AXI_SRAM_BASE && s->source < AXI_SRAM_END;
    destination_axi = s->destination >= AXI_SRAM_BASE &&
                      s->destination < AXI_SRAM_END;
    return source_axi || destination_axi ? GNW_M7_BUS_MASTER_DMA2D : 0;
}

static bool gnw_h7b0_dma2d_phase_need(
    uint32_t setup_cycles, uint32_t pixels,
    const GnwH7B0Dma2dTimingProfile *profile, uint64_t *need)
{
    unsigned __int128 value =
        (unsigned __int128)setup_cycles * profile->cycles_per_pixel_den +
        (unsigned __int128)pixels * profile->cycles_per_pixel_num +
        profile->cycles_per_line_num;

    if (!value || value > UINT64_MAX) {
        return false;
    }
    *need = value;
    return true;
}

static bool gnw_h7b0_dma2d_mode_phase_needs(
    GnwH7B0Dma2dModeTiming timing, uint32_t pixels,
    uint64_t *regular_need, uint64_t *first_need)
{
    unsigned __int128 regular =
        (unsigned __int128)pixels * timing.cycles_per_pixel_num;
    unsigned __int128 first = timing.setup_num + regular;

    if (!timing.cycles_per_pixel_den || !regular ||
        regular > UINT64_MAX || first > UINT64_MAX) {
        return false;
    }
    *regular_need = regular;
    *first_need = first;
    return true;
}

static bool gnw_h7b0_dma2d_poll_r4_geometry(uint32_t lines,
                                             uint32_t pixels)
{
    static const struct {
        uint16_t lines;
        uint16_t pixels;
    } geometries[] = {
        { 16, 48 }, { 3, 256 }, { 32, 64 }, { 2, 1024 },
        { 64, 64 }, { 2, 2048 }, { 64, 96 }, { 2, 3072 },
        { 16, 80 }, { 2, 640 }, { 40, 64 }, { 2, 1280 },
        { 72, 64 }, { 2, 2304 },
    };

    for (size_t i = 0; i < ARRAY_SIZE(geometries); i++) {
        if (geometries[i].lines == lines && geometries[i].pixels == pixels) {
            return true;
        }
    }
    return false;
}

static bool gnw_h7b0_dma2d_poll_r6_geometry(uint32_t lines,
                                             uint32_t pixels)
{
    return (lines == 32 && pixels == 56) ||
           (lines == 2 && pixels == 896) ||
           (lines == 64 && pixels == 56) ||
           (lines == 2 && pixels == 1792) ||
           (lines == 64 && pixels == 88) ||
           (lines == 2 && pixels == 2816);
}

static void gnw_h7b0_dma2d_phase_add(GnwH7B0Dma2dState *s,
                                      uint64_t elapsed)
{
    uint32_t rate = s->evidence_reference_hz ?: s->clock_hz;
    uint32_t den = s->active_timing.cycles_per_pixel_den ?:
                   s->profile.cycles_per_pixel_den;
    unsigned __int128 scaled = (unsigned __int128)elapsed * rate * den +
                               s->phase_subcycle;
    unsigned __int128 whole = scaled / NANOSECONDS_PER_SECOND;

    s->phase_subcycle = scaled % NANOSECONDS_PER_SECOND;
    s->phase_progress = whole > UINT64_MAX - s->phase_progress ?
                        UINT64_MAX : s->phase_progress + whole;
}

/* Commit exactly one completed output line.  Timed mode intentionally starts
 * with the two operation classes for which the current functional model has
 * a direct line worker.  Conversion, blend and CLUT traffic remain visible as
 * unsupported and use the unchanged synchronous fallback. */
static void gnw_h7b0_dma2d_commit_line(GnwH7B0Dma2dState *s)
{
    hwaddr dst = s->destination +
                 (hwaddr)s->current_line * s->destination_stride;
    hwaddr row_bytes = (hwaddr)s->pixels_per_line * s->bytes_per_pixel;

    if (s->active_mode == DMA2D_MODE_R2M) {
        g_autofree uint8_t *row = g_malloc(row_bytes);

        for (uint32_t x = 0; x < s->pixels_per_line; x++) {
            uint8_t *pixel = row + (hwaddr)x * s->bytes_per_pixel;

            if (s->bytes_per_pixel == 4) {
                stl_le_p(pixel, s->fill_color);
            } else {
                stw_le_p(pixel, (uint16_t)s->fill_color);
            }
        }
        cpu_physical_memory_write(dst, row, row_bytes);
    } else {
        hwaddr src = s->source +
                     (hwaddr)s->current_line * s->source_stride;
        g_autofree uint8_t *row = g_malloc(row_bytes);

        cpu_physical_memory_read(src, row, row_bytes);
        cpu_physical_memory_write(dst, row, row_bytes);
    }

    s->current_line++;
    if ((s->regs[GNW_H7B0_DMA2D_LWR >> 2] & DMA2D_LWR_LINE_MASK) ==
        s->current_line) {
        s->regs[GNW_H7B0_DMA2D_ISR >> 2] |= DMA2D_ISR_TWIF;
    }
    if (s->current_line == s->total_lines) {
        s->active = false;
        s->regs[GNW_H7B0_DMA2D_CR >> 2] &= ~DMA2D_CR_START;
        s->regs[GNW_H7B0_DMA2D_ISR >> 2] |= DMA2D_ISR_TCIF;
        timer_del(s->timer);
        gnw_h7b0_dma2d_update_irq(s);
        return;
    }
    if (s->active_timing.cycles_per_pixel_den) {
        s->phase_need = (uint64_t)s->pixels_per_line *
                        s->active_timing.cycles_per_pixel_num;
    } else {
        g_assert(gnw_h7b0_dma2d_phase_need(0, s->pixels_per_line,
                                           &s->profile, &s->phase_need));
    }
    gnw_h7b0_dma2d_update_irq(s);
}

static bool gnw_h7b0_dma2d_settle(GnwH7B0Dma2dState *s, int64_t now)
{
    unsigned events = 0;

    if (!s->active || now < s->last_update_ns) {
        return true;
    }
    if (!s->clock_known || !s->clock_hz) {
        s->last_update_ns = now;
        return true;
    }
    while (s->active) {
        if (s->phase_progress >= s->phase_need) {
            s->phase_progress -= s->phase_need;
            gnw_h7b0_dma2d_commit_line(s);
            if (++events == DMA2D_SETTLE_BUDGET) {
                return false;
            }
            continue;
        }
        uint64_t elapsed = now - s->last_update_ns;
        uint64_t need = s->phase_need - s->phase_progress;
        unsigned __int128 numerator =
            (unsigned __int128)need * NANOSECONDS_PER_SECOND -
            s->phase_subcycle;
        unsigned __int128 denominator =
            (unsigned __int128)(s->evidence_reference_hz ?: s->clock_hz) *
            (s->active_timing.cycles_per_pixel_den ?:
             s->profile.cycles_per_pixel_den);
        unsigned __int128 delta128 =
            (numerator + denominator - 1) / denominator;
        uint64_t delta = delta128 > INT64_MAX ? INT64_MAX : delta128;

        if (elapsed < delta) {
            gnw_h7b0_dma2d_phase_add(s, elapsed);
            s->last_update_ns = now;
            return true;
        }
        gnw_h7b0_dma2d_phase_add(s, delta);
        s->last_update_ns += delta;
    }
    return true;
}

static void gnw_h7b0_dma2d_arm(GnwH7B0Dma2dState *s)
{
    if (!s->active || !s->clock_known || !s->clock_hz) {
        timer_del(s->timer);
        return;
    }
    if (s->phase_progress >= s->phase_need) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        return;
    }
    uint64_t need = s->phase_need - s->phase_progress;
    unsigned __int128 numerator =
        (unsigned __int128)need * NANOSECONDS_PER_SECOND -
        s->phase_subcycle;
    unsigned __int128 denominator =
        (unsigned __int128)(s->evidence_reference_hz ?: s->clock_hz) *
        (s->active_timing.cycles_per_pixel_den ?:
         s->profile.cycles_per_pixel_den);
    unsigned __int128 delay = (numerator + denominator - 1) / denominator;
    int64_t deadline = delay > INT64_MAX - s->last_update_ns ?
                       INT64_MAX : s->last_update_ns + (int64_t)delay;

    timer_mod(s->timer, deadline);
}

static void gnw_h7b0_dma2d_tick(void *opaque)
{
    GnwH7B0Dma2dState *s = opaque;

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!gnw_h7b0_dma2d_settle(s, now)) {
        timer_mod(s->timer, now);
    } else {
        gnw_h7b0_dma2d_arm(s);
    }
}

void gnw_h7b0_dma2d_set_timing_profile(
    GnwH7B0Dma2dState *s, const GnwH7B0Dma2dTimingProfile *profile)
{
    g_assert(!DEVICE(s)->realized);
    s->profile = profile ? *profile : (GnwH7B0Dma2dTimingProfile) { 0 };
    s->migration_profile_identity = s->profile.identity;
}

void gnw_h7b0_dma2d_set_clock_info(GnwH7B0Dma2dState *s, bool known,
                                    uint32_t reference_hz,
                                    bool gate_enabled)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    while (!gnw_h7b0_dma2d_settle(s, now)) {
        /* At most NLR.NL (65535) line commits; preserve the old clock for
         * every completed line before installing the new rate. */
    }
    if (s->active && s->evidence_required_clock_hz &&
        (!known || !gate_enabled ||
         reference_hz != s->evidence_required_clock_hz)) {
        s->unsupported_count++;
        s->active = false;
        s->regs[GNW_H7B0_DMA2D_CR >> 2] &= ~DMA2D_CR_START;
        s->regs[GNW_H7B0_DMA2D_ISR >> 2] |= DMA2D_ISR_TEIF;
        timer_del(s->timer);
        gnw_h7b0_dma2d_update_irq(s);
    }
    if (!known && s->active) {
        s->unsupported_count++;
        qemu_log_mask(LOG_UNIMP,
                      "gnw-h7b0-dma2d: active timed transfer lost known "
                      "engine clock; phase paused\n");
    }
    s->clock_known = known;
    s->clock_gate_enabled = gate_enabled;
    s->clock_hz = known && gate_enabled ? reference_hz : 0;
    gnw_h7b0_dma2d_arm(s);
}

#define GNW_DMA2D_AXI_SRAM_BASE UINT32_C(0x24000000)
#define GNW_DMA2D_AXI_SRAM_BYTES UINT32_C(0x00100000)

static bool gnw_h7b0_dma2d_axi_range(uint32_t address, uint64_t bytes)
{
    uint64_t end = (uint64_t)address + bytes;

    return bytes && address >= GNW_DMA2D_AXI_SRAM_BASE &&
           end <= (uint64_t)GNW_DMA2D_AXI_SRAM_BASE +
                  GNW_DMA2D_AXI_SRAM_BYTES;
}

typedef enum GnwDma2dTimedStart {
    GNW_DMA2D_TIMED_DISABLED,
    GNW_DMA2D_TIMED_STARTED,
    GNW_DMA2D_TIMED_REJECTED,
} GnwDma2dTimedStart;

static GnwDma2dTimedStart gnw_h7b0_dma2d_reject_timed(
    GnwH7B0Dma2dState *s, const char *reason)
{
    s->unsupported_count++;
    s->active = false;
    timer_del(s->timer);
    s->regs[GNW_H7B0_DMA2D_CR >> 2] &= ~DMA2D_CR_START;
    s->regs[GNW_H7B0_DMA2D_ISR >> 2] |= DMA2D_ISR_TEIF;
    qemu_log_mask(LOG_UNIMP, "gnw-h7b0-dma2d: %s; transfer rejected "
                  "before data became visible\n", reason);
    gnw_h7b0_dma2d_update_irq(s);
    return GNW_DMA2D_TIMED_REJECTED;
}

static GnwDma2dTimedStart gnw_h7b0_dma2d_start_timed(
    GnwH7B0Dma2dState *s)
{
    uint32_t cr = s->regs[GNW_H7B0_DMA2D_CR >> 2];
    uint32_t mode = (cr & DMA2D_CR_MODE_MASK) >> DMA2D_CR_MODE_SHIFT;
    uint32_t nlr = s->regs[GNW_H7B0_DMA2D_NLR >> 2];
    uint32_t lines = nlr & DMA2D_NLR_NL_MASK;
    uint32_t pixels = (nlr & DMA2D_NLR_PL_MASK) >> DMA2D_NLR_PL_SHIFT;
    uint32_t out_cm = s->regs[GNW_H7B0_DMA2D_OPFCCR >> 2] &
                      DMA2D_OPFCCR_CM_MASK;
    int bpp = gnw_h7b0_dma2d_output_bpp(out_cm);
    uint64_t source_stride;
    uint64_t destination_stride;
    uint64_t first_need;
    uint64_t regular_need;

    if (!s->timing_mode) {
        return GNW_DMA2D_TIMED_DISABLED;
    }

    if (!s->profile.identity) {
        return GNW_DMA2D_TIMED_DISABLED;
    }
    if (!s->profile.eligible ||
        !s->profile.cycles_per_pixel_num ||
        !s->profile.cycles_per_pixel_den || !s->clock_known ||
        !(s->profile.supported_modes & (1U << mode)) || !lines || !pixels ||
        bpp == 0 || (mode != DMA2D_MODE_R2M && mode != DMA2D_MODE_M2M)) {
        if (s->profile.identity == DMA2D_DMA_ENGINE_R1_PROFILE_ID) {
            return GNW_DMA2D_TIMED_DISABLED;
        }
        return gnw_h7b0_dma2d_reject_timed(
            s, "timed transfer class unsupported");
    }
    if (s->profile.identity == DMA2D_DMA_ENGINE_R1_PROFILE_ID) {
        GnwH7B0Dma2dModeTiming timing = s->mode_timing[mode];
        if (!timing.cycles_per_pixel_den ||
            s->clock_hz != s->evidence_required_clock_hz ||
            out_cm != DMA2D_OUTPUT_ARGB8888 || lines > 32 || pixels > 32 ||
            (uint64_t)lines * pixels < 8 ||
            (uint64_t)lines * pixels > 1024 ||
            !gnw_h7b0_dma2d_axi_range(
                s->regs[GNW_H7B0_DMA2D_OMAR >> 2],
                (uint64_t)lines * pixels * 4) ||
            s->regs[GNW_H7B0_DMA2D_OOR >> 2] != 0 ||
            (mode == DMA2D_MODE_M2M &&
             (!gnw_h7b0_dma2d_axi_range(
                  s->regs[GNW_H7B0_DMA2D_FGMAR >> 2],
                  (uint64_t)lines * pixels * 4) ||
              s->regs[GNW_H7B0_DMA2D_FGOR >> 2] != 0 ||
              ((uint64_t)s->regs[GNW_H7B0_DMA2D_FGMAR >> 2] <
                   (uint64_t)s->regs[GNW_H7B0_DMA2D_OMAR >> 2] +
                       (uint64_t)lines * pixels * 4 &&
               (uint64_t)s->regs[GNW_H7B0_DMA2D_OMAR >> 2] <
                   (uint64_t)s->regs[GNW_H7B0_DMA2D_FGMAR >> 2] +
                       (uint64_t)lines * pixels * 4)))) {
            return GNW_DMA2D_TIMED_DISABLED;
        }
        s->active_timing = timing;
    } else if (s->profile.identity == DMA2D_POLL_R4_PROFILE_ID ||
               s->profile.identity == DMA2D_POLL_R6_PROFILE_ID) {
        if (s->clock_hz != DMA2D_POLL_R4_REFERENCE_HZ ||
            mode != DMA2D_MODE_M2M ||
            out_cm != DMA2D_OUTPUT_ARGB8888 ||
            s->regs[GNW_H7B0_DMA2D_FGPFCCR >> 2] !=
                DMA2D_INPUT_ARGB8888 ||
            s->regs[GNW_H7B0_DMA2D_FGMAR >> 2] != DMA2D_POLL_R4_SOURCE ||
            s->regs[GNW_H7B0_DMA2D_OMAR >> 2] !=
                DMA2D_POLL_R4_DESTINATION ||
            s->regs[GNW_H7B0_DMA2D_FGOR >> 2] != 0 ||
            s->regs[GNW_H7B0_DMA2D_OOR >> 2] != 0 ||
            !(s->profile.identity == DMA2D_POLL_R4_PROFILE_ID ?
              gnw_h7b0_dma2d_poll_r4_geometry(lines, pixels) :
              gnw_h7b0_dma2d_poll_r6_geometry(lines, pixels))) {
            return GNW_DMA2D_TIMED_DISABLED;
        }
        s->active_timing = s->mode_timing[DMA2D_MODE_M2M];
    } else {
        s->active_timing = (GnwH7B0Dma2dModeTiming) { 0 };
    }

    source_stride = mode == DMA2D_MODE_M2M ?
        ((uint64_t)pixels + s->regs[GNW_H7B0_DMA2D_FGOR >> 2]) * bpp : 0;
    destination_stride = ((uint64_t)pixels +
        s->regs[GNW_H7B0_DMA2D_OOR >> 2]) * bpp;
    if (source_stride > UINT32_MAX || destination_stride > UINT32_MAX ||
        (uint64_t)s->regs[GNW_H7B0_DMA2D_OMAR >> 2] +
            (uint64_t)(lines - 1) * destination_stride +
            (uint64_t)pixels * bpp > UINT32_MAX ||
        (mode == DMA2D_MODE_M2M &&
         (uint64_t)s->regs[GNW_H7B0_DMA2D_FGMAR >> 2] +
             (uint64_t)(lines - 1) * source_stride +
             (uint64_t)pixels * bpp > UINT32_MAX) ||
        !gnw_h7b0_dma2d_phase_need(s->profile.setup_cycles, pixels,
                                    &s->profile, &first_need)) {
        return gnw_h7b0_dma2d_reject_timed(
            s, "timed descriptor overflows");
    }
    if (s->active_timing.cycles_per_pixel_den) {
        if (!gnw_h7b0_dma2d_mode_phase_needs(
                s->active_timing, pixels, &regular_need, &first_need)) {
            return gnw_h7b0_dma2d_reject_timed(
                s, "timed descriptor overflows");
        }
    }

    if (mode == DMA2D_MODE_M2M) {
        uint32_t fg_cm = s->regs[GNW_H7B0_DMA2D_FGPFCCR >> 2] &
                         DMA2D_PFCCR_CM_MASK;
        int fg_bpp;

        if (!gnw_h7b0_dma2d_format_bpp(fg_cm, &fg_bpp) || fg_bpp != bpp) {
            if (s->profile.identity == DMA2D_DMA_ENGINE_R1_PROFILE_ID) {
                return GNW_DMA2D_TIMED_DISABLED;
            }
            return gnw_h7b0_dma2d_reject_timed(
                s, "timed raw M2M format unsupported");
        }
    }

    s->active = true;
    s->active_mode = mode;
    s->current_line = 0;
    s->total_lines = lines;
    s->pixels_per_line = pixels;
    s->source = s->regs[GNW_H7B0_DMA2D_FGMAR >> 2];
    s->destination = s->regs[GNW_H7B0_DMA2D_OMAR >> 2];
    s->bytes_per_pixel = bpp;
    s->source_stride = source_stride;
    s->destination_stride = destination_stride;
    s->fill_color = s->regs[GNW_H7B0_DMA2D_OCOLR >> 2];
    s->phase_need = first_need;
    s->phase_progress = 0;
    s->phase_subcycle = 0;
    s->last_update_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->regs[GNW_H7B0_DMA2D_CR >> 2] |= DMA2D_CR_START;
    gnw_h7b0_dma2d_arm(s);
    return GNW_DMA2D_TIMED_STARTED;
}

static void gnw_h7b0_dma2d_reset(DeviceState *dev)
{
    GnwH7B0Dma2dState *s = GNW_H7B0_DMA2D(dev);

    timer_del(s->timer);
    memset(s->regs, 0, sizeof(s->regs));
    s->active = false;
    s->current_line = 0;
    s->phase_progress = 0;
    s->phase_subcycle = 0;
    s->unsupported_count = 0;
    qemu_set_irq(s->irq, 0);
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
        if (s->active) {
            int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

            while (!gnw_h7b0_dma2d_settle(s, now)) {
                /* An MMIO control write observes every line due by now. */
            }
            if (!s->active) {
                /* Settlement may have completed the transfer. A stale START
                 * copied from CR must not resurrect the completed work. */
                s->regs[addr >> 2] = value &
                    ~(DMA2D_CR_START | DMA2D_CR_ABORT | DMA2D_CR_SUSP);
                gnw_h7b0_dma2d_update_irq(s);
                return;
            }
            if (value & DMA2D_CR_ABORT) {
                s->active = false;
                timer_del(s->timer);
                s->regs[addr >> 2] = value &
                    ~(DMA2D_CR_START | DMA2D_CR_ABORT);
                gnw_h7b0_dma2d_update_irq(s);
                return;
            }
            if (value & DMA2D_CR_SUSP) {
                qemu_log_mask(LOG_UNIMP,
                              "gnw-h7b0-dma2d: timed suspend unsupported; "
                              "transfer remains active\n");
            }
            s->regs[addr >> 2] = value | DMA2D_CR_START;
            gnw_h7b0_dma2d_update_irq(s);
            return;
        }
        s->regs[addr >> 2] = value & ~DMA2D_CR_START;
        if (value & DMA2D_CR_START) {
            s->regs[addr >> 2] = value;
            GnwDma2dTimedStart timed = gnw_h7b0_dma2d_start_timed(s);

            if (timed == GNW_DMA2D_TIMED_DISABLED) {
                s->regs[addr >> 2] &= ~DMA2D_CR_START;
                gnw_h7b0_dma2d_do_transfer(s);
                s->regs[GNW_H7B0_DMA2D_ISR >> 2] |= DMA2D_ISR_TCIF;
                gnw_h7b0_dma2d_update_irq(s);
            }
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
    s->test_clock_known = true;
    s->test_clock_hz = 100000000;
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gnw_h7b0_dma2d_tick, s);
}

static int gnw_h7b0_dma2d_pre_save(void *opaque)
{
    GnwH7B0Dma2dState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    while (!gnw_h7b0_dma2d_settle(s, now)) {
        /* NLR bounds completion work. */
    }
    s->migration_timing_mode = s->timing_mode;
    s->migration_profile_identity = s->profile.identity;
    s->migration_clock_ns = now;
    return 0;
}

static bool gnw_h7b0_dma2d_post_load(void *opaque, int version_id,
                                      Error **errp)
{
    GnwH7B0Dma2dState *s = opaque;
    uint64_t regular_need;
    uint64_t first_need;

    if (version_id < 2) {
        if (s->timing_mode || s->profile.identity) {
            error_setg(errp, "DMA2D v1 migration lacks timed engine state");
            return false;
        }
        gnw_h7b0_dma2d_update_irq(s);
        return true;
    }
    if (s->migration_timing_mode != s->timing_mode) {
        error_setg(errp, "DMA2D timing mode differs between endpoints");
        return false;
    }
    if (s->migration_clock_ns < 0) {
        error_setg(errp, "DMA2D migration has invalid virtual clock epoch");
        return false;
    }
    if ((!s->clock_known || !s->clock_gate_enabled) && s->clock_hz) {
        error_setg(errp,
                   "DMA2D migration has inconsistent engine clock state");
        return false;
    }
    if (!s->active) {
        gnw_h7b0_dma2d_update_irq(s);
        return true;
    }
    if (s->profile.identity == DMA2D_DMA_ENGINE_R1_PROFILE_ID ||
        s->profile.identity == DMA2D_POLL_R4_PROFILE_ID ||
        s->profile.identity == DMA2D_POLL_R6_PROFILE_ID) {
        if (s->active_mode >= ARRAY_SIZE(s->mode_timing) ||
            ((s->profile.identity == DMA2D_POLL_R4_PROFILE_ID ||
              s->profile.identity == DMA2D_POLL_R6_PROFILE_ID) &&
             s->active_mode != DMA2D_MODE_M2M)) {
            error_setg(errp, "DMA2D migration has invalid timed mode");
            return false;
        }
        s->active_timing = s->mode_timing[s->active_mode];
        if (!gnw_h7b0_dma2d_mode_phase_needs(
                s->active_timing, s->pixels_per_line,
                &regular_need, &first_need)) {
            error_setg(errp, "DMA2D migration has overflowing timed profile");
            return false;
        }
    } else if (!gnw_h7b0_dma2d_phase_need(0, s->pixels_per_line,
                                    &s->profile, &regular_need) ||
        !gnw_h7b0_dma2d_phase_need(s->profile.setup_cycles,
                                    s->pixels_per_line,
                                    &s->profile, &first_need)) {
        error_setg(errp, "DMA2D migration has overflowing timed profile");
        return false;
    }
    if (!s->profile.eligible || !s->profile.identity ||
        !s->total_lines ||
        s->total_lines > DMA2D_NLR_NL_MASK || !s->pixels_per_line ||
        s->pixels_per_line > (DMA2D_NLR_PL_MASK >> DMA2D_NLR_PL_SHIFT) ||
        s->current_line >= s->total_lines ||
        (s->active_mode != DMA2D_MODE_R2M &&
         s->active_mode != DMA2D_MODE_M2M) ||
        (s->bytes_per_pixel != 2 && s->bytes_per_pixel != 4) ||
        (s->phase_need != regular_need &&
         !(s->current_line == 0 && s->phase_need == first_need)) ||
        s->phase_progress >= s->phase_need ||
        s->phase_subcycle >= NANOSECONDS_PER_SECOND ||
        s->last_update_ns < 0 ||
        s->last_update_ns > s->migration_clock_ns ||
        (uint64_t)s->destination +
            (uint64_t)(s->total_lines - 1) * s->destination_stride +
            (uint64_t)s->pixels_per_line * s->bytes_per_pixel > UINT32_MAX ||
        (s->active_mode == DMA2D_MODE_M2M &&
         (uint64_t)s->source +
             (uint64_t)(s->total_lines - 1) * s->source_stride +
             (uint64_t)s->pixels_per_line * s->bytes_per_pixel > UINT32_MAX) ||
        !(s->regs[GNW_H7B0_DMA2D_CR >> 2] & DMA2D_CR_START)) {
        error_setg(errp, "DMA2D migration has corrupt timed state");
        return false;
    }
    s->last_update_ns += qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                         s->migration_clock_ns;
    gnw_h7b0_dma2d_arm(s);
    gnw_h7b0_dma2d_update_irq(s);
    return true;
}

static const VMStateDescription vmstate_gnw_h7b0_dma2d = {
    .name = TYPE_GNW_H7B0_DMA2D,
    .version_id = 2,
    .minimum_version_id = 1,
    .pre_save = gnw_h7b0_dma2d_pre_save,
    .post_load_errp = gnw_h7b0_dma2d_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0Dma2dState, GNW_H7B0_DMA2D_SIZE / 4),
        VMSTATE_BOOL_V(migration_timing_mode, GnwH7B0Dma2dState, 2),
        VMSTATE_UINT32_EQUAL_V(migration_profile_identity,
                               GnwH7B0Dma2dState, 2,
                               "DMA2D timing profile differs"),
        VMSTATE_BOOL_V(clock_known, GnwH7B0Dma2dState, 2),
        VMSTATE_BOOL_V(clock_gate_enabled, GnwH7B0Dma2dState, 2),
        VMSTATE_UINT32_V(clock_hz, GnwH7B0Dma2dState, 2),
        VMSTATE_UINT64_V(unsupported_count, GnwH7B0Dma2dState, 2),
        VMSTATE_BOOL_V(active, GnwH7B0Dma2dState, 2),
        VMSTATE_UINT32_V(active_mode, GnwH7B0Dma2dState, 2),
        VMSTATE_UINT32_V(current_line, GnwH7B0Dma2dState, 2),
        VMSTATE_UINT32_V(total_lines, GnwH7B0Dma2dState, 2),
        VMSTATE_UINT32_V(pixels_per_line, GnwH7B0Dma2dState, 2),
        VMSTATE_UINT32_V(source, GnwH7B0Dma2dState, 2),
        VMSTATE_UINT32_V(destination, GnwH7B0Dma2dState, 2),
        VMSTATE_UINT32_V(source_stride, GnwH7B0Dma2dState, 2),
        VMSTATE_UINT32_V(destination_stride, GnwH7B0Dma2dState, 2),
        VMSTATE_UINT32_V(bytes_per_pixel, GnwH7B0Dma2dState, 2),
        VMSTATE_UINT32_V(fill_color, GnwH7B0Dma2dState, 2),
        VMSTATE_UINT64_V(phase_need, GnwH7B0Dma2dState, 2),
        VMSTATE_UINT64_V(phase_progress, GnwH7B0Dma2dState, 2),
        VMSTATE_UINT32_V(phase_subcycle, GnwH7B0Dma2dState, 2),
        VMSTATE_INT64_V(last_update_ns, GnwH7B0Dma2dState, 2),
        VMSTATE_INT64_V(migration_clock_ns, GnwH7B0Dma2dState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_dma2d_test_clock_set(Object *obj, Visitor *v,
                                           const char *name, void *opaque,
                                           Error **errp)
{
    const Property *prop = opaque;
    uint32_t *value = object_field_prop_ptr(obj, prop);

    if (!visit_type_uint32(v, name, value, errp)) {
        return;
    }
    GnwH7B0Dma2dState *s = GNW_H7B0_DMA2D(obj);
    gnw_h7b0_dma2d_set_clock_info(s, s->test_clock_known,
                                  s->test_clock_hz, true);
}

static const PropertyInfo gnw_h7b0_dma2d_test_clock_prop = {
    .type = "uint32",
    .set = gnw_h7b0_dma2d_test_clock_set,
    .realized_set_allowed = true,
};

static void gnw_h7b0_dma2d_test_known_set(Object *obj, Visitor *v,
                                           const char *name, void *opaque,
                                           Error **errp)
{
    const Property *prop = opaque;
    bool *value = object_field_prop_ptr(obj, prop);

    if (!visit_type_bool(v, name, value, errp)) {
        return;
    }
    GnwH7B0Dma2dState *s = GNW_H7B0_DMA2D(obj);
    gnw_h7b0_dma2d_set_clock_info(s, s->test_clock_known,
                                  s->test_clock_hz, true);
}

static const PropertyInfo gnw_h7b0_dma2d_test_known_prop = {
    .type = "bool",
    .set = gnw_h7b0_dma2d_test_known_set,
    .realized_set_allowed = true,
};

static void gnw_h7b0_dma2d_realize(DeviceState *dev, Error **errp)
{
    GnwH7B0Dma2dState *s = GNW_H7B0_DMA2D(dev);

    if ((s->test_profile ? 1 : 0) +
        (s->dma_engine_successor_profile ? 1 : 0) +
        (s->dma2d_poll_r4_profile ? 1 : 0) +
        (s->dma2d_poll_r6_profile ? 1 : 0) > 1) {
        error_setg(errp, "DMA2D timing profiles are mutually exclusive");
        return;
    }
    if ((s->dma_engine_successor_profile || s->dma2d_poll_r4_profile ||
         s->dma2d_poll_r6_profile) &&
        !s->timing_mode) {
        error_setg(errp, "DMA2D evidence timing profile requires timing-mode=on");
        return;
    }
    if (s->dma_engine_successor_profile &&
        (!DMA_ENGINE_R1_RECEIPT_SHA256[0] ||
         !s->dma_engine_successor_receipt_sha256 ||
         strcmp(s->dma_engine_successor_receipt_sha256,
                DMA_ENGINE_R1_RECEIPT_SHA256))) {
        error_setg(errp, "DMA engine successor profile requires the exact reviewed replay receipt");
        return;
    }
    if (!s->dma_engine_successor_profile &&
        s->dma_engine_successor_receipt_sha256) {
        error_setg(errp, "DMA2D DMA engine replay receipt requires its profile");
        return;
    }
    if (s->dma2d_poll_r4_profile &&
        (!s->dma2d_poll_r4_receipt_sha256 ||
         strcmp(s->dma2d_poll_r4_receipt_sha256,
                DMA2D_POLL_R4_RECEIPT_SHA256))) {
        error_setg(errp, "DMA2D poll r4 candidate requires its exact "
                   "physical HOLDOUT score receipt");
        return;
    }
    if (!s->dma2d_poll_r4_profile && s->dma2d_poll_r4_receipt_sha256) {
        error_setg(errp, "DMA2D poll r4 receipt requires its profile");
        return;
    }
    if (s->dma2d_poll_r6_profile &&
        (!s->dma2d_poll_r6_scope_sha256 ||
         strcmp(s->dma2d_poll_r6_scope_sha256,
                DMA2D_POLL_R6_SCOPE_SHA256))) {
        error_setg(errp, "DMA2D poll r6 comparison profile requires its exact scope freeze");
        return;
    }
    if (!s->dma2d_poll_r6_profile && s->dma2d_poll_r6_scope_sha256) {
        error_setg(errp, "DMA2D poll r6 scope digest requires its profile");
        return;
    }
    if (s->test_profile) {
        GnwH7B0Dma2dTimingProfile profile = {
            .identity = DMA2D_TEST_PROFILE_ID,
            .eligible = true,
            .supported_modes = (1U << DMA2D_MODE_R2M) |
                               (1U << DMA2D_MODE_M2M),
            .setup_cycles = 4,
            .cycles_per_pixel_num = 1,
            .cycles_per_line_num = s->test_cycles_per_line,
            .cycles_per_pixel_den = 1,
        };
        gnw_h7b0_dma2d_set_timing_profile(s, &profile);
        gnw_h7b0_dma2d_set_clock_info(s, s->test_clock_known,
                                      s->test_clock_hz, true);
    } else if (s->dma_engine_successor_profile) {
        GnwH7B0Dma2dTimingProfile profile = {
            .identity = DMA2D_DMA_ENGINE_R1_PROFILE_ID,
            .eligible = true,
            .supported_modes = (1U << DMA2D_MODE_M2M) |
                               (1U << DMA2D_MODE_R2M),
            .cycles_per_pixel_num = 1,
            .cycles_per_pixel_den = 1,
        };
        gnw_h7b0_dma2d_set_timing_profile(s, &profile);
        s->evidence_reference_hz = 280000000;
        s->evidence_required_clock_hz = 280000000;
        s->mode_timing[DMA2D_MODE_M2M] = (GnwH7B0Dma2dModeTiming) {
            .setup_num = 36672, .cycles_per_pixel_num = 372,
            .cycles_per_pixel_den = 224,
        };
        s->mode_timing[DMA2D_MODE_R2M] = (GnwH7B0Dma2dModeTiming) {
            .setup_num = 53408, .cycles_per_pixel_num = 217,
            .cycles_per_pixel_den = 240,
        };
    } else if (s->dma2d_poll_r4_profile || s->dma2d_poll_r6_profile) {
        GnwH7B0Dma2dTimingProfile profile = {
            .identity = s->dma2d_poll_r4_profile ? DMA2D_POLL_R4_PROFILE_ID :
                                                   DMA2D_POLL_R6_PROFILE_ID,
            .eligible = true,
            .supported_modes = 1U << DMA2D_MODE_M2M,
            .cycles_per_pixel_num = DMA2D_POLL_R4_PIXEL_NUM,
            .cycles_per_pixel_den = DMA2D_POLL_R4_DEN,
        };
        gnw_h7b0_dma2d_set_timing_profile(s, &profile);
        s->evidence_reference_hz = DMA2D_POLL_R4_REFERENCE_HZ;
        s->evidence_required_clock_hz = DMA2D_POLL_R4_REFERENCE_HZ;
        s->mode_timing[DMA2D_MODE_M2M] = (GnwH7B0Dma2dModeTiming) {
            .setup_num = DMA2D_POLL_R4_SETUP_NUM,
            .cycles_per_pixel_num = DMA2D_POLL_R4_PIXEL_NUM,
            .cycles_per_pixel_den = DMA2D_POLL_R4_DEN,
        };
    }
}

static const Property gnw_h7b0_dma2d_properties[] = {
    DEFINE_PROP_BOOL("timing-mode", GnwH7B0Dma2dState, timing_mode, false),
    DEFINE_PROP_BOOL("x-test-profile", GnwH7B0Dma2dState,
                     test_profile, false),
    DEFINE_PROP_BOOL("x-dma-engine-successor-r1-profile", GnwH7B0Dma2dState,
                     dma_engine_successor_profile, false),
    DEFINE_PROP_STRING("x-dma-engine-successor-r1-receipt-sha256",
                       GnwH7B0Dma2dState, dma_engine_successor_receipt_sha256),
    DEFINE_PROP_BOOL("x-dma2d-poll-r4-profile", GnwH7B0Dma2dState,
                     dma2d_poll_r4_profile, false),
    DEFINE_PROP_STRING("x-dma2d-poll-r4-receipt-sha256",
                       GnwH7B0Dma2dState, dma2d_poll_r4_receipt_sha256),
    DEFINE_PROP_BOOL("x-dma2d-poll-r6-comparison-profile", GnwH7B0Dma2dState,
                     dma2d_poll_r6_profile, false),
    DEFINE_PROP_STRING("x-dma2d-poll-r6-scope-sha256",
                       GnwH7B0Dma2dState, dma2d_poll_r6_scope_sha256),
    DEFINE_PROP_UINT32("x-test-cycles-per-line", GnwH7B0Dma2dState,
                       test_cycles_per_line, 0),
    DEFINE_PROP("x-test-clock-known", GnwH7B0Dma2dState, test_clock_known,
                gnw_h7b0_dma2d_test_known_prop, bool),
    DEFINE_PROP("x-test-clock-hz", GnwH7B0Dma2dState, test_clock_hz,
                gnw_h7b0_dma2d_test_clock_prop, uint32_t),
};

static void gnw_h7b0_dma2d_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_dma2d;
    dc->realize = gnw_h7b0_dma2d_realize;
    device_class_set_props(dc, gnw_h7b0_dma2d_properties);
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
