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
static void gnw_h7b0_ltdc_composite_from_job(GnwH7B0LtdcState *s,
                                              GnwH7B0LtdcCaptureJob *job,
                                              uint32_t *out);
static void gnw_h7b0_ltdc_wait_compositor_idle(GnwH7B0LtdcState *s);
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

static void gnw_h7b0_ltdc_snapshot_and_dispatch(GnwH7B0LtdcState *s, int rows);

/*
 * Cheap peek at job_busy, used by vblank_tick()'s non-VBR fallback to
 * avoid calling gnw_h7b0_ltdc_fb_dirty_check_and_clear() (which
 * *consumes* the RAM dirty bitmap -- see that function's doc comment)
 * for a capture we already know gnw_h7b0_ltdc_capture_if_enabled() would
 * just drop for being busy. Without this, the dirty-bitmap signal would
 * be lost even though no capture actually happened -- a real gap
 * capture_if_enabled()'s own return-value checks alone don't cover,
 * since by the time it runs the dirty bitmap would already be cleared.
 */
static bool gnw_h7b0_ltdc_compositor_busy(GnwH7B0LtdcState *s)
{
    bool busy;

    qemu_mutex_lock(&s->job_lock);
    busy = s->job_busy;
    qemu_mutex_unlock(&s->job_lock);
    return busy;
}

/*
 * Return true when a capture job was actually dispatched to the
 * compositor worker thread this call (NOT when the resulting frame has
 * finished compositing -- that happens asynchronously; see
 * gnw_h7b0_ltdc_composite_from_job() and the worker thread function).
 * Returns false if LTDC/Layer1 is disabled/unsupported, or if the worker
 * is still busy with a previous frame (job_busy) -- every call site
 * that needs eventual delivery of a dropped request (SRCR.VBR's
 * vbr_deferred_capture, the non-VBR fallback's fb_reg_dirty) already
 * checks this return value and retries on the next vblank tick instead
 * of assuming a call here always succeeds, which is the one behavior
 * change this device's existing capture-gating logic (all of which
 * predates and is unrelated to the compositor thread -- vbr_active,
 * structural_transition_pending, the RAM-dirty-bitmap fallback -- see
 * gnw_h7b0_ltdc_vblank_tick()) needed to keep working correctly now that
 * a "capture" is no longer instantaneous.
 */
static bool gnw_h7b0_ltdc_capture_if_enabled(GnwH7B0LtdcState *s)
{
    int rows;

    if (!gnw_h7b0_ltdc_enabled(s)) {
        return false;
    }

    /*
     * Check job_busy BEFORE calling capture_setup(): capture_setup() may
     * resize shadow_buffer/compositor_back_buffer, which is only safe
     * while the compositor worker thread is provably not touching either
     * buffer. The producer side (this function) always runs with the
     * BQL held, so it can never race a *concurrent* producer -- only the
     * worker matters here, and job_busy==false is exactly "the worker
     * already published its last frame and is asleep waiting for work".
     */
    qemu_mutex_lock(&s->job_lock);
    if (s->job_busy) {
        qemu_mutex_unlock(&s->job_lock);
        return false;
    }
    qemu_mutex_unlock(&s->job_lock);

    rows = gnw_h7b0_ltdc_capture_setup(s);
    if (rows <= 0) {
        return false;
    }

    gnw_h7b0_ltdc_snapshot_and_dispatch(s, rows);
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

/*
 * Whether this reload represents a genuine layer/screen transition
 * (format, layer-enable, or window geometry differs from the
 * currently-active config) as opposed to routine same-screen double
 * buffering, which only ever changes CFBAR (the buffer address) --
 * deliberately excluded from this comparison since it flips every
 * frame during completely normal steady-state VBR-paced gameplay. See
 * srcr_idle_ticks'/structural_transition_pending's doc comments in
 * gnw_h7b0_ltdc.h for why this distinction matters.
 */
static bool gnw_h7b0_ltdc_reload_is_structural_change(GnwH7B0LtdcState *s)
{
    return s->active_l1pfcr != s->regs[GNW_H7B0_LTDC_L1PFCR >> 2] ||
           (s->active_l1cr & LTDC_LxCR_LEN) !=
               (s->regs[GNW_H7B0_LTDC_L1CR >> 2] & LTDC_LxCR_LEN) ||
           s->active_l2pfcr != s->regs[GNW_H7B0_LTDC_L2PFCR >> 2] ||
           (s->active_l2cr & LTDC_LxCR_LEN) !=
               (s->regs[GNW_H7B0_LTDC_L2CR >> 2] & LTDC_LxCR_LEN) ||
           s->active_l1whpcr != s->regs[GNW_H7B0_LTDC_L1WHPCR >> 2] ||
           s->active_l1wvpcr != s->regs[GNW_H7B0_LTDC_L1WVPCR >> 2] ||
           s->active_l2whpcr != s->regs[GNW_H7B0_LTDC_L2WHPCR >> 2] ||
           s->active_l2wvpcr != s->regs[GNW_H7B0_LTDC_L2WVPCR >> 2];
}

/*
 * True if any register gnw_h7b0_ltdc_reload_active() is about to commit
 * actually differs from what's already active -- i.e. this reload has
 * nothing new to contribute to the composited frame. Covers the exact
 * same register set reload_active() copies (both layers' CR/CFBAR/
 * CFBLR/CFBLNR/PFCR/CACR/CKCR/DCCR/BFCR/WHPCR/WVPCR plus GCR/BPCR/BCCR),
 * so "unchanged here" really does mean "the next capture would produce a
 * bit-identical frame to the last one," not just "the specific fields
 * gnw_h7b0_ltdc_reload_is_structural_change() happens to check."
 *
 * Added per stm32h7b0-diag's docs/qemu-reports/ltdc.md: any SRCR.IMR
 * write anywhere in the system -- including one that only touches an
 * always-disabled scratch Layer 2, as case_ltdc_frame_composite.c/
 * case_ltdc_clut_correct.c both deliberately do -- unconditionally
 * triggered a full 320x240 Layer-1 framebuffer
 * cpu_physical_memory_read() snapshot-and-dispatch, because the capture
 * is gated on "is LTDC enabled" (Layer 1 always is, for the real menu)
 * rather than "did anything that would change the composited frame
 * actually happen." Measured ~22x QEMU-vs-hardware gap, ~18.7us/reload
 * of real host work for a shadow-register latch that's near-instant on
 * real silicon. Skipping the capture when nothing composition-relevant
 * changed removes that cost for exactly this class of no-op reload
 * without touching real gameplay: an ordinary game's IMR-triggered
 * format/geometry reconfiguration (the case this reload path exists
 * for) always changes at least one of these registers by definition, so
 * the capture still fires for those, unchanged. If Layer 1's RAM
 * content changes without any register write at all (direct-paint
 * firmware), that's already independently covered by
 * gnw_h7b0_ltdc_vblank_tick()'s separate RAM-dirty-bitmap fallback --
 * this only skips the *register-triggered* capture, it doesn't disable
 * or interact with that other path.
 */
static bool gnw_h7b0_ltdc_reload_is_composition_change(GnwH7B0LtdcState *s)
{
    return s->active_l1cr     != s->regs[GNW_H7B0_LTDC_L1CR >> 2] ||
           s->active_l1cfbar  != s->regs[GNW_H7B0_LTDC_L1CFBAR >> 2] ||
           s->active_l1cfblr  != s->regs[GNW_H7B0_LTDC_L1CFBLR >> 2] ||
           s->active_l1cfblnr != s->regs[GNW_H7B0_LTDC_L1CFBLNR >> 2] ||
           s->active_l1pfcr   != s->regs[GNW_H7B0_LTDC_L1PFCR >> 2] ||
           s->active_l2cr     != s->regs[GNW_H7B0_LTDC_L2CR >> 2] ||
           s->active_l2cfbar  != s->regs[GNW_H7B0_LTDC_L2CFBAR >> 2] ||
           s->active_l2cfblr  != s->regs[GNW_H7B0_LTDC_L2CFBLR >> 2] ||
           s->active_l2cfblnr != s->regs[GNW_H7B0_LTDC_L2CFBLNR >> 2] ||
           s->active_l2pfcr   != s->regs[GNW_H7B0_LTDC_L2PFCR >> 2] ||
           s->active_l2cacr   != s->regs[GNW_H7B0_LTDC_L2CACR >> 2] ||
           s->active_l1ckcr   != s->regs[GNW_H7B0_LTDC_L1CKCR >> 2] ||
           s->active_l2ckcr   != s->regs[GNW_H7B0_LTDC_L2CKCR >> 2] ||
           s->active_l1dccr   != s->regs[GNW_H7B0_LTDC_L1DCCR >> 2] ||
           s->active_l2dccr   != s->regs[GNW_H7B0_LTDC_L2DCCR >> 2] ||
           s->active_bccr     != s->regs[GNW_H7B0_LTDC_BCCR >> 2] ||
           s->active_l1cacr   != s->regs[GNW_H7B0_LTDC_L1CACR >> 2] ||
           s->active_l1bfcr   != s->regs[GNW_H7B0_LTDC_L1BFCR >> 2] ||
           s->active_l2bfcr   != s->regs[GNW_H7B0_LTDC_L2BFCR >> 2] ||
           s->active_l1whpcr  != s->regs[GNW_H7B0_LTDC_L1WHPCR >> 2] ||
           s->active_l1wvpcr  != s->regs[GNW_H7B0_LTDC_L1WVPCR >> 2] ||
           s->active_l2whpcr  != s->regs[GNW_H7B0_LTDC_L2WHPCR >> 2] ||
           s->active_l2wvpcr  != s->regs[GNW_H7B0_LTDC_L2WVPCR >> 2] ||
           s->active_gcr      != s->regs[GNW_H7B0_LTDC_GCR >> 2] ||
           s->active_bpcr     != s->regs[GNW_H7B0_LTDC_BPCR >> 2];
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

    if (gnw_h7b0_ltdc_reload_is_structural_change(s)) {
        s->structural_transition_pending = true;
    }

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

static void gnw_h7b0_ltdc_timing(GnwH7B0LtdcState *s, int64_t *frame_ns_out,
                                  int64_t *line_ns_out, uint32_t *lipcr_out,
                                  uint32_t *totalh_out);

/*
 * Re-derives the frame period from live clock/timing registers and
 * re-arms the timers -- PHASE-PRESERVING. The vblank deadline grid is
 * kept if it is still sane (strictly in the future, no further than one
 * freshly computed frame away) and only re-anchored at "now" when it is
 * stale, unarmed, or wedged against a transiently-huge period (the
 * mid-PLL3-programming case the SRCR-write call site exists for).
 *
 * Phase preservation is load-bearing for audio, not cosmetic: firmware
 * writes SRCR.VBR once per frame ~1.4ms after taking the vblank IRQ,
 * and the previous unconditional "deadline = now + frame_ns" here moved
 * the next vblank a full period past *that write* -- stretching every
 * frame to (guest work + 16.65ms) ~= 18.1ms. Stock Mario's NES-emulator
 * audio producer is paced by this exact vsync chain while its audio DMA
 * drains at a metronomic 48kHz, so those stretched frames were a
 * structural ~8% sample shortfall: the long-standing "crunchy audio"
 * bug (measured: 55.15fps, ~20% of DMA chunks handed off as ring-
 * underrun silence; see CHANGELOG 2026-07-22).
 */
static void gnw_h7b0_ltdc_recalc_timers(GnwH7B0LtdcState *s, int64_t now)
{
    int64_t frame_ns, line_ns;
    uint32_t lipcr, totalh;
    int64_t next;

    gnw_h7b0_ltdc_timing(s, &frame_ns, &line_ns, &lipcr, &totalh);

    next = s->vblank_deadline_ns;
    if (next <= now || next > now + frame_ns) {
        next = now + frame_ns;
    }
    s->vblank_deadline_ns = next;
    timer_mod(s->vblank_timer, next);

    if (lipcr < totalh) {
        /* The line event belongs to the in-flight frame (start = next -
         * frame_ns); if that moment already passed, arm it for the next
         * frame instead of firing a stale one immediately. */
        int64_t line_t = next - frame_ns + lipcr * line_ns;
        timer_mod(s->line_timer, line_t > now ? line_t
                                               : next + lipcr * line_ns);
    } else {
        timer_del(s->line_timer);
    }
}

static void gnw_h7b0_ltdc_timing(GnwH7B0LtdcState *s, int64_t *frame_ns_out,
                                  int64_t *line_ns_out, uint32_t *lipcr_out,
                                  uint32_t *totalh_out)
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
    /*
     * Exact fractional frame period, not a truncated-to-integer-Hz one:
     * stock Mario's real rate is 60.05Hz (pll3r 6.042MHz / 393x256), and
     * even the 0.08% loss from rounding that down to a flat 60Hz is a
     * systematic production shortfall for the vsync-paced audio producer
     * (see vblank_deadline_ns's doc comment).
     */
    int64_t frame_ns = NANOSECONDS_PER_SECOND / GNW_H7B0_LTDC_VBLANK_HZ;
    if (s->rcc && totalw > 0 && totalh > 0) {
        uint32_t pll3r_hz = gnw_h7b0_rcc_get_pll3r_hz(s->rcc);
        uint64_t pixels = (uint64_t)totalw * totalh;

        if (pll3r_hz > 0 && pixels > 0) {
            int64_t real_ns = muldiv64(pixels, NANOSECONDS_PER_SECOND,
                                        pll3r_hz);
            /* Same 1..1000Hz sanity window as before, in period form. */
            if (real_ns >= 1000000 && real_ns <= 1000000000) {
                frame_ns = real_ns;
            }
        }
    }

    *frame_ns_out = frame_ns;
    *line_ns_out = frame_ns / totalh;
    *lipcr_out = lipcr;
    *totalh_out = totalh;
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
         * fallback even with vbr_active still true -- but ONLY if the
         * last reload also represented a genuine structural transition
         * (structural_transition_pending), not just an idle timer:
         * confirmed live that ordinary in-game frame-skipping suppresses
         * VBR reloads for the exact same duration as a real abandoned
         * transition, so the idle timer alone (even combined with the
         * RAM-dirty check) fires just as often during normal stutter,
         * reintroducing mid-draw tearing/flicker -- see
         * structural_transition_pending's doc comment in gnw_h7b0_ltdc.h.
         */
        bool vbr_idle = s->srcr_idle_ticks >= GNW_H7B0_LTDC_SRCR_IDLE_TICKS_THRESHOLD &&
                         s->structural_transition_pending;
        if ((!s->vbr_active || vbr_idle) && !s->content_dirty &&
            gnw_h7b0_ltdc_enabled(s) &&
            !gnw_h7b0_ltdc_compositor_busy(s) &&
            gnw_h7b0_ltdc_fb_dirty_check_and_clear(s)) {
            /*
             * Only clear fb_reg_dirty if the dispatch actually happened
             * (it always should here, given the !compositor_busy() check
             * just above -- gnw_h7b0_ltdc_capture_if_enabled() can still
             * legitimately return false for other reasons, e.g. an
             * unsupported Layer1 pixel format, in which case there is
             * nothing to redo next tick and leaving fb_reg_dirty set
             * would just harmlessly re-check RAM dirtiness again).
             */
            if (gnw_h7b0_ltdc_capture_if_enabled(s)) {
                s->fb_reg_dirty = false;
            }
        }
    }

    /*
     * Retried every vblank tick (not just the one where vbr_reload_pending
     * just resolved) since the compositor worker thread may still have
     * been busy with a previous frame the first time this was attempted
     * (gnw_h7b0_ltdc_capture_if_enabled() returns false, without
     * dispatching, when job_busy) -- only clear the flag once a job is
     * actually dispatched, so a deferred capture is never silently lost
     * to worker-thread backpressure the way it could be if this only
     * fired once inside the vbr_reload_pending branch above.
     */
    if (s->vbr_deferred_capture) {
        if (gnw_h7b0_ltdc_capture_if_enabled(s)) {
            s->vbr_deferred_capture = false;
        }
    }

    /*
     * Re-arm on the deadline grid, not at "now + frame_ns" -- see
     * vblank_deadline_ns's doc comment (and gnw_h7b0_dma_schedule_next(),
     * which this mirrors). Clamp only when more than a full frame
     * behind (host stall, debugger attach), so ordinary dispatch
     * latency never shifts the grid.
     */
    {
        int64_t frame_ns, line_ns;
        uint32_t lipcr, totalh;
        int64_t frame_start;

        gnw_h7b0_ltdc_timing(s, &frame_ns, &line_ns, &lipcr, &totalh);

        frame_start = s->vblank_deadline_ns;
        if (frame_start < now - frame_ns) {
            frame_start = now;
        }
        s->vblank_deadline_ns = frame_start + frame_ns;
        timer_mod(s->vblank_timer, s->vblank_deadline_ns);

        if (lipcr < totalh) {
            timer_mod(s->line_timer, frame_start + lipcr * line_ns);
        } else {
            timer_del(s->line_timer);
        }
    }
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

    /*
     * Reset can run at any time, including with a compositor job still
     * in flight on the worker thread -- wait for it to finish and
     * publish before tearing down shadow_buffer/compositor_back_buffer/
     * clut below, or the worker's pending pointer swap/CLUT read could
     * race a free() here (a real use-after-free, not a style nit: reset
     * is BQL-synchronous but the worker thread is not).
     */
    if (s->compositor_running) {
        gnw_h7b0_ltdc_wait_compositor_idle(s);
    }

    for (int i = 0; i < (GNW_H7B0_LTDC_SIZE / 4); i++) {
        s->regs[i] = get_ltdc_reset_value(i * 4);
    }
    s->invalidate = 1;
    s->pf_warned = false;
    s->vbr_reload_pending = false;
    s->vbr_deferred_capture = false;
    s->vbr_active = false;
    s->srcr_idle_ticks = 0;
    s->structural_transition_pending = false;

    g_free(s->shadow_buffer);
    s->shadow_buffer = NULL;
    g_free(s->compositor_back_buffer);
    s->compositor_back_buffer = NULL;
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
    /*
     * memory_region_snapshot_and_clear_dirty()/_get_dirty() both do
     * assert(mr->ram_block) internally -- fine in a debug build (loud
     * abort), but assertions compile out under NDEBUG (this project's
     * release builds), silently proceeding into undefined behavior on
     * a non-RAM-backed region instead. Confirmed via a real,
     * reproducible segfault in a release/PGO build at this exact call
     * site (deterministic: identical link-time crash address across
     * multiple runs) -- check memory_region_is_ram() ourselves, a real
     * runtime check that can't be compiled away, instead of trusting
     * QEMU's own assert to catch this.
     */
    if (!section->mr || !memory_region_is_ram(section->mr)) {
        return true;
    }

    addr = section->offset_within_region;
    snap = memory_region_snapshot_and_clear_dirty(section->mr, addr, len,
                                                   DIRTY_MEMORY_VGA);
    if (!snap) {
        return true;
    }
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
        /*
         * Only reachable when the compositor worker thread is provably
         * idle (see gnw_h7b0_ltdc_capture_if_enabled()'s job_busy check,
         * always performed before this function is called), so neither
         * buffer can be concurrently read/written by the worker here.
         * Still take compositor_publish_lock around the shadow_buffer/
         * dimensions update, since gnw_h7b0_ltdc_update_display() (main
         * thread) may be reading the old pointer/dims right up until
         * this resize.
         */
        qemu_mutex_lock(&s->compositor_publish_lock);
        g_free(s->shadow_buffer);
        s->shadow_buffer = g_new0(uint32_t, cols * rows);
        s->shadow_width = cols;
        s->shadow_height = rows;
        qemu_mutex_unlock(&s->compositor_publish_lock);

        g_free(s->compositor_back_buffer);
        s->compositor_back_buffer = g_new0(uint32_t, cols * rows);
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

/*
 * Row-level SIMD fast path for the single most common per-pixel case in
 * this loop: Layer1 RGB565, fully opaque, default (PAxCA) blend factors,
 * no color-key, no dithering, and the whole row inside Layer1's window --
 * i.e. ordinary in-game rendering with no overlay active. See the gate
 * check inlined at its call site in gnw_h7b0_ltdc_capture_rows() for the
 * exact conditions; anything that doesn't match falls through to the
 * general per-pixel path unchanged (bit-for-bit -- verified against a
 * standalone host-side harness comparing every possible RGB565 value).
 *
 * Only RGB565->ARGB8888 unpack is vectorized (the L8/CLUT and Layer2
 * paths involve per-pixel gathers/branches that don't vectorize cleanly
 * with SSE2 and aren't worth the risk here) -- gated to hosts with SSE2
 * (baseline for every x86_64 target QEMU supports); every other host
 * arch/config uses the identical-math scalar fallback below.
 */
#if defined(__SSE2__)
#include <emmintrin.h>
#define GNW_H7B0_LTDC_HAVE_SSE2_ROW_CONVERT 1
#endif

#ifdef GNW_H7B0_LTDC_HAVE_SSE2_ROW_CONVERT
/*
 * Converts `cols` RGB565 pixels from `src` (2 bytes/pixel, native-endian
 * guest already batch-read into host memory) into ARGB8888 `dst`, 8
 * pixels/iteration via SSE2. Tail (< 8 remaining pixels) handled scalar.
 *
 * Deliberately keeps the whole computation in 16-bit lanes (16 pixels'
 * worth of R/G/B fit two per 32-bit slot) and only widens to bytes right
 * before interleaving into the ARGB output, instead of the more "obvious"
 * per-pixel-in-a-32-bit-lane approach (widen to 32 bits immediately, do
 * all the shift/mask math there, 4 pixels/vector). That first version was
 * actually built and benchmarked first -- it was *slower* than the plain
 * scalar loop (~0.82x) on this host: the widen-to-32/narrow-back-down
 * round trip plus only 4 pixels/vector wasn't enough work to amortize the
 * shuffle overhead against an out-of-order CPU already executing the
 * independent-per-pixel scalar loop at good IPC. This 16-bit-lane, 8-wide
 * version (measured ~1.5x) is the one actually shipped -- see this
 * session's doc for both benchmarks side by side. AVX2 was also tried
 * (16 pixels/vector) and rejected: cross-128-bit-lane shuffles needed to
 * reassemble the output made it *inconsistently* faster/slower than
 * scalar across repeated runs, not a reliable win.
 */
static void gnw_h7b0_ltdc_rgb565_row_to_argb8888_sse2(const uint8_t *src,
                                                       uint32_t *dst,
                                                       int cols)
{
    const __m128i mask5 = _mm_set1_epi16(0x1F);
    const __m128i mask6 = _mm_set1_epi16(0x3F);
    const __m128i zero = _mm_setzero_si128();
    const __m128i alpha_bytes = _mm_set1_epi8((char)0xFF);
    int x = 0;

    for (; x + 8 <= cols; x += 8) {
        __m128i px = _mm_loadu_si128((const __m128i *)(src + x * 2));

        __m128i r5 = _mm_and_si128(_mm_srli_epi16(px, 11), mask5);
        __m128i g6 = _mm_and_si128(_mm_srli_epi16(px, 5), mask6);
        __m128i b5 = _mm_and_si128(px, mask5);

        __m128i r8 = _mm_or_si128(_mm_slli_epi16(r5, 3),
                                   _mm_srli_epi16(r5, 2));
        __m128i g8 = _mm_or_si128(_mm_slli_epi16(g6, 2),
                                   _mm_srli_epi16(g6, 4));
        __m128i b8 = _mm_or_si128(_mm_slli_epi16(b5, 3),
                                   _mm_srli_epi16(b5, 2));

        /* Each of r8/g8/b8 holds one valid byte (0-255) per 16-bit lane,
         * high byte zero -- packus_epi16 against an all-zero vector packs
         * the 8 low bytes down contiguously without any saturation
         * actually occurring (values already fit in 0-255). */
        __m128i r8b = _mm_packus_epi16(r8, zero);
        __m128i g8b = _mm_packus_epi16(g8, zero);
        __m128i b8b = _mm_packus_epi16(b8, zero);

        /* Interleave to B,G,R,A byte order (little-endian ARGB8888 in
         * memory is B,G,R,A) via two rounds of unpacklo at byte then
         * 16-bit granularity -- standard planar-to-interleaved pattern. */
        __m128i bg = _mm_unpacklo_epi8(b8b, g8b);
        __m128i ra = _mm_unpacklo_epi8(r8b, alpha_bytes);
        __m128i lo = _mm_unpacklo_epi16(bg, ra);
        __m128i hi = _mm_unpackhi_epi16(bg, ra);

        _mm_storeu_si128((__m128i *)(dst + x), lo);
        _mm_storeu_si128((__m128i *)(dst + x + 4), hi);
    }

    for (; x < cols; x++) {
        dst[x] = gnw_h7b0_ltdc_rgb565_to_pixel32(lduw_le_p(src + x * 2));
    }
}
#endif

#ifndef GNW_H7B0_LTDC_HAVE_SSE2_ROW_CONVERT
/* Scalar fallback/reference, identical math to the SSE2 path above --
 * used on non-x86_64 (or SSE2-less) hosts. Only compiled in when the
 * SSE2 path itself isn't available, to avoid an unused-function error
 * on the (baseline x86_64) builds that always take the SSE2 path;
 * kept as its own standalone function (not inlined into the dispatcher
 * below) since it also serves as the correctness oracle in the
 * standalone host-side test harness used to verify the SSE2 path. */
static void gnw_h7b0_ltdc_rgb565_row_to_argb8888_scalar(const uint8_t *src,
                                                         uint32_t *dst,
                                                         int cols)
{
    for (int x = 0; x < cols; x++) {
        dst[x] = gnw_h7b0_ltdc_rgb565_to_pixel32(lduw_le_p(src + x * 2));
    }
}
#endif

static void gnw_h7b0_ltdc_rgb565_row_to_argb8888(const uint8_t *src,
                                                  uint32_t *dst, int cols)
{
#ifdef GNW_H7B0_LTDC_HAVE_SSE2_ROW_CONVERT
    gnw_h7b0_ltdc_rgb565_row_to_argb8888_sse2(src, dst, cols);
#else
    gnw_h7b0_ltdc_rgb565_row_to_argb8888_scalar(src, dst, cols);
#endif
}

/*
 * True iff BFCR's BF1/BF2 are both the PAxCA-mode encoding used by
 * default-reset firmware config (0x607) -- the mode
 * gnw_h7b0_ltdc_blend_over()'s own fast path requires (see its comment)
 * before the row fast path below can skip calling it at all.
 */
static inline bool gnw_h7b0_ltdc_bfcr_is_pa_pa(uint32_t bfcr)
{
    unsigned int bf1 = (bfcr >> LTDC_LxBFCR_BF1_SHIFT) & LTDC_LxBFCR_BF1_MASK;
    unsigned int bf2 = bfcr & LTDC_LxBFCR_BF2_MASK;

    return (bf1 & LTDC_LxBFCR_MODE_PA) && (bf2 & LTDC_LxBFCR_MODE_PA);
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

/*
 * Cheap (memcpy-only, no per-pixel conversion) synchronous snapshot of
 * everything the compositor worker thread needs, taken on the BQL-
 * holding producer thread. Fills s->compositor_job and hands it to the
 * worker; the expensive per-pixel work happens in
 * gnw_h7b0_ltdc_composite_from_job() below, entirely off this thread.
 * Only called when job_busy is already known false (see
 * gnw_h7b0_ltdc_capture_if_enabled()), so s->compositor_job is not
 * touched concurrently by the worker while this runs. Batches each
 * layer's entire row span into one cpu_physical_memory_read() call, same
 * as the synchronous capture_rows() this replaces used to (see that
 * function's git history for the perf rationale -- one read per capture
 * instead of one per scanline).
 */
static void gnw_h7b0_ltdc_snapshot_and_dispatch(GnwH7B0LtdcState *s, int rows)
{
    GnwH7B0LtdcCaptureJob *job = &s->compositor_job;
    int cols = s->shadow_width;
    uint32_t pfcr = s->active_l1pfcr & LTDC_LxPFCR_PF_MASK;
    bool l1_l8 = (pfcr == LTDC_PF_L8);
    int l1_src_width = cols * (l1_l8 ? 1 : 2);

    bool l2_en = s->active_l2cr & LTDC_LxCR_LEN;
    uint32_t l2_pfcr = s->active_l2pfcr & LTDC_LxPFCR_PF_MASK;
    int l2_src_width = gnw_h7b0_ltdc_layer_stride_bytes(s->active_l2cfblr);
    int l2_bpp = 0;
    /*
     * l2_src_width > 0 is required, not just l2_en: firmware can leave
     * Layer2 enabled (LEN set) with CFBLR still at its reset/zero value
     * mid-transition (e.g. reconfiguring Layer2 for JPEG-decoded cover
     * art), which makes l2_src_width 0. Without this check l2_bpp would
     * still come out > 0 for a recognized pixel format below, but
     * job->l2_raw is allocated as g_malloc(l2_src_width * rows) == 0
     * bytes == NULL (g_malloc(0) is valid and returns NULL), and
     * l2_linebuf derived from it in gnw_h7b0_ltdc_composite_from_job()
     * would stay NULL for every row while still gating only on
     * l2_en && l2_bpp > 0 -- the real cause of a confirmed host segfault
     * in an inlined 2-byte pixel load (lduw_he_p()) once real firmware
     * sequences started actually hitting this transient zero-stride
     * state.
     */
    if (l2_en && l2_src_width > 0) {
        switch (l2_pfcr) {
        case 0: l2_bpp = 4; break;
        case 2: case 3: case 4: l2_bpp = 2; break;
        case 5: l2_bpp = 1; break; /* L8 */
        case 6: l2_bpp = 1; break; /* AL44 */
        default: l2_bpp = 0; break;
        }
    }
    /*
     * Layer2's own column count, derived from its own stride/bpp -- NOT
     * assumed equal to Layer1's `cols` (shadow_width). Real firmware can
     * configure Layer2 with a narrower CFBLR/stride than Layer1's (e.g.
     * a smaller cover-art overlay composited over a full-width
     * background), and the compositor worker's per-pixel loop is driven
     * by Layer1's `cols`. Without this bound, `x` can run past how many
     * whole pixels job->l2_raw actually holds (l2_src_width bytes per
     * row) -- a real, confirmed second host segfault (a 4-byte ARGB8888
     * read via gnw_h7b0_ltdc_read_pixel_buf()) distinct from the
     * l2_src_width==0 NULL-pointer case the l2_bpp assignment's own
     * guard above handles. Zero when l2_bpp is 0 so it never wrongly
     * reads as "in bounds" for an unsupported/disabled format. Stored
     * into the job (see below) since gnw_h7b0_ltdc_composite_from_job()
     * runs on the worker thread and can't recompute it from live `s->`
     * register state.
     */
    int l2_cols = l2_bpp > 0 ? l2_src_width / l2_bpp : 0;

    job->cols = cols;
    job->rows = rows;

    job->active_l1cr = s->active_l1cr;
    job->active_l1pfcr = s->active_l1pfcr;
    job->active_l1cfblr = s->active_l1cfblr;
    job->active_l1ckcr = s->active_l1ckcr;
    job->active_l1dccr = s->active_l1dccr;
    job->active_l1bfcr = s->active_l1bfcr;
    job->active_l1cacr = s->active_l1cacr;
    job->active_l1whpcr = s->active_l1whpcr;
    job->active_l1wvpcr = s->active_l1wvpcr;

    job->l2_en = l2_en;
    job->active_l2cr = s->active_l2cr;
    job->active_l2pfcr = s->active_l2pfcr;
    job->active_l2cfblr = s->active_l2cfblr;
    job->active_l2ckcr = s->active_l2ckcr;
    job->active_l2dccr = s->active_l2dccr;
    job->active_l2bfcr = s->active_l2bfcr;
    job->active_l2cacr = s->active_l2cacr;
    job->active_l2whpcr = s->active_l2whpcr;
    job->active_l2wvpcr = s->active_l2wvpcr;

    job->active_bccr = s->active_bccr;
    job->active_gcr = s->active_gcr;
    job->active_bpcr = s->active_bpcr;

    job->l1_src_width = l1_src_width;
    job->l2_src_width = l2_src_width;
    job->l2_bpp = l2_bpp;
    job->l2_cols = l2_cols;

    size_t l1_needed = (size_t)l1_src_width * rows;
    if (job->l1_raw_size < l1_needed) {
        g_free(job->l1_raw);
        job->l1_raw = g_malloc(l1_needed);
        job->l1_raw_size = l1_needed;
    }
    cpu_physical_memory_read(s->active_l1cfbar, job->l1_raw, l1_needed);

    if (l2_en && l2_bpp > 0) {
        size_t l2_needed = (size_t)l2_src_width * rows;
        if (job->l2_raw_size < l2_needed) {
            g_free(job->l2_raw);
            job->l2_raw = g_malloc(l2_needed);
            job->l2_raw_size = l2_needed;
        }
        cpu_physical_memory_read(s->active_l2cfbar, job->l2_raw, l2_needed);
    }

    memcpy(job->clut, s->clut, sizeof(job->clut));
    memcpy(job->clut2, s->clut2, sizeof(job->clut2));

    qemu_mutex_lock(&s->job_lock);
    s->job_busy = true;
    s->job_pending = true;
    qemu_cond_signal(&s->job_cond);
    qemu_mutex_unlock(&s->job_lock);
}

/*
 * The actual per-pixel convert/blend/dither work (identical logic to
 * this device's previous synchronous gnw_h7b0_ltdc_capture_rows(), just
 * sourcing from a GnwH7B0LtdcCaptureJob snapshot instead of live guest
 * RAM/s->regs/s->clut). Runs on the compositor worker thread, without
 * the BQL held, writing into `out` (the worker's private back buffer --
 * never touched by any other thread while this runs). Preserves both of
 * capture_rows()'s existing perf optimizations: the per-column window-
 * clip precompute (l1_x_in/l2_x_in) and gnw_h7b0_ltdc_blend_over()'s
 * fully-transparent/fully-opaque fast paths.
 */
static void gnw_h7b0_ltdc_composite_from_job(GnwH7B0LtdcState *s,
                                              GnwH7B0LtdcCaptureJob *job,
                                              uint32_t *out)
{
    int cols = job->cols;
    uint32_t pfcr = job->active_l1pfcr & LTDC_LxPFCR_PF_MASK;
    bool l1_l8 = (pfcr == LTDC_PF_L8);
    int src_width = job->l1_src_width;

    bool l1_colken = job->active_l1cr & LTDC_LxCR_COLKEN;
    uint32_t l1_ckcr = job->active_l1ckcr;
    uint32_t l1_dccr = job->active_l1dccr;
    uint32_t l1_bfcr = job->active_l1bfcr;
    uint32_t l1_cacr = job->active_l1cacr & 0xFF;
    int l1_whstpos = job->active_l1whpcr & LTDC_LxWHPCR_WHSTPOS_MASK;
    int l1_whsppos = (job->active_l1whpcr >> LTDC_LxWHPCR_WHSPPOS_SHIFT) &
                      LTDC_LxWHPCR_WHSPPOS_MASK;
    int l1_wvstpos = job->active_l1wvpcr & LTDC_LxWVPCR_WVSTPOS_MASK;
    int l1_wvsppos = (job->active_l1wvpcr >> LTDC_LxWVPCR_WVSPPOS_SHIFT) &
                      LTDC_LxWVPCR_WVSPPOS_MASK;

    bool l2_en = job->l2_en;
    uint32_t l2_pfcr = job->active_l2pfcr & LTDC_LxPFCR_PF_MASK;
    bool l2_l8 = (l2_pfcr == LTDC_PF_L8);
    bool l2_al44 = (l2_pfcr == LTDC_PF_AL44);
    int l2_src_width = job->l2_src_width;
    int l2_bpp = job->l2_bpp;
    /* See gnw_h7b0_ltdc_snapshot_and_dispatch()'s computation of this
     * field -- required to keep the per-pixel loop below from reading
     * past job->l2_raw's per-row allocation when Layer2's stride is
     * narrower than Layer1's `cols`. */
    int l2_cols = job->l2_cols;
    uint32_t l2_cacr = job->active_l2cacr & 0xFF;
    bool l2_colken = job->active_l2cr & LTDC_LxCR_COLKEN;
    uint32_t l2_ckcr = job->active_l2ckcr;
    uint32_t l2_dccr = job->active_l2dccr;
    uint32_t l2_bfcr = job->active_l2bfcr;
    int l2_whstpos = job->active_l2whpcr & LTDC_LxWHPCR_WHSTPOS_MASK;
    int l2_whsppos = (job->active_l2whpcr >> LTDC_LxWHPCR_WHSPPOS_SHIFT) &
                      LTDC_LxWHPCR_WHSPPOS_MASK;
    int l2_wvstpos = job->active_l2wvpcr & LTDC_LxWVPCR_WVSTPOS_MASK;
    int l2_wvsppos = (job->active_l2wvpcr >> LTDC_LxWVPCR_WVSPPOS_SHIFT) &
                      LTDC_LxWVPCR_WVSPPOS_MASK;

    uint32_t bccr = 0xFF000000U | (job->active_bccr & 0xFFFFFFU);
    bool dither_en = job->active_gcr & LTDC_GCR_DEN;

    int ahbp = (job->active_bpcr >> LTDC_BPCR_AHBP_SHIFT) & LTDC_BPCR_AHBP_MASK;
    int avbp = job->active_bpcr & LTDC_BPCR_AVBP_MASK;

    g_autofree bool *l1_x_in = g_malloc(cols * sizeof(bool));
    g_autofree bool *l2_x_in = g_malloc(cols * sizeof(bool));
    for (int x = 0; x < cols; x++) {
        int abs_x = ahbp + x + 1;
        l1_x_in[x] = abs_x >= l1_whstpos && abs_x <= l1_whsppos;
        l2_x_in[x] = abs_x >= l2_whstpos && abs_x <= l2_whsppos;
    }

    /*
     * Row fast-path eligibility that doesn't depend on y: Layer1 must be
     * RGB565 (not L8 -- CLUT gather isn't vectorized here), fully opaque
     * with the default PAxCA blend factors and CACR==255 (so
     * gnw_h7b0_ltdc_blend_over()'s own fast path would just return fg
     * unmodified for every pixel), no color-key (which can force
     * per-pixel transparency), no dithering, Layer2 either disabled or
     * an unsupported/zero-bpp format, and the whole row's x-range inside
     * Layer1's window (checked once here via the endpoints -- l1_x_in[]
     * is monotonic in x since WHSTPOS<=WHSPPOS bounds a single
     * contiguous span).
     */
    bool l1_fast_eligible = !l1_l8 && !l1_colken && l1_cacr == 255 &&
        gnw_h7b0_ltdc_bfcr_is_pa_pa(l1_bfcr) && !(l2_en && l2_bpp > 0) &&
        !dither_en && cols > 0 && l1_x_in[0] && l1_x_in[cols - 1];

    for (int y = 0; y < job->rows; y++) {
        int abs_y = avbp + y + 1;
        bool l1_row_in = abs_y >= l1_wvstpos && abs_y <= l1_wvsppos;
        bool l2_row_in = abs_y >= l2_wvstpos && abs_y <= l2_wvsppos;
        const uint8_t *linebuf = job->l1_raw + (size_t)y * src_width;
        const uint8_t *l2_linebuf = (l2_en && l2_bpp > 0) ?
            job->l2_raw + (size_t)y * l2_src_width : NULL;

        if (l1_fast_eligible && l1_row_in) {
            gnw_h7b0_ltdc_rgb565_row_to_argb8888(
                linebuf, out + (hwaddr)y * cols, cols);
            continue;
        }

        for (int x = 0; x < cols; x++) {
            int abs_x = ahbp + x + 1;
            bool l1_in = l1_row_in && l1_x_in[x];
            uint32_t l1_raw = l1_l8 ? job->clut[linebuf[x]]
                                    : gnw_h7b0_ltdc_rgb565_to_pixel32(
                                          lduw_le_p(linebuf + x * 2));

            unsigned int l1_alpha;
            uint32_t l1_resolved = gnw_h7b0_ltdc_resolve_layer(
                l1_raw, l1_in, l1_dccr, l1_colken, l1_ckcr, &l1_alpha);
            uint32_t below = gnw_h7b0_ltdc_blend_over(l1_resolved, l1_alpha,
                                                       l1_bfcr, l1_cacr, bccr);

            if (l2_en && l2_bpp > 0 && x < l2_cols) {
                bool l2_in = l2_row_in && l2_x_in[x];
                uint32_t l2_raw;
                unsigned int l2_alpha;
                uint32_t l2_resolved;

                if (l2_l8) {
                    uint8_t idx = l2_linebuf[x];
                    l2_raw = job->clut2[idx];
                } else if (l2_al44) {
                    uint8_t raw = l2_linebuf[x];
                    unsigned int a4 = (raw >> 4) & 0xF;
                    unsigned int l4 = raw & 0xF;
                    unsigned int a8 = a4 * 0x11U;
                    uint32_t clut_rgb = job->clut2[l4 * 0x11U] & 0x00FFFFFFU;
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

            out[y * cols + x] = px;
        }
    }
}

static void *gnw_h7b0_ltdc_compositor_thread_fn(void *opaque)
{
    GnwH7B0LtdcState *s = GNW_H7B0_LTDC(opaque);

    qemu_mutex_lock(&s->job_lock);
    while (!s->compositor_stop) {
        while (!s->job_pending && !s->compositor_stop) {
            qemu_cond_wait(&s->job_cond, &s->job_lock);
        }
        if (s->compositor_stop) {
            break;
        }
        s->job_pending = false;
        qemu_mutex_unlock(&s->job_lock);

        /*
         * s->compositor_job and s->compositor_back_buffer are safe to
         * touch here without any lock: the producer side is guaranteed
         * (by job_busy, still true) not to mutate compositor_job again,
         * and compositor_back_buffer is never read/written by anything
         * but this worker thread.
         */
        gnw_h7b0_ltdc_composite_from_job(s, &s->compositor_job,
                                          s->compositor_back_buffer);

        qemu_mutex_lock(&s->compositor_publish_lock);
        {
            uint32_t *tmp = s->shadow_buffer;
            s->shadow_buffer = s->compositor_back_buffer;
            s->compositor_back_buffer = tmp;
        }
        s->content_dirty = true;
        s->invalidate = 1;
        qemu_mutex_unlock(&s->compositor_publish_lock);

        qemu_mutex_lock(&s->job_lock);
        s->job_busy = false;
        /* Wake gnw_h7b0_ltdc_wait_compositor_idle() (reset/unrealize),
         * which waits on this same condvar for job_busy to clear. */
        qemu_cond_broadcast(&s->job_cond);
    }
    qemu_mutex_unlock(&s->job_lock);
    return NULL;
}

/*
 * Block until the compositor worker thread is idle (no job in flight).
 * Must be called before anything that frees/reassigns shadow_buffer,
 * compositor_back_buffer, or compositor_job's raw snapshot buffers
 * outside the normal job_busy-gated resize path in
 * gnw_h7b0_ltdc_capture_setup() -- currently that's device reset and
 * unrealize, both of which can race an in-flight compositor job
 * otherwise (reset in particular runs under the BQL but that says
 * nothing about the worker thread, which runs without it).
 */
static void gnw_h7b0_ltdc_wait_compositor_idle(GnwH7B0LtdcState *s)
{
    qemu_mutex_lock(&s->job_lock);
    while (s->job_busy) {
        qemu_cond_wait(&s->job_cond, &s->job_lock);
    }
    qemu_mutex_unlock(&s->job_lock);
}

static void gnw_h7b0_ltdc_update_display(void *opaque)
{
    GnwH7B0LtdcState *s = GNW_H7B0_LTDC(opaque);
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t *buf;
    int width, height;
    bool dirty;

    /*
     * Snapshot the currently-published buffer pointer/dims/dirty flag
     * under compositor_publish_lock -- the ONLY thing that can change
     * concurrently with this function now that compositing runs on the
     * worker thread: a publish is a pointer swap the worker performs
     * under this same lock (see gnw_h7b0_ltdc_compositor_thread_fn()).
     * Once snapshotted, `buf` is guaranteed stable for the rest of this
     * function: the worker thread only ever writes into whichever
     * buffer is NOT currently published (i.e. never `buf` after this
     * point), until its NEXT publish, which cannot happen before this
     * function releases the lock and returns.
     */
    qemu_mutex_lock(&s->compositor_publish_lock);
    buf = s->shadow_buffer;
    width = s->shadow_width;
    height = s->shadow_height;
    dirty = s->content_dirty;
    qemu_mutex_unlock(&s->compositor_publish_lock);

    if (!buf || width <= 0 || height <= 0) {
        qemu_mutex_lock(&s->compositor_publish_lock);
        s->content_dirty = false;
        qemu_mutex_unlock(&s->compositor_publish_lock);
        return;
    }

    if (surface_bits_per_pixel(surface) != 32) {
        qemu_mutex_lock(&s->compositor_publish_lock);
        s->content_dirty = false;
        qemu_mutex_unlock(&s->compositor_publish_lock);
        return;
    }

    if (!dirty && !s->invalidate) {
        return;
    }

    if (s->invalidate ||
        width * GNW_H7B0_LTDC_SCALE != surface_width(surface) ||
        height * GNW_H7B0_LTDC_SCALE != surface_height(surface)) {
        qemu_console_resize(s->con, width * GNW_H7B0_LTDC_SCALE,
                            height * GNW_H7B0_LTDC_SCALE);
        surface = qemu_console_surface(s->con);
    }

    for (int y = 0; y < height; y++) {
        uint8_t *drow0 = surface_data(surface) +
                          (hwaddr)y * GNW_H7B0_LTDC_SCALE * surface_stride(surface);

        for (int x = 0; x < width; x++) {
            uint32_t px = buf[y * width + x];
            for (int sy = 0; sy < GNW_H7B0_LTDC_SCALE; sy++) {
                uint32_t *drow = (uint32_t *)(drow0 +
                                              (hwaddr)sy * surface_stride(surface));
                for (int sx = 0; sx < GNW_H7B0_LTDC_SCALE; sx++) {
                    drow[x * GNW_H7B0_LTDC_SCALE + sx] = px;
                }
            }
        }
    }

    dpy_gfx_update(s->con, 0, 0, width * GNW_H7B0_LTDC_SCALE,
                   height * GNW_H7B0_LTDC_SCALE);
    s->invalidate = 0;

    /*
     * Only clear content_dirty if the worker hasn't already published a
     * newer frame while we were blitting (buf would then no longer
     * match s->shadow_buffer) -- otherwise we'd silently drop that
     * newer frame's dirty flag and it would never get drawn until the
     * next unrelated invalidate.
     */
    qemu_mutex_lock(&s->compositor_publish_lock);
    if (s->shadow_buffer == buf) {
        s->content_dirty = false;
    }
    qemu_mutex_unlock(&s->compositor_publish_lock);
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
        /*
         * Reset alongside srcr_idle_ticks, not just when the idle-
         * fallback consumes it -- confirmed via live profiling that
         * leaving this "sticky" (as originally designed) meant a single
         * genuine transition anywhere in a session left it permanently
         * armed, so EVERY later ordinary stutter burst (which also
         * crosses the srcr_idle_ticks threshold, see that field's doc
         * comment) re-triggered the fallback for the rest of the
         * session, reintroducing the self-inflicted tlb_reset_dirty/
         * notdirty churn this was meant to avoid. reload_active() below
         * re-arms it if THIS write's reload is itself a genuine
         * structural change.
         */
        s->structural_transition_pending = false;
        if (value & LTDC_SRCR_IMR) {
            /*
             * An IMR reload means firmware is applying a fresh layer/
             * format config -- the natural checkpoint for "a new screen
             * just took over the layers, prove it uses VBR before we
             * disable the no-VBR fallback for it again" (see the
             * vblank_tick() comment for why this isn't a permanent
             * latch or a timeout).
             */
            bool composition_changed = gnw_h7b0_ltdc_reload_is_composition_change(s);
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
             *
             * Skip the (expensive, full-framebuffer) capture entirely when
             * this reload didn't actually change anything composition-
             * affecting (composition_changed, computed above BEFORE
             * reload_active() overwrote the active_* fields) AND there
             * isn't already a stale captured-but-unpublished frame
             * pending (content_dirty) -- see
             * gnw_h7b0_ltdc_reload_is_composition_change()'s doc comment.
             * A real format/geometry change always flips at least one of
             * the compared registers, so this only ever skips truly
             * redundant reloads (e.g. an always-disabled scratch layer's
             * own reload, which can't affect what's on screen).
             *
             * If the compositor worker thread is still busy with a
             * previous frame, this dispatch is dropped (see
             * gnw_h7b0_ltdc_capture_if_enabled()'s doc comment) -- fall
             * back to the same vbr_deferred_capture retry-on-next-
             * vblank-tick mechanism the VBR path below uses, since an
             * IMR-triggered geometry/format change is exactly the kind
             * of update that must not silently get lost.
             */
            if ((composition_changed || s->content_dirty) &&
                !gnw_h7b0_ltdc_capture_if_enabled(s)) {
                s->vbr_deferred_capture = true;
            }
        }
        if (value & LTDC_SRCR_VBR) {
            if (getenv("GNW_AUDIO_TRACE")) {
                fprintf(stderr, "VBR %" PRId64 "\n",
                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
            }
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
             * previous capture. Also retry on the next vblank tick if the
             * dispatch itself was dropped because the compositor worker
             * thread was still busy with a previous frame (see
             * gnw_h7b0_ltdc_capture_if_enabled()'s doc comment) -- not just
             * when content_dirty was already true. */
            if (!s->content_dirty && gnw_h7b0_ltdc_capture_if_enabled(s)) {
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

    qemu_mutex_init(&s->compositor_publish_lock);
    qemu_mutex_init(&s->job_lock);
    qemu_cond_init(&s->job_cond);
    s->compositor_stop = false;
    qemu_thread_create(&s->compositor_thread, "gnw-h7b0-ltdc-compositor",
                        gnw_h7b0_ltdc_compositor_thread_fn, s,
                        QEMU_THREAD_JOINABLE);
    s->compositor_running = true;
}

static void gnw_h7b0_ltdc_unrealize(DeviceState *dev)
{
    GnwH7B0LtdcState *s = GNW_H7B0_LTDC(dev);

    /*
     * Stop the compositor worker thread and join it before tearing down
     * anything it might still touch. Signalled the same way a real job
     * is (job_lock held, job_cond signalled) so the worker's existing
     * wait loop picks up compositor_stop without needing a second wakeup
     * path.
     */
    qemu_mutex_lock(&s->job_lock);
    s->compositor_stop = true;
    qemu_cond_broadcast(&s->job_cond);
    qemu_mutex_unlock(&s->job_lock);
    qemu_thread_join(&s->compositor_thread);
    s->compositor_running = false;

    qemu_mutex_destroy(&s->job_lock);
    qemu_cond_destroy(&s->job_cond);
    qemu_mutex_destroy(&s->compositor_publish_lock);

    g_free(s->shadow_buffer);
    s->shadow_buffer = NULL;
    g_free(s->compositor_back_buffer);
    s->compositor_back_buffer = NULL;
    g_free(s->compositor_job.l1_raw);
    s->compositor_job.l1_raw = NULL;
    g_free(s->compositor_job.l2_raw);
    s->compositor_job.l2_raw = NULL;

    gnw_h7b0_ltdc_fb_track_range(&s->fb_l1_section, &s->fb_l1_track_base,
                                  &s->fb_l1_track_len, 0, 0);
    gnw_h7b0_ltdc_fb_track_range(&s->fb_l2_section, &s->fb_l2_track_base,
                                  &s->fb_l2_track_len, 0, 0);
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
    dc->unrealize = gnw_h7b0_ltdc_unrealize;
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
