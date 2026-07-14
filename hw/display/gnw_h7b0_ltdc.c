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
#include "framebuffer.h"
#include "system/address-spaces.h"

static int gnw_h7b0_ltdc_capture_setup(GnwH7B0LtdcState *s);
static void gnw_h7b0_ltdc_capture_rows(GnwH7B0LtdcState *s, int row_start,
                                        int row_end);
static bool gnw_h7b0_ltdc_fb_dirty_check_and_clear(GnwH7B0LtdcState *s);
static int gnw_h7b0_ltdc_layer_stride_bytes(uint32_t cfblr);
static void gnw_h7b0_ltdc_fb_track_range(MemoryRegionSection *section,
                                          hwaddr *cur_base, hwaddr *cur_len,
                                          hwaddr base, hwaddr len);

static bool gnw_h7b0_ltdc_enabled(GnwH7B0LtdcState *s)
{
    return (s->active_gcr & LTDC_GCR_LTDCEN) &&
           ((s->active_l1cr & LTDC_LxCR_LEN) ||
            (s->active_l2cr & LTDC_LxCR_LEN));
}

/* Return true when a full frame was captured into shadow_buffer. */
static bool gnw_h7b0_ltdc_capture_if_enabled(GnwH7B0LtdcState *s)
{
    int rows;

    if (!gnw_h7b0_ltdc_enabled(s)) {
        return false;
    }

    rows = gnw_h7b0_ltdc_capture_setup(s);
    if (rows <= 0) {
        return false;
    }

    gnw_h7b0_ltdc_capture_rows(s, 0, rows);
    s->content_dirty = true;
    s->invalidate = 1;
    return true;
}

/*
 * Integer nearest-neighbor upscale factor for the host window. The G&W
 * LCD is a tiny 320x240 panel; displayed 1:1 that's uncomfortably small
 * on a modern monitor, so every frame is blitted at SCALE x its native
 * size instead of relying on host-side window scaling (not uniformly
 * supported/discoverable across QEMU's -display backends).
 */
#define GNW_H7B0_LTDC_SCALE 2

/*
 * How many consecutive vblank ticks with no SRCR write (VBR or IMR) at
 * all count as "VBR mode has gone idle" -- see srcr_idle_ticks' doc
 * comment in gnw_h7b0_ltdc.h. ~130ms at the nominal 60Hz vblank rate:
 * comfortably longer than a single dropped/late frame, but much shorter
 * than the multi-hundred-ms real gaps normal in-game stalls can produce
 * (safe specifically because this is additionally gated on the
 * framebuffer having genuinely new content, which a stalled game isn't
 * producing during its stall). _MAX just bounds the counter itself so
 * it can't silently wrap on a long-running static screen.
 */
#define GNW_H7B0_LTDC_SRCR_IDLE_TICKS_THRESHOLD 8
#define GNW_H7B0_LTDC_SRCR_IDLE_TICKS_MAX 1000000

static void gnw_h7b0_ltdc_update_irq(GnwH7B0LtdcState *s)
{
    uint32_t pending = s->regs[GNW_H7B0_LTDC_ISR >> 2] &
                       s->regs[GNW_H7B0_LTDC_IER >> 2];

    qemu_set_irq(s->irq, pending != 0);
}

static void gnw_h7b0_ltdc_reload_active(GnwH7B0LtdcState *s)
{
    /*
     * Composition-affecting registers are about to change -- see
     * fb_reg_dirty's comment in gnw_h7b0_ltdc.h for why this is an
     * additional signal alongside (not instead of) the RAM-dirty
     * check in gnw_h7b0_ltdc_fb_dirty_check_and_clear().
     */
    s->fb_reg_dirty = true;

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

    s->active_l1ckcr = s->regs[GNW_H7B0_LTDC_L1CKCR >> 2];
    s->active_l2ckcr = s->regs[GNW_H7B0_LTDC_L2CKCR >> 2];
    s->active_l1dccr = s->regs[GNW_H7B0_LTDC_L1DCCR >> 2];
    s->active_l2dccr = s->regs[GNW_H7B0_LTDC_L2DCCR >> 2];
    s->active_bccr = s->regs[GNW_H7B0_LTDC_BCCR >> 2];
    s->active_l1cacr = s->regs[GNW_H7B0_LTDC_L1CACR >> 2];
    s->active_l1bfcr = s->regs[GNW_H7B0_LTDC_L1BFCR >> 2];
    s->active_l2bfcr = s->regs[GNW_H7B0_LTDC_L2BFCR >> 2];
    s->active_l1whpcr = s->regs[GNW_H7B0_LTDC_L1WHPCR >> 2];
    s->active_l1wvpcr = s->regs[GNW_H7B0_LTDC_L1WVPCR >> 2];
    s->active_l2whpcr = s->regs[GNW_H7B0_LTDC_L2WHPCR >> 2];
    s->active_l2wvpcr = s->regs[GNW_H7B0_LTDC_L2WVPCR >> 2];
    s->active_gcr = s->regs[GNW_H7B0_LTDC_GCR >> 2];
    s->active_bpcr = s->regs[GNW_H7B0_LTDC_BPCR >> 2];
}

static void gnw_h7b0_ltdc_recalc_timers(GnwH7B0LtdcState *s, int64_t now)
{
    uint32_t twcr = s->regs[GNW_H7B0_LTDC_TWCR >> 2];
    /* TWCR packs both fields: TOTALH bits[10:0], TOTALW bits[27:16] --
     * see STM32H7B0.svd. */
    uint32_t totalh = (twcr & 0x7FF) + 1;
    uint32_t totalw = ((twcr >> 16) & 0xFFF) + 1;
    uint32_t lipcr = s->regs[GNW_H7B0_LTDC_LIPCR >> 2] & 0x7FF;

    if (totalh <= 1) {
        totalh = 240;
    }

    /*
     * Derive the real vblank rate from the live PLL3R frequency (LTDC's
     * pixel clock is hardwired to pll3_r_ck, no clock-source mux) and the
     * real panel timing, instead of assuming the fixed
     * GNW_H7B0_LTDC_VBLANK_HZ -- real firmware's PLL3 config (M=4,N=9,R=24)
     * against the real 392x255 panel timing computes to ~66.7Hz, not 60Hz,
     * and that ~10% mismatch against a fixed 60Hz tick was a suspected
     * cause of coverflow-menu flicker/scroll roughness (see the plan doc
     * for the full derivation). Falls back to the fixed constant if RCC
     * isn't wired or the computed rate is nonsensical -- same defensive
     * clamping philosophy used for TIM2/DMA/SPI timing elsewhere in this
     * device family.
     */
    uint32_t vblank_hz = GNW_H7B0_LTDC_VBLANK_HZ;
    if (s->rcc && totalw > 0 && totalh > 0) {
        uint32_t pll3r_hz = gnw_h7b0_rcc_get_pll3r_hz(s->rcc);
        uint64_t pixels = (uint64_t)totalw * totalh;
        uint32_t real_hz = pixels > 0 ? (uint32_t)(pll3r_hz / pixels) : 0;

        if (real_hz >= 1 && real_hz <= 1000) {
            vblank_hz = real_hz;
        }
    }

    int64_t frame_ns = NANOSECONDS_PER_SECOND / vblank_hz;
    int64_t line_ns = frame_ns / totalh;

    timer_mod(s->vblank_timer, now + frame_ns);

    if (lipcr < totalh) {
        timer_mod(s->line_timer, now + lipcr * line_ns);
    } else {
        timer_del(s->line_timer);
    }
}

void gnw_h7b0_ltdc_set_rcc(GnwH7B0LtdcState *s, GnwH7B0RccState *rcc)
{
    s->rcc = rcc;
}

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
    /* We DO NOT capture here anymore.
     * Capturing here while the CPU is still drawing (because emulated CPU is slower
     * than 60Hz) captures partially drawn frames.
     * We only capture on VBR write.
     */

    /*
     * RRIF only belongs here when a VBR reload actually applies this
     * tick -- real hardware sets it on a genuine reload event, not on
     * every vblank. Setting it unconditionally (the previous behavior)
     * made firmware's HAL_LTDC_ReloadEventCallback() (which re-stages
     * CFBAR for double buffering) fire on every single vblank instead
     * of only when a swap was actually requested, since retro-go leaves
     * LTDC_IT_RR permanently enabled.
     */
    if (s->vbr_reload_pending) {
        gnw_h7b0_ltdc_reload_active(s);
        s->vbr_reload_pending = false;
        s->regs[GNW_H7B0_LTDC_SRCR >> 2] &= ~LTDC_SRCR_VBR;
        s->regs[GNW_H7B0_LTDC_ISR >> 2] |= LTDC_ISR_RRIF;
        gnw_h7b0_ltdc_update_irq(s);
        if (s->vbr_deferred_capture) {
            gnw_h7b0_ltdc_capture_if_enabled(s);
            s->vbr_deferred_capture = false;
        }
    } else {
        /*
         * Auto-capture for firmware (like gnwmanager, or retro-go's own
         * main-menu UI) that doesn't reload via VBR at all -- it just
         * paints pixels directly into an already-configured Layer1/2.
         * Gated on vbr_active, which tracks "is the *current* screen/
         * config VBR-paced" rather than "has this device ever used VBR":
         * an idle-timeout version of this gate was tried and reverted --
         * real in-game firmware can leave multi-hundred-ms real gaps
         * between VBR writes on its own (see the SMW APU-catchup-burst
         * investigation), so any timeout short enough to un-stick the
         * menu promptly was also short enough to spuriously re-arm this
         * fallback mid-game during those same bursts, reintroducing
         * mid-draw tearing exactly when the game stutters. vbr_active is
         * instead reset by IMR reloads (see the SRCR write handler),
         * which real screen/config transitions actually use -- letting a
         * genuinely non-VBR screen (the menu) re-arm the fallback
         * immediately after taking over the layers, without depending on
         * any elapsed-time guess.
         */
        if (s->srcr_idle_ticks < GNW_H7B0_LTDC_SRCR_IDLE_TICKS_MAX) {
            s->srcr_idle_ticks++;
        }
        /*
         * A transition can end on a VBR-type reload (not IMR) with no
         * further reloads ever coming, permanently latching vbr_active
         * true with nothing left to reset it -- see srcr_idle_ticks'
         * doc comment in gnw_h7b0_ltdc.h. Once VBR has gone idle for
         * GNW_H7B0_LTDC_SRCR_IDLE_TICKS_THRESHOLD ticks, allow the
         * fallback even with vbr_active still true, but ONLY alongside
         * the same RAM-dirty requirement as the normal case -- a stalled
         * game isn't writing new framebuffer content during its stall,
         * so this can't spuriously re-arm mid-game the way a bare
         * elapsed-time guess did.
         */
        bool vbr_idle = s->srcr_idle_ticks >= GNW_H7B0_LTDC_SRCR_IDLE_TICKS_THRESHOLD;
        if ((!s->vbr_active || vbr_idle) && !s->content_dirty &&
            gnw_h7b0_ltdc_enabled(s) &&
            gnw_h7b0_ltdc_fb_dirty_check_and_clear(s)) {
            gnw_h7b0_ltdc_capture_if_enabled(s);
            s->fb_reg_dirty = false;
        }
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
    s->vbr_deferred_capture = false;
    s->vbr_active = false;
    s->srcr_idle_ticks = 0;

    g_free(s->shadow_buffer);
    s->shadow_buffer = NULL;
    s->shadow_width = 0;
    s->shadow_height = 0;
    memset(s->clut, 0, sizeof(s->clut));
    memset(s->clut2, 0, sizeof(s->clut2));

    gnw_h7b0_ltdc_fb_track_range(&s->fb_l1_section, &s->fb_l1_track_base,
                                  &s->fb_l1_track_len, 0, 0);
    gnw_h7b0_ltdc_fb_track_range(&s->fb_l2_section, &s->fb_l2_track_base,
                                  &s->fb_l2_track_len, 0, 0);

    gnw_h7b0_ltdc_reload_active(s);
    gnw_h7b0_ltdc_recalc_timers(s, now);
}

static int gnw_h7b0_ltdc_layer_stride_bytes(uint32_t cfblr)
{
    int stride = (int)((cfblr >> LTDC_LxCFBLR_CFBP_SHIFT) &
                        LTDC_LxCFBLR_CFBP_MASK);
    int line_bytes = (int)(cfblr & LTDC_LxCFBLR_CFBLL_MASK);

    if (stride <= 0) {
        stride = line_bytes;
    }
    return stride;
}

/*
 * Bytes-per-pixel for Layer2's supported formats -- mirrors the l2_bpp
 * switch inside gnw_h7b0_ltdc_capture_rows() (kept as a separate literal
 * copy there rather than refactored to share this helper, to avoid
 * touching that already-working per-pixel hot path while adding this
 * dirty-tracking feature). Returns 0 for an unsupported/disabled format,
 * same convention as capture_rows()'s l2_bpp.
 */
static int gnw_h7b0_ltdc_l2_bpp(uint32_t l2_pfcr)
{
    switch (l2_pfcr) {
    case 0: return 4; /* ARGB8888 */
    case 2: case 3: case 4: return 2; /* RGB565/ARGB1555/ARGB4444 */
    case 5: return 1; /* L8 */
    case 6: return 1; /* AL44 */
    default: return 0;
    }
}

/*
 * Track (and rebind, only when the underlying range actually changes)
 * one framebuffer range's MemoryRegionSection for DIRTY_MEMORY_VGA
 * dirty-bitmap tracking, same pattern as pl110.c's s->fbsection via
 * framebuffer_update_memory_section() -- see that function's doc
 * comment in hw/display/framebuffer.h. Rebinding toggles
 * memory_region_set_log() when the logging refcount transitions
 * 0<->1, which can trigger a memory-transaction/TLB update, so this
 * must only run when `base`/`len` actually change from what's already
 * tracked (checked via cur_base/cur_len), not unconditionally every
 * tick -- pl110_update_display() only calls the equivalent on
 * s->invalidate for the same reason.
 */
static void gnw_h7b0_ltdc_fb_track_range(MemoryRegionSection *section,
                                          hwaddr *cur_base, hwaddr *cur_len,
                                          hwaddr base, hwaddr len)
{
    if (len == 0) {
        /*
         * Tear down directly instead of calling
         * framebuffer_update_memory_section(..., 0, 0, 0) -- that helper
         * calls memory_region_find(root, 0, 0) for the "new" section
         * whenever the old one is non-NULL, and a zero-size lookup at
         * address 0 can still match whatever real RAM region happens to
         * sit at guest physical address 0 (e.g. this SoC's boot flash
         * alias), silently turning DIRTY_MEMORY_VGA logging back on for
         * an unrelated region instead of actually leaving nothing
         * tracked. Confirmed via profiling: this bug meant the "torn
         * down" logging was never really gone, so the intended teardown
         * fix had no effect.
         */
        if (section->mr) {
            memory_region_set_log(section->mr, false, DIRTY_MEMORY_VGA);
            memory_region_unref(section->mr);
            section->mr = NULL;
        }
        *cur_base = 0;
        *cur_len = 0;
        return;
    }

    if (section->mr && *cur_base == base && *cur_len == len) {
        return;
    }

    framebuffer_update_memory_section(section, get_system_memory(), base, 1,
                                       len);
    *cur_base = base;
    *cur_len = len;
}

/*
 * Check (and clear, so subsequent guest writes are what's tracked for
 * next time) whether any byte in one tracked framebuffer range has been
 * written since the last check. Returns true (dirty) whenever the range
 * couldn't be resolved to plain guest RAM at all -- e.g.
 * framebuffer_update_memory_section() found no backing region, a region
 * smaller than requested, or a non-RAM region (MMIO alias, etc). Per
 * this feature's explicit design constraint: a missed dirty write would
 * freeze the screen permanently, so any case we can't positively prove
 * clean must be treated as dirty.
 */
static bool gnw_h7b0_ltdc_fb_range_dirty(MemoryRegionSection *section,
                                          hwaddr len)
{
    DirtyBitmapSnapshot *snap;
    hwaddr addr;
    bool dirty;

    if (len == 0) {
        return false;
    }
    if (!section->mr) {
        return true;
    }

    addr = section->offset_within_region;
    snap = memory_region_snapshot_and_clear_dirty(section->mr, addr, len,
                                                   DIRTY_MEMORY_VGA);
    dirty = memory_region_snapshot_get_dirty(section->mr, snap, addr, len);
    g_free(snap);
    return dirty;
}

/*
 * Primary dirty-tracking check for gnw_h7b0_ltdc_vblank_tick()'s non-VBR
 * auto-capture fallback (see that function's else-branch and
 * fb_reg_dirty's doc comment in gnw_h7b0_ltdc.h for full rationale).
 * Checks Layer1's and (if enabled) Layer2's actual guest-RAM
 * framebuffer ranges via the RAM dirty bitmap -- NOT just LTDC register
 * writes -- since firmware commonly paints new pixels directly into an
 * already-configured framebuffer without touching any LTDC register
 * again. ORs in fb_reg_dirty as an additional (not primary) signal for
 * register-only composition changes (e.g. a window move) that wouldn't
 * touch the framebuffer itself. Only called when the fallback is about
 * to actually consume a capture decision (see call site), so it's safe
 * for this call to also be the point where tracked ranges are
 * (re)bound and the dirty bitmap is cleared.
 */
static bool gnw_h7b0_ltdc_fb_dirty_check_and_clear(GnwH7B0LtdcState *s)
{
    uint32_t pfcr = s->active_l1pfcr & LTDC_LxPFCR_PF_MASK;
    bool l1_l8 = (pfcr == LTDC_PF_L8);
    int cols = s->shadow_width;
    int rows = s->shadow_height;
    bool l1_dirty, l2_dirty = false;
    hwaddr l1_len;

    /* Shadow buffer not sized yet (e.g. never captured before) -- can't
     * know the range, so force a capture attempt. */
    if (cols <= 0 || rows <= 0) {
        return true;
    }

    l1_len = (hwaddr)(cols * (l1_l8 ? 1 : 2)) * rows;
    gnw_h7b0_ltdc_fb_track_range(&s->fb_l1_section, &s->fb_l1_track_base,
                                  &s->fb_l1_track_len, s->active_l1cfbar,
                                  l1_len);
    l1_dirty = gnw_h7b0_ltdc_fb_range_dirty(&s->fb_l1_section, l1_len);

    if (s->active_l2cr & LTDC_LxCR_LEN) {
        uint32_t l2_pfcr = s->active_l2pfcr & LTDC_LxPFCR_PF_MASK;
        int l2_bpp = gnw_h7b0_ltdc_l2_bpp(l2_pfcr);

        if (l2_bpp > 0) {
            int l2_src_width =
                gnw_h7b0_ltdc_layer_stride_bytes(s->active_l2cfblr);
            hwaddr l2_len = (hwaddr)l2_src_width * rows;

            gnw_h7b0_ltdc_fb_track_range(&s->fb_l2_section,
                                          &s->fb_l2_track_base,
                                          &s->fb_l2_track_len,
                                          s->active_l2cfbar, l2_len);
            l2_dirty = gnw_h7b0_ltdc_fb_range_dirty(&s->fb_l2_section,
                                                     l2_len);
        } else {
            gnw_h7b0_ltdc_fb_track_range(&s->fb_l2_section,
                                          &s->fb_l2_track_base,
                                          &s->fb_l2_track_len, 0, 0);
        }
    } else {
        gnw_h7b0_ltdc_fb_track_range(&s->fb_l2_section, &s->fb_l2_track_base,
                                      &s->fb_l2_track_len, 0, 0);
    }

    return l1_dirty || l2_dirty || s->fb_reg_dirty;
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

/*
 * Decodes one already-fetched pixel from `buf` (part of a row batch-read by
 * the caller) -- see gnw_h7b0_ltdc_capture_rows()'s l2_linebuf comment for
 * why this takes an in-memory buffer instead of a guest physical address
 * plus its own cpu_physical_memory_read() call per pixel.
 */
static uint32_t gnw_h7b0_ltdc_read_pixel_buf(const uint8_t *buf, uint32_t pfcr)
{
    switch (pfcr) {
    case 0: /* ARGB8888 */
        return ldl_le_p(buf);
    case 2: /* RGB565 */
        return gnw_h7b0_ltdc_rgb565_to_pixel32(lduw_le_p(buf));
    case 3: /* ARGB1555 */
    {
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

    src_width = gnw_h7b0_ltdc_layer_stride_bytes(cfblr);
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

/*
 * Generalized STM32H7 LTDC blend: applies LxBFCR's BF1/BF2 fields
 * literally rather than assuming a fixed PAxCA-with-complement formula.
 * BF1 (this layer's own weight) is either the layer's constant alpha
 * (CACR) alone (register value 0x4/0x5, LTDC_LxBFCR_MODE_PA clear) or
 * pixel-alpha x constant-alpha (0x6/0x7, MODE_PA set). BF2 is the
 * complement of that same computation applied to what's already
 * composited below -- real hardware's default reset value for both
 * fields is the PAxCA pair (BFCR=0x607), which is exactly the classic
 * "over" operator this produces: factor1 = pa*ca/255, factor2 =
 * 255-factor1. For the common case (CACR=255, pa=255, i.e. an opaque
 * RGB565/L8 source with default blend factors) factor1=255/factor2=0,
 * so the result is the foreground pixel unmodified -- matching the
 * previous hardcoded behavior exactly.
 */
static uint32_t gnw_h7b0_ltdc_blend_over(uint32_t fg, unsigned int fg_a,
                                          uint32_t bfcr, unsigned int ca,
                                          uint32_t bg)
{
    unsigned int bf1 = (bfcr >> LTDC_LxBFCR_BF1_SHIFT) & LTDC_LxBFCR_BF1_MASK;
    unsigned int bf2 = bfcr & LTDC_LxBFCR_BF2_MASK;

    /*
     * Fast paths for the two overwhelmingly common cases in any
     * alpha-blended overlay (anti-aliased text/UI: mostly fully
     * transparent or fully opaque pixels, with only edge pixels
     * partially blended) -- skips the divisions below entirely. Only
     * safe when both factors use PA (per-pixel-alpha) mode -- the
     * default/reset BFCR value and the only mode retro-go/stock
     * firmware's AL44 overlays configure (see the file comment above) --
     * since with constant-alpha (non-PA) mode a "fully transparent"
     * source pixel can still contribute via a fixed ca, and skipping the
     * general path would silently drop that contribution.
     */
    if ((bf1 & LTDC_LxBFCR_MODE_PA) && (bf2 & LTDC_LxBFCR_MODE_PA)) {
        if (fg_a == 0) {
            return bg;
        }
        if (fg_a == 255 && ca == 255) {
            return fg;
        }
    }

    unsigned int factor1 = (bf1 & LTDC_LxBFCR_MODE_PA) ? (fg_a * ca) / 255U : ca;
    unsigned int factor2_base = (bf2 & LTDC_LxBFCR_MODE_PA) ? (fg_a * ca) / 255U : ca;
    unsigned int factor2 = 255U - factor2_base;

    unsigned int fg_r = (fg >> 16) & 0xFF, fg_g = (fg >> 8) & 0xFF, fg_b = fg & 0xFF;
    unsigned int bg_r = (bg >> 16) & 0xFF, bg_g = (bg >> 8) & 0xFF, bg_b = bg & 0xFF;

    unsigned int r = MIN(255U, (fg_r * factor1 + bg_r * factor2) / 255U);
    unsigned int g = MIN(255U, (fg_g * factor1 + bg_g * factor2) / 255U);
    unsigned int b = MIN(255U, (fg_b * factor1 + bg_b * factor2) / 255U);

    return 0xFF000000U | (r << 16) | (g << 8) | b;
}

/*
 * Resolve one layer's raw fetched pixel (already-converted ARGB, native
 * alpha for ARGB formats or 255 for opaque RGB565/L8) against that
 * layer's window-clip and color-key configuration, returning the
 * effective ARGB pixel to hand to gnw_h7b0_ltdc_blend_over() -- outside
 * the layer's window it's DCCR's default color+alpha instead of
 * framebuffer content (DCCR's bit layout, alpha[31:24]/RGB[23:16:8:0],
 * is already a valid ARGB32 value); a color-key match forces alpha to 0
 * so the pixel is fully transparent (falls through to what's below),
 * independent of that layer's blend-factor mode.
 */
static uint32_t gnw_h7b0_ltdc_resolve_layer(uint32_t raw_px, bool in_window,
                                             uint32_t dccr, bool colken,
                                             uint32_t ckcr,
                                             unsigned int *out_alpha)
{
    uint32_t px;
    unsigned int alpha;

    if (!in_window) {
        px = dccr;
        alpha = (dccr >> 24) & 0xFF;
    } else if (colken && (raw_px & 0xFFFFFFU) == (ckcr & 0xFFFFFFU)) {
        px = raw_px;
        alpha = 0;
    } else {
        px = raw_px;
        alpha = (raw_px >> 24) & 0xFF;
    }

    *out_alpha = alpha;
    return px;
}

static const int gnw_h7b0_ltdc_bayer4x4[4][4] = {
    { 0,  8,  2, 10 },
    {12,  4, 14,  6 },
    { 3, 11,  1,  9 },
    {15,  7, 13,  5 },
};

/*
 * Standard 4x4 Bayer ordered dither, gated on GCR.DEN. Confirmed a true
 * no-op for this model today: shadow_buffer and the host display surface
 * are both full 32bpp/8-bit-per-channel, so nothing downstream ever
 * quantizes bit depth (unlike a real TFT panel, which this hardware
 * feature targets). Implemented anyway for fidelity/future-proofing (per
 * explicit request), using the same truncate-then-replicate-low-bits
 * re-expansion pattern as gnw_h7b0_ltdc_rgb565_to_pixel32().
 */
static inline uint8_t gnw_h7b0_ltdc_dither_channel(uint8_t value, int px, int py)
{
    const int n = 6; /* assumed panel bit depth (e.g. RGB666) */
    const int step = 1 << (8 - n);
    int d = gnw_h7b0_ltdc_bayer4x4[py & 3][px & 3];
    int adjusted = (int)value + (d * step) / 16 - step / 2;
    unsigned int truncated;
    unsigned int nbit;

    adjusted = MAX(0, MIN(255, adjusted));
    truncated = ((unsigned int)adjusted) & ~(unsigned int)(step - 1);
    nbit = truncated >> (8 - n);

    return (uint8_t)((nbit << (8 - n)) | (nbit >> (2 * n - 8)));
}

static inline uint32_t gnw_h7b0_ltdc_dither_pixel(uint32_t px, int x, int y)
{
    unsigned int r = gnw_h7b0_ltdc_dither_channel((px >> 16) & 0xFF, x, y);
    unsigned int g = gnw_h7b0_ltdc_dither_channel((px >> 8) & 0xFF, x, y);
    unsigned int b = gnw_h7b0_ltdc_dither_channel(px & 0xFF, x, y);

    return (px & 0xFF000000U) | (r << 16) | (g << 8) | b;
}

static void gnw_h7b0_ltdc_capture_rows(GnwH7B0LtdcState *s, int row_start,
                                        int row_end)
{
    uint32_t cfbar = s->active_l1cfbar;
    int cols = s->shadow_width;
    uint32_t pfcr = s->active_l1pfcr & LTDC_LxPFCR_PF_MASK;
    bool l1_l8 = (pfcr == LTDC_PF_L8);
    int src_width = cols * (l1_l8 ? 1 : 2);
    int nrows = row_end - row_start;

    bool l1_colken = s->active_l1cr & LTDC_LxCR_COLKEN;
    uint32_t l1_ckcr = s->active_l1ckcr;
    uint32_t l1_dccr = s->active_l1dccr;
    uint32_t l1_bfcr = s->active_l1bfcr;
    uint32_t l1_cacr = s->active_l1cacr & 0xFF;
    int l1_whstpos = s->active_l1whpcr & LTDC_LxWHPCR_WHSTPOS_MASK;
    int l1_whsppos = (s->active_l1whpcr >> LTDC_LxWHPCR_WHSPPOS_SHIFT) &
                      LTDC_LxWHPCR_WHSPPOS_MASK;
    int l1_wvstpos = s->active_l1wvpcr & LTDC_LxWVPCR_WVSTPOS_MASK;
    int l1_wvsppos = (s->active_l1wvpcr >> LTDC_LxWVPCR_WVSPPOS_SHIFT) &
                      LTDC_LxWVPCR_WVSPPOS_MASK;

    bool l2_en = s->active_l2cr & LTDC_LxCR_LEN;
    uint32_t l2_cfbar = s->active_l2cfbar;
    uint32_t l2_pfcr = s->active_l2pfcr & LTDC_LxPFCR_PF_MASK;
    bool l2_l8 = (l2_pfcr == LTDC_PF_L8);
    bool l2_al44 = (l2_pfcr == LTDC_PF_AL44);
    uint32_t l2_cfblr = s->active_l2cfblr;
    int l2_src_width = gnw_h7b0_ltdc_layer_stride_bytes(l2_cfblr);
    int l2_bpp = 0;
    if (l2_en) {
        switch (l2_pfcr) {
        case 0: l2_bpp = 4; break;
        case 2: case 3: case 4: l2_bpp = 2; break;
        case 5: l2_bpp = 1; break; /* L8 */
        case 6: l2_bpp = 1; break; /* AL44 -- 4-bit alpha + 4-bit luminance */
        default: l2_bpp = 0; break;
        }
    }
    uint32_t l2_cacr = s->active_l2cacr & 0xFF;
    bool l2_colken = s->active_l2cr & LTDC_LxCR_COLKEN;
    uint32_t l2_ckcr = s->active_l2ckcr;
    uint32_t l2_dccr = s->active_l2dccr;
    uint32_t l2_bfcr = s->active_l2bfcr;
    int l2_whstpos = s->active_l2whpcr & LTDC_LxWHPCR_WHSTPOS_MASK;
    int l2_whsppos = (s->active_l2whpcr >> LTDC_LxWHPCR_WHSPPOS_SHIFT) &
                      LTDC_LxWHPCR_WHSPPOS_MASK;
    int l2_wvstpos = s->active_l2wvpcr & LTDC_LxWVPCR_WVSTPOS_MASK;
    int l2_wvsppos = (s->active_l2wvpcr >> LTDC_LxWVPCR_WVSPPOS_SHIFT) &
                      LTDC_LxWVPCR_WVSPPOS_MASK;

    uint32_t bccr = 0xFF000000U | (s->active_bccr & 0xFFFFFFU);
    bool dither_en = s->active_gcr & LTDC_GCR_DEN;

    int ahbp = (s->active_bpcr >> LTDC_BPCR_AHBP_SHIFT) & LTDC_BPCR_AHBP_MASK;
    int avbp = s->active_bpcr & LTDC_BPCR_AVBP_MASK;

    /*
     * Batch Layer1 and Layer2's *entire* row span into one buffer each with
     * one cpu_physical_memory_read() call total, instead of one call per
     * row (this function is always called with row_start=0, i.e. the whole
     * frame in one span, so every row is contiguous guest memory -- no
     * per-row gaps to work around). This is on top of 2026-07-13's earlier
     * per-*pixel*-to-per-*row* batching fix for Layer2 (see CHANGELOG):
     * that fix cut ~240*392*60Hz calls/sec down to ~240*60Hz; this cuts
     * that ~240*60Hz down to ~60Hz (one read per full-frame capture instead
     * of one per scanline). g_malloc(0) is valid and returns NULL, so this
     * is safe even when Layer2 is disabled.
     */
    g_autofree uint8_t *framebuf = g_malloc((size_t)src_width * nrows);
    g_autofree uint8_t *l2_framebuf =
        g_malloc(l2_en ? (size_t)l2_src_width * nrows : 0);

    cpu_physical_memory_read(cfbar + (hwaddr)row_start * src_width, framebuf,
                             (size_t)src_width * nrows);
    if (l2_en && l2_bpp > 0) {
        cpu_physical_memory_read(l2_cfbar + (hwaddr)row_start * l2_src_width,
                                 l2_framebuf, (size_t)l2_src_width * nrows);
    }

    /*
     * l1_whstpos/whsppos (and l2's) don't depend on y, so the horizontal
     * window-clip test is identical for every row -- precompute it once per
     * column instead of re-evaluating two comparisons per pixel per row.
     */
    g_autofree bool *l1_x_in = g_malloc(cols * sizeof(bool));
    g_autofree bool *l2_x_in = g_malloc(cols * sizeof(bool));
    for (int x = 0; x < cols; x++) {
        int abs_x = ahbp + x + 1;
        l1_x_in[x] = abs_x >= l1_whstpos && abs_x <= l1_whsppos;
        l2_x_in[x] = abs_x >= l2_whstpos && abs_x <= l2_whsppos;
    }

    for (int y = row_start; y < row_end; y++) {
        int abs_y = avbp + y + 1;
        bool l1_row_in = abs_y >= l1_wvstpos && abs_y <= l1_wvsppos;
        bool l2_row_in = abs_y >= l2_wvstpos && abs_y <= l2_wvsppos;
        uint8_t *linebuf = framebuf + (hwaddr)(y - row_start) * src_width;
        uint8_t *l2_linebuf =
            l2_framebuf + (hwaddr)(y - row_start) * l2_src_width;

        for (int x = 0; x < cols; x++) {
            int abs_x = ahbp + x + 1;
            bool l1_in = l1_row_in && l1_x_in[x];
            uint32_t l1_raw = l1_l8 ? s->clut[linebuf[x]]
                                    : gnw_h7b0_ltdc_rgb565_to_pixel32(
                                          lduw_le_p(linebuf + x * 2));

            /*
             * Layer1 first/bottom, composited over the opaque background
             * color (BCCR); Layer2 second/top (foreground), composited
             * over that result -- real STM32 LTDC hardware always
             * displays Layer2 above/in front of Layer1 (RM0455's LTDC
             * overview: "Layer 2 is displayed in the foreground of
             * Layer 1").
             */
            unsigned int l1_alpha;
            uint32_t l1_resolved = gnw_h7b0_ltdc_resolve_layer(
                l1_raw, l1_in, l1_dccr, l1_colken, l1_ckcr, &l1_alpha);
            uint32_t below = gnw_h7b0_ltdc_blend_over(l1_resolved, l1_alpha,
                                                       l1_bfcr, l1_cacr, bccr);

            if (l2_en && l2_bpp > 0) {
                bool l2_in = l2_row_in && l2_x_in[x];
                uint32_t l2_raw;
                unsigned int l2_alpha;
                uint32_t l2_resolved;

                if (l2_l8) {
                    uint8_t idx = l2_linebuf[x];
                    l2_raw = s->clut2[idx];
                } else if (l2_al44) {
                    /*
                     * AL44 (A4L4): raw byte packs a 4-bit alpha (upper
                     * nibble) and a 4-bit luminance (lower nibble) --
                     * looked up via the luminance nibble replicated to a
                     * full byte (matching ConfigCLUT's own idx encoding)
                     * for RGB, with the pixel's own alpha nibble applied
                     * directly (replicated to 8 bits), overriding
                     * whatever alpha the CLUT entry itself carries.
                     */
                    uint8_t raw = l2_linebuf[x];
                    unsigned int a4 = (raw >> 4) & 0xF;
                    unsigned int l4 = raw & 0xF;
                    unsigned int a8 = a4 * 0x11U;
                    uint32_t clut_rgb = s->clut2[l4 * 0x11U] & 0x00FFFFFFU;
                    l2_raw = (a8 << 24) | clut_rgb;
                } else {
                    l2_raw = gnw_h7b0_ltdc_read_pixel_buf(
                        l2_linebuf + x * l2_bpp, l2_pfcr);
                }

                l2_resolved = gnw_h7b0_ltdc_resolve_layer(
                    l2_raw, l2_in, l2_dccr, l2_colken, l2_ckcr, &l2_alpha);
                below = gnw_h7b0_ltdc_blend_over(l2_resolved, l2_alpha,
                                                  l2_bfcr, l2_cacr, below);
            }

            uint32_t px = below;

            if (dither_en) {
                px = gnw_h7b0_ltdc_dither_pixel(px, abs_x, abs_y);
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
        s->content_dirty = false;
        return;
    }

    if (surface_bits_per_pixel(surface) != 32) {
        s->content_dirty = false;
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
         *
         * RRIF must be raised here too: real hardware sets it for any
         * reload that actually takes effect, immediate or deferred, and
         * an immediate reload here is exactly that -- it was previously
         * only ever set (unconditionally, wrongly) from the vblank tick,
         * so IMR reloads never raised it at all.
         */
        s->srcr_idle_ticks = 0;
        if (value & LTDC_SRCR_IMR) {
            /*
             * An IMR reload means firmware is applying a fresh layer/
             * format config -- the natural checkpoint for "a new screen
             * just took over the layers, prove it uses VBR before we
             * disable the no-VBR fallback for it again" (see the
             * vblank_tick() comment for why this isn't a permanent
             * latch or a timeout).
             */
            s->vbr_active = false;
            gnw_h7b0_ltdc_reload_active(s);
            s->regs[GNW_H7B0_LTDC_ISR >> 2] |= LTDC_ISR_RRIF;
            gnw_h7b0_ltdc_update_irq(s);
            /*
             * HAL_LTDC_SetPixelFormat() and friends use IMR reloads for
             * on-the-fly format/geometry changes. Unlike VBR (which
             * captures the outgoing frame *before* the reload for double
             * buffering), IMR takes effect instantly -- capture right away
             * with the new active configuration or the host display never
             * picks up the new framebuffer layout (lcd_setup_framebuffers()
             * calls SetPixelFormat/SetAddress then a separate VBR reload,
             * but intermediate IMR reloads from the HAL must still refresh).
             */
            gnw_h7b0_ltdc_capture_if_enabled(s);
        }
        if (value & LTDC_SRCR_VBR) {
            s->vbr_reload_pending = true;
            s->vbr_active = true;

            /*
             * Tear down the RAM dirty-bitmap tracking bound for the
             * non-VBR fallback (see gnw_h7b0_ltdc_fb_dirty_check_and_clear()
             * and fb_reg_dirty's doc comment in gnw_h7b0_ltdc.h) now that
             * VBR-paced double buffering is active again -- leaving
             * DIRTY_MEMORY_VGA logging bound to the framebuffer taxes
             * *every* guest write into it via QEMU's slow notdirty TLB
             * path (profiled: >50% of total CPU cycles during active
             * gameplay), and once bound it otherwise stays bound
             * indefinitely (gnw_h7b0_ltdc_fb_track_range() only rebinds
             * when the tracked range changes, which a game's fixed
             * framebuffer address usually never does). The fallback lazily
             * re-binds it the next time it's actually needed, so this is
             * safe to tear down unconditionally here.
             */
            gnw_h7b0_ltdc_fb_track_range(&s->fb_l1_section,
                                          &s->fb_l1_track_base,
                                          &s->fb_l1_track_len, 0, 0);
            gnw_h7b0_ltdc_fb_track_range(&s->fb_l2_section,
                                          &s->fb_l2_track_base,
                                          &s->fb_l2_track_len, 0, 0);

            /* The guest has finished drawing the frame and requested a swap.
             * Capture it NOW to avoid capturing it mid-draw during the next
             * frame. Only capture if the UI thread has finished blitting the
             * previous capture. */
            if (!s->content_dirty) {
                gnw_h7b0_ltdc_capture_if_enabled(s);
                s->vbr_deferred_capture = false;
            } else {
                s->vbr_deferred_capture = true;
            }
        }
        s->regs[addr >> 2] = value & ~LTDC_SRCR_IMR;
        /*
         * Re-derive the vblank/line timer rate on every reload request:
         * the timers otherwise only re-arm at tick time, so a single
         * recalculation made against a transient (mid-PLL3-programming)
         * clock config can arm a pathologically long period that wedges
         * the whole frame-tick chain (stock's main loop is paced by the
         * reload-interrupt -> frame-flag path) until it finally fires.
         * Firmware writes SRCR once per frame, making this the natural
         * self-healing point.
         */
        gnw_h7b0_ltdc_recalc_timers(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
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
    case GNW_H7B0_LTDC_L2CLUTWR:
    {
        /* Mirrors L1CLUTWR above, but into Layer2's own independent
         * CLUT -- real hardware has separate CLUTs per layer. */
        unsigned int idx = (value >> 24) & 0xFF;
        unsigned int r = (value >> 16) & 0xFF;
        unsigned int g = (value >> 8) & 0xFF;
        unsigned int b = value & 0xFF;
        s->clut2[idx] = 0xFF000000U | (r << 16) | (g << 8) | b;
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

static void gnw_h7b0_ltdc_class_init(ObjectClass *klass, const void *data)
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
