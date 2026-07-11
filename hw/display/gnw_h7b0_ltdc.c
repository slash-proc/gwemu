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
#include "hw/display/gnw_h7b0_regs_ltdc.h"

/*
 * Integer nearest-neighbor upscale factor for the host window. The G&W
 * LCD is a tiny 320x240 panel; displayed 1:1 that's uncomfortably small
 * on a modern monitor, so every frame is blitted at SCALE x its native
 * size instead of relying on host-side window scaling (not uniformly
 * supported/discoverable across QEMU's -display backends).
 */
#define GNW_H7B0_LTDC_SCALE 2

static void gnw_h7b0_ltdc_update_irq(GnwH7B0LtdcState *s)
{
    uint32_t pending = s->regs[GNW_H7B0_LTDC_ISR >> 2] &
                       s->regs[GNW_H7B0_LTDC_IER >> 2];

    qemu_set_irq(s->irq, pending != 0);
}

static void gnw_h7b0_ltdc_reload_active(GnwH7B0LtdcState *s)
{
    s->active_l1cr = s->regs[GNW_H7B0_LTDC_L1CR >> 2];
    s->active_l1cfbar = s->regs[GNW_H7B0_LTDC_L1CFBAR >> 2];
    s->active_l1cfblr = s->regs[GNW_H7B0_LTDC_L1CFBLR >> 2];
    s->active_l1cfblnr = s->regs[GNW_H7B0_LTDC_L1CFBLNR >> 2];
    s->active_l1pfcr = s->regs[GNW_H7B0_LTDC_L1PFCR >> 2];

    s->active_l2cr = s->regs[GNW_H7B0_LTDC_L2CR >> 2];
    s->active_l2cfbar = s->regs[GNW_H7B0_LTDC_L2CFBAR >> 2];
    s->active_l2cfblr = s->regs[GNW_H7B0_LTDC_L2CFBLR >> 2];
    s->active_l2cfblnr = s->regs[GNW_H7B0_LTDC_L2CFBLNR >> 2];
    s->active_l2pfcr = s->regs[GNW_H7B0_LTDC_L2PFCR >> 2];
    s->active_l2cacr = s->regs[GNW_H7B0_LTDC_L2CACR >> 2];
}

static void gnw_h7b0_ltdc_recalc_timers(GnwH7B0LtdcState *s, int64_t now)
{
    uint32_t twcr = s->regs[GNW_H7B0_LTDC_TWCR >> 2];
    uint32_t totalh = (twcr & 0x7FF) + 1;
    uint32_t lipcr = s->regs[GNW_H7B0_LTDC_LIPCR >> 2] & 0x7FF;

    if (totalh <= 1) {
        totalh = 240;
    }

    int64_t frame_ns = NANOSECONDS_PER_SECOND / GNW_H7B0_LTDC_VBLANK_HZ;
    int64_t line_ns = frame_ns / totalh;

    timer_mod(s->vblank_timer, now + frame_ns);

    if (lipcr < totalh) {
        timer_mod(s->line_timer, now + lipcr * line_ns);
    } else {
        timer_del(s->line_timer);
    }
}

static int gnw_h7b0_ltdc_capture_setup(GnwH7B0LtdcState *s);
static void gnw_h7b0_ltdc_capture_rows(GnwH7B0LtdcState *s, int row_start,
                                        int row_end);

static void gnw_h7b0_ltdc_vblank_tick(void *opaque)
{
    GnwH7B0LtdcState *s = GNW_H7B0_LTDC(opaque);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    /*
     * Capture the fully composed frame just before VBR reload takes
     * effect and the next frame begins. This guarantees we don't catch
     * the guest in the middle of drawing overlays to the frontbuffer.
     *
     * A progressive/scanline-timed version of this (spreading the
     * capture across the whole frame period like a real raster scan,
     * instead of one atomic read here) was tried and made things
     * *worse*: it let this device's own ~60Hz capture and QEMU's
     * independent ~33Hz UI-refresh blit interleave, so the blit could
     * catch shadow_buffer mid-capture -- some rows from this frame,
     * some still from last frame -- which is a tear QEMU introduces
     * itself, on top of (not instead of) whatever the guest's own
     * redraw timing does. Reverted. The real horizontal-scroll tearing
     * this was meant to fix needs understanding retro-go's actual
     * framebuffer-swap strategy during scrolling, not a generic capture-
     * timing change here.
     */
    int rows = gnw_h7b0_ltdc_capture_setup(s);
    if (rows > 0) {
        gnw_h7b0_ltdc_capture_rows(s, 0, rows);
        s->content_dirty = true;
    }

    s->regs[GNW_H7B0_LTDC_ISR >> 2] |= LTDC_ISR_RRIF;
    gnw_h7b0_ltdc_update_irq(s);

    if (s->vbr_reload_pending) {
        gnw_h7b0_ltdc_reload_active(s);
        s->vbr_reload_pending = false;
        s->regs[GNW_H7B0_LTDC_SRCR >> 2] &= ~LTDC_SRCR_VBR;
    }

    gnw_h7b0_ltdc_recalc_timers(s, now);
}

static void gnw_h7b0_ltdc_line_tick(void *opaque)
{
    GnwH7B0LtdcState *s = GNW_H7B0_LTDC(opaque);

    s->regs[GNW_H7B0_LTDC_ISR >> 2] |= LTDC_ISR_LIF;
    gnw_h7b0_ltdc_update_irq(s);
}

static void gnw_h7b0_ltdc_reset(DeviceState *dev)
{
    GnwH7B0LtdcState *s = GNW_H7B0_LTDC(dev);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    for (int i = 0; i < (GNW_H7B0_LTDC_SIZE / 4); i++) {
        s->regs[i] = get_ltdc_reset_value(i * 4);
    }
    s->invalidate = 1;
    s->pf_warned = false;
    s->vbr_reload_pending = false;

    g_free(s->shadow_buffer);
    s->shadow_buffer = NULL;
    s->shadow_width = 0;
    s->shadow_height = 0;
    memset(s->clut, 0, sizeof(s->clut));

    gnw_h7b0_ltdc_reload_active(s);
    gnw_h7b0_ltdc_recalc_timers(s, now);
}

static bool gnw_h7b0_ltdc_enabled(GnwH7B0LtdcState *s)
{
    return (s->regs[GNW_H7B0_LTDC_GCR >> 2] & LTDC_GCR_LTDCEN) &&
           ((s->regs[GNW_H7B0_LTDC_L1CR >> 2] & LTDC_LxCR_LEN) ||
            (s->regs[GNW_H7B0_LTDC_L2CR >> 2] & LTDC_LxCR_LEN));
}

static inline uint32_t gnw_h7b0_ltdc_rgb565_to_pixel32(uint16_t px)
{
    unsigned int r = ((px >> 11) & 0x1f) << 3;
    unsigned int g = ((px >> 5) & 0x3f) << 2;
    unsigned int b = (px & 0x1f) << 3;
    unsigned int r8 = r | (r >> 5);
    unsigned int g8 = g | (g >> 6);
    unsigned int b8 = b | (b >> 5);

    return 0xFF000000U | (r8 << 16) | (g8 << 8) | b8;
}

static uint32_t gnw_h7b0_ltdc_read_pixel(hwaddr addr, uint32_t pfcr)
{
    switch (pfcr) {
    case 0: /* ARGB8888 */
    {
        uint8_t buf[4];
        cpu_physical_memory_read(addr, buf, 4);
        return ldl_le_p(buf);
    }
    case 2: /* RGB565 */
    {
        uint8_t buf[2];
        cpu_physical_memory_read(addr, buf, 2);
        return gnw_h7b0_ltdc_rgb565_to_pixel32(lduw_le_p(buf));
    }
    case 3: /* ARGB1555 */
    {
        uint8_t buf[2];
        cpu_physical_memory_read(addr, buf, 2);
        uint16_t px = lduw_le_p(buf);
        unsigned int a1 = (px >> 15) & 0x1;
        unsigned int r5 = (px >> 10) & 0x1F;
        unsigned int g5 = (px >> 5) & 0x1F;
        unsigned int b5 = px & 0x1F;
        unsigned int r8 = (r5 << 3) | (r5 >> 2);
        unsigned int g8 = (g5 << 3) | (g5 >> 2);
        unsigned int b8 = (b5 << 3) | (b5 >> 2);
        return (a1 ? 0xFF000000U : 0U) | (r8 << 16) | (g8 << 8) | b8;
    }
    case 4: /* ARGB4444 */
    {
        uint8_t buf[2];
        cpu_physical_memory_read(addr, buf, 2);
        uint16_t px = lduw_le_p(buf);
        unsigned int a4 = (px >> 12) & 0xF;
        unsigned int r4 = (px >> 8) & 0xF;
        unsigned int g4 = (px >> 4) & 0xF;
        unsigned int b4 = px & 0xF;
        return ((a4 * 0x11U) << 24) | ((r4 * 0x11U) << 16) |
               ((g4 * 0x11U) << 8) | (b4 * 0x11U);
    }
    default:
        return 0;
    }
}

/*
 * Validate Layer1's format/geometry, (re)size the shadow buffer if
 * needed, and return the row count to capture this frame (0 if
 * disabled/unsupported -- caller should skip scheduling capture
 * ticks). Layer2/Layer1 CFBAR etc. are all read from s->active_* here
 * and in gnw_h7b0_ltdc_capture_rows() below; those only change on an
 * SRCR reload (applied at the vblank tick that also calls this), so
 * they're stable for the whole frame and safe to re-read per capture
 * step without caching.
 */
static int gnw_h7b0_ltdc_capture_setup(GnwH7B0LtdcState *s)
{
    uint32_t cfblr, cfblnr, pfcr;
    int cols, rows, src_width;

    if (!gnw_h7b0_ltdc_enabled(s)) {
        return 0;
    }

    pfcr = s->active_l1pfcr & LTDC_LxPFCR_PF_MASK;
    if (pfcr != LTDC_PF_RGB565 && pfcr != LTDC_PF_L8) {
        if (!s->pf_warned) {
            qemu_log_mask(LOG_UNIMP,
                          "gnw_h7b0_ltdc: only RGB565/L8 are supported for Layer 1 PFCR "
                          "requested format %u -- nothing will be shown\n",
                          pfcr);
            s->pf_warned = true;
        }
        return 0;
    }

    if (!(s->active_l1cr & LTDC_LxCR_LEN)) {
        return 0; /* Layer 1 must be enabled for base frame */
    }

    cfblr = s->active_l1cfblr;
    cfblnr = s->active_l1cfblnr & 0x7FFU;

    src_width = (int)((cfblr >> LTDC_LxCFBLR_CFBP_SHIFT) &
                       LTDC_LxCFBLR_CFBP_MASK);
    cols = src_width / (pfcr == LTDC_PF_L8 ? 1 : 2);
    rows = (int)cfblnr;

    if (cols <= 0 || rows <= 0) {
        return 0;
    }

    if (cols != s->shadow_width || rows != s->shadow_height) {
        g_free(s->shadow_buffer);
        s->shadow_buffer = g_new0(uint32_t, cols * rows);
        s->shadow_width = cols;
        s->shadow_height = rows;
    }

    return rows;
}

static void gnw_h7b0_ltdc_capture_rows(GnwH7B0LtdcState *s, int row_start,
                                        int row_end)
{
    uint32_t cfbar = s->active_l1cfbar;
    int cols = s->shadow_width;
    uint32_t pfcr = s->active_l1pfcr & LTDC_LxPFCR_PF_MASK;
    bool l1_l8 = (pfcr == LTDC_PF_L8);
    int src_width = cols * (l1_l8 ? 1 : 2);
    g_autofree uint8_t *linebuf = g_malloc(src_width);

    bool l2_en = s->active_l2cr & LTDC_LxCR_LEN;
    uint32_t l2_cfbar = s->active_l2cfbar;
    uint32_t l2_pfcr = s->active_l2pfcr & LTDC_LxPFCR_PF_MASK;
    uint32_t l2_cfblr = s->active_l2cfblr;
    int l2_src_width = (int)((l2_cfblr >> LTDC_LxCFBLR_CFBP_SHIFT) &
                              LTDC_LxCFBLR_CFBP_MASK);
    int l2_bpp = 0;
    if (l2_en) {
        switch (l2_pfcr) {
        case 0: l2_bpp = 4; break;
        case 2: case 3: case 4: l2_bpp = 2; break;
        default: l2_bpp = 0; break;
        }
    }
    uint32_t l2_cacr = s->active_l2cacr & 0xFF;

    for (int y = row_start; y < row_end; y++) {
        cpu_physical_memory_read(cfbar + (hwaddr)y * src_width, linebuf,
                                 src_width);

        for (int x = 0; x < cols; x++) {
            uint32_t px = l1_l8 ? s->clut[linebuf[x]]
                                 : gnw_h7b0_ltdc_rgb565_to_pixel32(
                                       lduw_le_p(linebuf + x * 2));

            if (l2_en && l2_bpp > 0) {
                uint32_t fg_px = gnw_h7b0_ltdc_read_pixel(
                    l2_cfbar + (hwaddr)y * l2_src_width + (hwaddr)x * l2_bpp,
                    l2_pfcr);
                unsigned int fg_a = (fg_px >> 24) & 0xFF;
                fg_a = (fg_a * l2_cacr) / 255U;
                if (fg_a > 0) {
                    unsigned int bg_r = (px >> 16) & 0xFF;
                    unsigned int bg_g = (px >> 8) & 0xFF;
                    unsigned int bg_b = px & 0xFF;
                    unsigned int fg_r = (fg_px >> 16) & 0xFF;
                    unsigned int fg_g = (fg_px >> 8) & 0xFF;
                    unsigned int fg_b = fg_px & 0xFF;

                    unsigned int r = (fg_r * fg_a + bg_r * (255 - fg_a)) / 255U;
                    unsigned int g = (fg_g * fg_a + bg_g * (255 - fg_a)) / 255U;
                    unsigned int b = (fg_b * fg_a + bg_b * (255 - fg_a)) / 255U;
                    px = 0xFF000000U | (r << 16) | (g << 8) | b;
                }
            }

            s->shadow_buffer[y * cols + x] = px;
        }
    }
}

static void gnw_h7b0_ltdc_update_display(void *opaque)
{
    GnwH7B0LtdcState *s = GNW_H7B0_LTDC(opaque);
    DisplaySurface *surface = qemu_console_surface(s->con);

    if (!s->shadow_buffer || s->shadow_width <= 0 || s->shadow_height <= 0) {
        return;
    }

    if (surface_bits_per_pixel(surface) != 32) {
        return;
    }

    if (!s->content_dirty && !s->invalidate) {
        return;
    }

    if (s->invalidate ||
        s->shadow_width * GNW_H7B0_LTDC_SCALE != surface_width(surface) ||
        s->shadow_height * GNW_H7B0_LTDC_SCALE != surface_height(surface)) {
        qemu_console_resize(s->con, s->shadow_width * GNW_H7B0_LTDC_SCALE,
                            s->shadow_height * GNW_H7B0_LTDC_SCALE);
        surface = qemu_console_surface(s->con);
    }

    for (int y = 0; y < s->shadow_height; y++) {
        uint8_t *drow0 = surface_data(surface) +
                          (hwaddr)y * GNW_H7B0_LTDC_SCALE * surface_stride(surface);

        for (int x = 0; x < s->shadow_width; x++) {
            uint32_t px = s->shadow_buffer[y * s->shadow_width + x];
            for (int sy = 0; sy < GNW_H7B0_LTDC_SCALE; sy++) {
                uint32_t *drow = (uint32_t *)(drow0 +
                                              (hwaddr)sy * surface_stride(surface));
                for (int sx = 0; sx < GNW_H7B0_LTDC_SCALE; sx++) {
                    drow[x * GNW_H7B0_LTDC_SCALE + sx] = px;
                }
            }
        }
    }

    dpy_gfx_update(s->con, 0, 0, s->shadow_width * GNW_H7B0_LTDC_SCALE,
                   s->shadow_height * GNW_H7B0_LTDC_SCALE);
    s->invalidate = 0;
    s->content_dirty = false;
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

    uint32_t mask = get_ltdc_write_mask(addr);
    value = (s->regs[addr >> 2] & ~mask) | (value & mask);

    switch (addr) {
    case GNW_H7B0_LTDC_SRCR:
        /*
         * Real hardware copies shadow registers (CFBAR/CFBLR/CFBLNR/
         * PFCR) into the active set on a reload event, clearing IMR/VBR
         * once done. IMR ("immediate") really is instant on real
         * hardware too; VBR ("vertical blanking") is real hardware's
         * deferred-to-next-vblank path and is now modeled that way
         * here (gnw_h7b0_ltdc_vblank_tick() applies it) instead of also
         * being instant -- that instant-VBR approximation was the
         * documented cause of gnw-chainloader/retro-go's menu tearing:
         * firmware's fb1/fb2 double-buffer swap (a VBR reload) used to
         * take effect immediately, so a redraw could land mid-swap
         * relative to what firmware had actually finished drawing.
         *
         * Critically, VBR must also *stay set* in the register until
         * the real vblank tick clears it -- gnw-chainloader's
         * gui_refresh() (src/chainloader/gui.c) does exactly what real
         * hardware expects: request the reload, then busy-wait on
         * `SRCR & (VBR|IMR)` clearing as its actual frame-rate throttle.
         * Clearing VBR synchronously here (the previous behavior) made
         * that wait a no-op, so gui_refresh() looped at whatever rate
         * the CPU could manage (measured ~1000x/sec) instead of ~60Hz
         * -- the real root cause of the flicker (foreground redrawn/
         * cleared far faster than any real display could show) and,
         * via the identical poll-on-SRCR/ISR pattern in retro-go's
         * lcd_wait_for_vblank(), very likely also the "runs too fast"
         * bug. IMR keeps clearing instantly (real immediate reload
         * really is instant on real hardware too).
         */
        if (value & LTDC_SRCR_IMR) {
            gnw_h7b0_ltdc_reload_active(s);
        }
        if (value & LTDC_SRCR_VBR) {
            s->vbr_reload_pending = true;
        }
        s->regs[addr >> 2] = value & ~LTDC_SRCR_IMR;
        return;
    case GNW_H7B0_LTDC_IER:
        s->regs[addr >> 2] = value;
        gnw_h7b0_ltdc_update_irq(s);
        return;
    case GNW_H7B0_LTDC_ICR:
        /* Write-1-to-clear: ICR bit positions mirror ISR's exactly. */
        s->regs[GNW_H7B0_LTDC_ISR >> 2] &= ~value;
        gnw_h7b0_ltdc_update_irq(s);
        return;
    case GNW_H7B0_LTDC_ISR:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: ISR is read-only on real hardware (use ICR to "
                      "clear flags)\n", __func__);
        return;
    case GNW_H7B0_LTDC_L1CLUTWR:
    {
        /*
         * Write-only on real hardware (L1CLUTWR reads back as 0, hence
         * no shadow storage needed beyond s->clut[] itself): CLUTADD is
         * the palette index, RED/GREEN/BLUE pack the entry -- see the
         * SVD's L1CLUTWR fields. Converted to this device's host
         * pixel32 format up front so capture_rows() can use L8 entries
         * exactly like RGB565-converted pixels.
         */
        unsigned int idx = (value >> 24) & 0xFF;
        unsigned int r = (value >> 16) & 0xFF;
        unsigned int g = (value >> 8) & 0xFF;
        unsigned int b = value & 0xFF;
        s->clut[idx] = 0xFF000000U | (r << 16) | (g << 8) | b;
        return;
    }
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
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    s->con = graphic_console_init(dev, 0, &gnw_h7b0_ltdc_gfx_ops, s);
    s->vblank_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    gnw_h7b0_ltdc_vblank_tick, s);
    s->line_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                  gnw_h7b0_ltdc_line_tick, s);
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
