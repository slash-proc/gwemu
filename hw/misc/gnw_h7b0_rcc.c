/*
 * STM32H7B0 RCC minimal stub (Nintendo Game & Watch)
 *
 * See gnw_h7b0_rcc.h for scope/rationale. Deliberately not the existing
 * hw/misc/stm32_rcc.c device: that model is F4-family register layout
 * (different offsets) and, more importantly, doesn't mirror *ON bits
 * into *RDY bits at all -- reused as-is, real H7B0 firmware's clock-init
 * polling loops would hang forever waiting for a status bit nothing ever
 * sets.
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
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/core/clock.h"
#include "hw/core/resettable.h"
#include "hw/misc/gnw_h7b0_rcc.h"
#include "hw/misc/gnw_h7b0_clock_dividers.h"
#include "hw/misc/gnw_h7b0_regs_rcc.h"
#include "hw/misc/gnw_h7b0_timer_clock.h"
#include "hw/core/qdev.h"

GnwH7B0RccEngineClockInfo gnw_h7b0_rcc_engine_clock_info(
    GnwH7B0RccState *s, GnwH7B0RccEngine engine)
{
    uint32_t enable_bit;
    uint32_t sws = (s->regs[GNW_H7B0_RCC_CFGR >> 2] &
                    RCC_CFGR_SWS_MASK) >> RCC_CFGR_SWS_SHIFT;
    bool known = sws <= RCC_CFGR_SWS_PLL1;
    uint32_t hz = known ? gnw_h7b0_rcc_get_hclk_hz(s) : 0;

    if (sws == RCC_CFGR_SWS_PLL1) {
        uint32_t cr = s->regs[GNW_H7B0_RCC_CR >> 2];
        /* RM0455 RCC_PLLCFGR.DIVP1EN, bit16. */
        if ((cr & (RCC_CR_PLL1ON | RCC_CR_PLL1RDY)) !=
            (RCC_CR_PLL1ON | RCC_CR_PLL1RDY) ||
            !(s->regs[GNW_H7B0_RCC_PLLCFGR_OFFSET >> 2] & (1U << 16))) {
            hz = 0;
        }
    }

    g_assert(engine >= 0 && engine < GNW_H7B0_RCC_ENGINE_COUNT);
    /* RM0455 RCC_AHB3ENR: MDMAEN bit0, DMA2DEN bit4. */
    enable_bit = engine == GNW_H7B0_RCC_ENGINE_MDMA ? 1U : 1U << 4;
    return (GnwH7B0RccEngineClockInfo) {
        .known = known,
        .enabled = !!(s->regs[GNW_H7B0_RCC_AHB3ENR_OFFSET >> 2] & enable_bit),
        .reference_hz = hz,
    };
}

void gnw_h7b0_rcc_add_engine_clock_observer(
    GnwH7B0RccState *s, GnwH7B0RccEngine engine,
    GnwH7B0RccEngineClockChangeFn callback, void *opaque)
{
    g_assert(engine >= 0 && engine < GNW_H7B0_RCC_ENGINE_COUNT && callback);
    for (int i = 0; i < s->n_engine_clock_observers; i++) {
        GnwH7B0RccEngineClockObserver *o = &s->engine_clock_observers[i];
        if (o->engine == engine && o->callback == callback && o->opaque == opaque) {
            return;
        }
    }
    g_assert(s->n_engine_clock_observers < GNW_H7B0_RCC_ENGINE_CLOCK_OBSERVER_MAX);
    s->engine_clock_observers[s->n_engine_clock_observers++] =
        (GnwH7B0RccEngineClockObserver) { engine, callback, opaque };
}

static void gnw_h7b0_rcc_notify_engine_clocks(
    GnwH7B0RccState *s,
    const GnwH7B0RccEngineClockInfo old[GNW_H7B0_RCC_ENGINE_COUNT])
{
    for (int i = 0; i < s->n_engine_clock_observers; i++) {
        GnwH7B0RccEngineClockObserver *o = &s->engine_clock_observers[i];
        GnwH7B0RccEngineClockInfo next =
            gnw_h7b0_rcc_engine_clock_info(s, o->engine);
        GnwH7B0RccEngineClockInfo prior = old[o->engine];
        if (prior.known != next.known || prior.enabled != next.enabled ||
            prior.reference_hz != next.reference_hz) {
            o->callback(o->opaque, prior, next);
        }
    }
}

#include "hw/misc/gnw_h7b0_dma.h"

static void gnw_h7b0_rcc_update_sysclk_clock(GnwH7B0RccState *s);

typedef struct GnwH7B0RccAdcClockInputs {
    uint32_t async_hz;
    uint32_t sync_hz;
} GnwH7B0RccAdcClockInputs;

static GnwH7B0RccAdcClockInputs gnw_h7b0_rcc_adc_clock_inputs(
    GnwH7B0RccState *s);
static void gnw_h7b0_rcc_notify_adc_clock_change(
    GnwH7B0RccState *s, GnwH7B0RccAdcClockInputs old_inputs,
    GnwH7B0RccAdcClockInputs new_inputs);
static void gnw_h7b0_rcc_notify_octospi_clock_change(
    GnwH7B0RccState *s, uint32_t old_hz, uint32_t new_hz);
static void gnw_h7b0_rcc_notify_sai1_clock_change(
    GnwH7B0RccState *s, GnwH7B0RccSai1ClockInfo old_info,
    GnwH7B0RccSai1ClockInfo new_info);
static uint32_t gnw_h7b0_rcc_get_pll1q_hz(GnwH7B0RccState *s);
static uint32_t gnw_h7b0_rcc_get_pll3p_hz(GnwH7B0RccState *s);

static GnwH7B0RccSai1ClockInfo gnw_h7b0_rcc_sai1_clock_info_snapshot(
    GnwH7B0RccState *s)
{
    GnwH7B0RccSai1ClockInfo info = {
        .enabled = gnw_h7b0_rcc_sai1_clock_enabled(s),
    };

    info.known = gnw_h7b0_rcc_get_sai1_clock_info(s, &info.kernel_hz);
    return info;
}

static uint32_t gnw_h7b0_rcc_timer_clock_hz(
    GnwH7B0RccState *s, GnwH7B0RccTimerClockDomain domain)
{
    return domain == GNW_H7B0_RCC_TIMER_APB1 ?
           gnw_h7b0_rcc_get_timer_ker_hz(s) :
           gnw_h7b0_rcc_get_timer_apb2_ker_hz(s);
}

static void gnw_h7b0_rcc_notify_timer_clock_change(
    GnwH7B0RccState *s, GnwH7B0RccTimerClockDomain domain,
    uint32_t old_hz, uint32_t new_hz)
{
    if (old_hz == new_hz) {
        return;
    }

    for (int i = 0; i < s->n_timer_clock_observers; i++) {
        GnwH7B0RccTimerClockObserver *observer =
            &s->timer_clock_observers[i];

        if (observer->domain == domain) {
            observer->callback(observer->opaque, old_hz, new_hz);
        }
    }
}

static uint32_t gnw_h7b0_rcc_timing_domain_hz(
    GnwH7B0RccState *s, GnwH7B0RccTimingDomain domain)
{
    switch (domain) {
    case GNW_H7B0_RCC_TIMING_CORE:
        return gnw_h7b0_rcc_get_core_hz(s);
    case GNW_H7B0_RCC_TIMING_APB1:
        return gnw_h7b0_rcc_timer_clock_hz(s, GNW_H7B0_RCC_TIMER_APB1);
    case GNW_H7B0_RCC_TIMING_APB2:
        return gnw_h7b0_rcc_timer_clock_hz(s, GNW_H7B0_RCC_TIMER_APB2);
    default:
        g_assert_not_reached();
    }
}

static bool gnw_h7b0_rcc_write_affects_timing(hwaddr addr)
{
    switch (addr) {
    case GNW_H7B0_RCC_CR:
    case GNW_H7B0_RCC_CFGR:
    case GNW_H7B0_RCC_CDCFGR1_OFFSET:
    case GNW_H7B0_RCC_CDCFGR2_OFFSET:
    case GNW_H7B0_RCC_PLLCKSELR_OFFSET:
    case GNW_H7B0_RCC_PLLCFGR_OFFSET:
    case GNW_H7B0_RCC_PLL1DIVR_OFFSET:
    case GNW_H7B0_RCC_PLL1FRACR_OFFSET:
        return true;
    default:
        return false;
    }
}

static bool gnw_h7b0_rcc_post_load(void *opaque, int version_id,
                                  Error **errp)
{
    GnwH7B0RccState *s = GNW_H7B0_RCC(opaque);
    uint32_t cdcfgr1 = s->regs[GNW_H7B0_RCC_CDCFGR1_OFFSET >> 2];

    if (version_id < 3) {
        /* Older streams modeled FRACR itself as the active value. */
        s->pll1_fracn_latched =
            (s->regs[GNW_H7B0_RCC_PLL1FRACR_OFFSET >> 2] &
             RCC_PLL1FRACR_FRACN1_MASK) >> RCC_PLL1FRACR_FRACN1_SHIFT;
        s->pll2_fracn_latched =
            (s->regs[GNW_H7B0_RCC_PLL2FRACR_OFFSET >> 2] &
             RCC_PLL2FRACR_FRACN2_MASK) >> RCC_PLL2FRACR_FRACN2_SHIFT;
        s->pll3_fracn_latched =
            (s->regs[GNW_H7B0_RCC_PLL3FRACR_OFFSET >> 2] &
             RCC_PLL3FRACR_FRACN3_MASK) >> RCC_PLL3FRACR_FRACN3_SHIFT;
    }
    if (s->pll1_fracn_latched > 0x1fff ||
        s->pll2_fracn_latched > 0x1fff ||
        s->pll3_fracn_latched > 0x1fff) {
        error_setg(errp, "RCC migration contains an invalid latched FRACN");
        return false;
    }

    /* Version 1 saved the divider registers but did not propagate CDCPRE
     * into the CPU clock.  Accept its /1 default state; reject non-/1 states
     * rather than silently resuming with a rate the source VM never used. */
    if (version_id < 2 &&
        (gnw_h7b0_rcc_prescaler_shift((cdcfgr1 >> 8) & 0xf) != 0 ||
         gnw_h7b0_rcc_prescaler_shift(cdcfgr1 & 0xf) != 0)) {
        error_setg(errp,
                   "RCC v1 migration with non-/1 CDCPRE or HPRE cannot "
                   "preserve core-clock semantics");
        return false;
    }

    /* Clock periods themselves are not serialized here.  The clock framework
     * restores dependent clock state without callbacks during migration, so
     * restore this parent period directly from the loaded RCC register image. */
    if (s->sysclk) {
        clock_set_hz(s->sysclk, gnw_h7b0_rcc_get_core_hz(s));
    }
    if (s->systick_refclk) {
        clock_set_hz(s->systick_refclk,
                     gnw_h7b0_rcc_get_hclk_hz(s) / 8);
    }
    return true;
}

static void gnw_h7b0_rcc_reset_hold(Object *obj, ResetType type)
{
    GnwH7B0RccState *s = GNW_H7B0_RCC(obj);
    uint32_t old_core_hz = gnw_h7b0_rcc_get_core_hz(s);
    IcountCycleStamp before = { 0 }, after = { 0 }, actual_before;
    bool timing_active = type != RESET_TYPE_SNAPSHOT_LOAD &&
                         s->n_timing_observers && icount_enabled() &&
                         old_core_hz;
    GnwH7B0RccAdcClockInputs old_adc_inputs =
        gnw_h7b0_rcc_adc_clock_inputs(s);
    uint32_t old_apb1_hz = gnw_h7b0_rcc_timer_clock_hz(
        s, GNW_H7B0_RCC_TIMER_APB1);
    uint32_t old_apb2_hz = gnw_h7b0_rcc_timer_clock_hz(
        s, GNW_H7B0_RCC_TIMER_APB2);
    uint32_t old_octospi_hz = gnw_h7b0_rcc_get_octospi_kernel_hz(s);
    GnwH7B0RccSai1ClockInfo old_sai1_info =
        gnw_h7b0_rcc_sai1_clock_info_snapshot(s);
    GnwH7B0RccEngineClockInfo old_engines[GNW_H7B0_RCC_ENGINE_COUNT];
    for (int i = 0; i < GNW_H7B0_RCC_ENGINE_COUNT; i++) {
        old_engines[i] = gnw_h7b0_rcc_engine_clock_info(s, i);
    }

    if (timing_active) {
        icount_get_cycle_stamp(&before);
        g_assert(before.core_hz == old_core_hz);
        for (int i = 0; i < s->n_timing_observers; i++) {
            GnwH7B0RccTimingObserver *observer = &s->timing_observers[i];

            g_assert(observer->prepare(
                observer->opaque, &before,
                gnw_h7b0_rcc_timing_domain_hz(s, observer->domain)));
        }
    }

    for (int i = 0; i < (GNW_H7B0_RCC_SIZE / 4); i++) {
        s->regs[i] = get_rcc_reset_value(i * 4);
    }
    s->pll1_fracn_latched =
        (s->regs[GNW_H7B0_RCC_PLL1FRACR_OFFSET >> 2] &
         RCC_PLL1FRACR_FRACN1_MASK) >> RCC_PLL1FRACR_FRACN1_SHIFT;
    s->pll2_fracn_latched =
        (s->regs[GNW_H7B0_RCC_PLL2FRACR_OFFSET >> 2] &
         RCC_PLL2FRACR_FRACN2_MASK) >> RCC_PLL2FRACR_FRACN2_SHIFT;
    s->pll3_fracn_latched =
        (s->regs[GNW_H7B0_RCC_PLL3FRACR_OFFSET >> 2] &
         RCC_PLL3FRACR_FRACN3_MASK) >> RCC_PLL3FRACR_FRACN3_SHIFT;
    
    /* Override RSR reset per the rationale in gnw_h7b0_rcc.h */
    s->regs[GNW_H7B0_RCC_RSR >> 2] = RCC_RSR_RESET_VALUE;

    /*
     * Pre-populate BDCR so stock firmware thinks LSE is running and RTC is
     * selected. Otherwise it resets the backup domain, wiping the RTC!
     */
    s->regs[GNW_H7B0_RCC_BDCR >> 2] = RCC_BDCR_LSEON | RCC_BDCR_LSERDY |
                                      (1 << 8) | /* RTCSEL = 01 (LSE) */
                                      (1 << 15); /* RTCEN */

    /* Snapshot-load reset runs inside migration. Do not propagate here:
     * input clocks are restored from VMState, and the post-load hook below
     * restores this parent clock without invoking callbacks. */
    if (type != RESET_TYPE_SNAPSHOT_LOAD) {
        if (timing_active) {
            uint32_t new_core_hz = gnw_h7b0_rcc_get_core_hz(s);
            bool rates_changed = old_core_hz != new_core_hz ||
                old_apb1_hz != gnw_h7b0_rcc_timer_clock_hz(
                    s, GNW_H7B0_RCC_TIMER_APB1) ||
                old_apb2_hz != gnw_h7b0_rcc_timer_clock_hz(
                    s, GNW_H7B0_RCC_TIMER_APB2);

            g_assert(new_core_hz);
            if (rates_changed) {
                icount_cycle_clock_boundary(new_core_hz,
                    old_core_hz == new_core_hz, &actual_before, &after);
                g_assert(actual_before.modeled_cycles ==
                             before.modeled_cycles &&
                         actual_before.idle_ns == before.idle_ns &&
                         actual_before.generation == before.generation);
            } else {
                after = before;
            }
            gnw_h7b0_rcc_update_sysclk_clock(s);
            for (int i = 0; i < s->n_timing_observers; i++) {
                GnwH7B0RccTimingObserver *observer =
                    &s->timing_observers[i];

                observer->commit(observer->opaque, &before, &after,
                    gnw_h7b0_rcc_timing_domain_hz(s, observer->domain));
            }
        } else {
            gnw_h7b0_rcc_update_sysclk_clock(s);
            gnw_h7b0_rcc_notify_timer_clock_change(
                s, GNW_H7B0_RCC_TIMER_APB1, old_apb1_hz,
                gnw_h7b0_rcc_timer_clock_hz(s, GNW_H7B0_RCC_TIMER_APB1));
            gnw_h7b0_rcc_notify_timer_clock_change(
                s, GNW_H7B0_RCC_TIMER_APB2, old_apb2_hz,
                gnw_h7b0_rcc_timer_clock_hz(s, GNW_H7B0_RCC_TIMER_APB2));
        }
        gnw_h7b0_rcc_notify_adc_clock_change(
            s, old_adc_inputs, gnw_h7b0_rcc_adc_clock_inputs(s));
        gnw_h7b0_rcc_notify_octospi_clock_change(
            s, old_octospi_hz, gnw_h7b0_rcc_get_octospi_kernel_hz(s));
        gnw_h7b0_rcc_notify_sai1_clock_change(
            s, old_sai1_info, gnw_h7b0_rcc_sai1_clock_info_snapshot(s));
        gnw_h7b0_rcc_notify_engine_clocks(s, old_engines);
    }
}

static uint64_t gnw_h7b0_rcc_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0RccState *s = GNW_H7B0_RCC(opaque);

    if (addr >= GNW_H7B0_RCC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }
    return s->regs[addr >> 2];
}

static bool gnw_h7b0_rcc_sysclk_source_ready(GnwH7B0RccState *s,
                                              uint32_t source)
{
    uint32_t cr = s->regs[GNW_H7B0_RCC_CR >> 2];

    switch (source) {
    case RCC_CFGR_SWS_HSI:
        return (cr & (RCC_CR_HSION | RCC_CR_HSIRDY)) ==
               (RCC_CR_HSION | RCC_CR_HSIRDY);
    case RCC_CFGR_SWS_CSI:
        return (cr & (RCC_CR_CSION | RCC_CR_CSIRDY)) ==
               (RCC_CR_CSION | RCC_CR_CSIRDY);
    case RCC_CFGR_SWS_HSE:
        return (cr & (RCC_CR_HSEON | RCC_CR_HSERDY)) ==
               (RCC_CR_HSEON | RCC_CR_HSERDY);
    case RCC_CFGR_SWS_PLL1:
        return (cr & (RCC_CR_PLL1ON | RCC_CR_PLL1RDY)) ==
               (RCC_CR_PLL1ON | RCC_CR_PLL1RDY) &&
               (s->regs[GNW_H7B0_RCC_PLLCFGR_OFFSET >> 2] & (1U << 16));
    default:
        return false;
    }
}

static void gnw_h7b0_rcc_complete_pending_sysclk_switch(GnwH7B0RccState *s)
{
    uint32_t *cfgr = &s->regs[GNW_H7B0_RCC_CFGR >> 2];
    uint32_t requested = (*cfgr & RCC_CFGR_SW_MASK) >> RCC_CFGR_SW_SHIFT;

    if (gnw_h7b0_rcc_sysclk_source_ready(s, requested)) {
        *cfgr = (*cfgr & ~RCC_CFGR_SWS_MASK) |
                (requested << RCC_CFGR_SWS_SHIFT);
    }
}

void gnw_h7b0_rcc_add_reset_target(GnwH7B0RccState *s, hwaddr rstr_offset,
                                    unsigned bit, DeviceState *dev)
{
    if (s->n_reset_targets >= GNW_H7B0_RCC_RESET_TARGET_MAX) {
        qemu_log_mask(LOG_UNIMP, "%s: reset-target table full\n", __func__);
        return;
    }
    s->reset_targets[s->n_reset_targets].rstr_offset = rstr_offset;
    s->reset_targets[s->n_reset_targets].bit = bit;
    s->reset_targets[s->n_reset_targets].dev = dev;
    s->n_reset_targets++;
}

void gnw_h7b0_rcc_set_backup_domain_reset(
    GnwH7B0RccState *s, GnwH7B0RccBackupDomainResetFn callback,
    void *opaque)
{
    s->backup_domain_reset = callback;
    s->backup_domain_reset_opaque = opaque;
}

void gnw_h7b0_rcc_add_timer_clock_observer(
    GnwH7B0RccState *s, GnwH7B0RccTimerClockDomain domain,
    GnwH7B0RccTimerClockChangeFn callback, void *opaque)
{
    if (!callback || (domain != GNW_H7B0_RCC_TIMER_APB1 &&
                      domain != GNW_H7B0_RCC_TIMER_APB2)) {
        return;
    }
    for (int i = 0; i < s->n_timer_clock_observers; i++) {
        GnwH7B0RccTimerClockObserver *observer =
            &s->timer_clock_observers[i];

        if (observer->domain == domain && observer->callback == callback &&
            observer->opaque == opaque) {
            return;
        }
    }
    if (s->n_timer_clock_observers >=
        GNW_H7B0_RCC_TIMER_CLOCK_OBSERVER_MAX) {
        qemu_log_mask(LOG_UNIMP, "%s: timer-clock observer table full\n",
                      __func__);
        return;
    }

    GnwH7B0RccTimerClockObserver *observer =
        &s->timer_clock_observers[s->n_timer_clock_observers++];
    observer->domain = domain;
    observer->callback = callback;
    observer->opaque = opaque;
}

void gnw_h7b0_rcc_add_timing_observer(
    GnwH7B0RccState *s, GnwH7B0RccTimingDomain domain,
    GnwH7B0RccTimingPrepareFn prepare,
    GnwH7B0RccTimingCommitFn commit, GnwH7B0RccTimingAbortFn abort,
    void *opaque)
{
    g_assert(domain >= GNW_H7B0_RCC_TIMING_CORE &&
             domain <= GNW_H7B0_RCC_TIMING_APB2);
    g_assert(prepare && commit && abort && opaque);
    for (int i = 0; i < s->n_timing_observers; i++) {
        GnwH7B0RccTimingObserver *observer = &s->timing_observers[i];

        if (observer->domain == domain && observer->prepare == prepare &&
            observer->commit == commit && observer->opaque == opaque) {
            return;
        }
    }
    if (s->n_timing_observers >= GNW_H7B0_RCC_TIMING_OBSERVER_MAX) {
        qemu_log_mask(LOG_UNIMP, "%s: timing observer table full\n",
                      __func__);
        return;
    }

    s->timing_observers[s->n_timing_observers++] =
        (GnwH7B0RccTimingObserver) {
            .domain = domain,
            .prepare = prepare,
            .commit = commit,
            .abort = abort,
            .opaque = opaque,
        };
}

void gnw_h7b0_rcc_add_adc_clock_observer(
    GnwH7B0RccState *s, GnwH7B0RccAdcClockChangeFn callback, void *opaque)
{
    if (!callback) {
        return;
    }
    for (int i = 0; i < s->n_adc_clock_observers; i++) {
        GnwH7B0RccAdcClockObserver *observer = &s->adc_clock_observers[i];

        if (observer->callback == callback && observer->opaque == opaque) {
            return;
        }
    }
    if (s->n_adc_clock_observers >= GNW_H7B0_RCC_ADC_CLOCK_OBSERVER_MAX) {
        qemu_log_mask(LOG_UNIMP, "%s: ADC-clock observer table full\n",
                      __func__);
        return;
    }

    GnwH7B0RccAdcClockObserver *observer =
        &s->adc_clock_observers[s->n_adc_clock_observers++];
    observer->callback = callback;
    observer->opaque = opaque;
}

void gnw_h7b0_rcc_add_octospi_clock_observer(
    GnwH7B0RccState *s, GnwH7B0RccOctospiClockChangeFn callback,
    void *opaque)
{
    if (!callback) {
        return;
    }
    for (int i = 0; i < s->n_octospi_clock_observers; i++) {
        GnwH7B0RccOctospiClockObserver *observer =
            &s->octospi_clock_observers[i];

        if (observer->callback == callback && observer->opaque == opaque) {
            return;
        }
    }
    if (s->n_octospi_clock_observers >=
        GNW_H7B0_RCC_OCTOSPI_CLOCK_OBSERVER_MAX) {
        qemu_log_mask(LOG_UNIMP, "%s: OCTOSPI-clock observer table full\n",
                      __func__);
        return;
    }

    GnwH7B0RccOctospiClockObserver *observer =
        &s->octospi_clock_observers[s->n_octospi_clock_observers++];
    observer->callback = callback;
    observer->opaque = opaque;
}

void gnw_h7b0_rcc_add_sai1_clock_observer(
    GnwH7B0RccState *s, GnwH7B0RccSai1ClockChangeFn callback,
    void *opaque)
{
    if (!callback) {
        return;
    }
    for (int i = 0; i < s->n_sai1_clock_observers; i++) {
        GnwH7B0RccSai1ClockObserver *observer = &s->sai1_clock_observers[i];

        if (observer->callback == callback && observer->opaque == opaque) {
            return;
        }
    }
    if (s->n_sai1_clock_observers >= GNW_H7B0_RCC_SAI1_CLOCK_OBSERVER_MAX) {
        qemu_log_mask(LOG_UNIMP, "%s: SAI1-clock observer table full\n",
                      __func__);
        return;
    }

    GnwH7B0RccSai1ClockObserver *observer =
        &s->sai1_clock_observers[s->n_sai1_clock_observers++];
    observer->callback = callback;
    observer->opaque = opaque;
}

static void gnw_h7b0_rcc_notify_adc_clock_change(
    GnwH7B0RccState *s, GnwH7B0RccAdcClockInputs old_inputs,
    GnwH7B0RccAdcClockInputs new_inputs)
{
    if (old_inputs.async_hz == new_inputs.async_hz &&
        old_inputs.sync_hz == new_inputs.sync_hz) {
        return;
    }

    for (int i = 0; i < s->n_adc_clock_observers; i++) {
        GnwH7B0RccAdcClockObserver *observer = &s->adc_clock_observers[i];

        observer->callback(observer->opaque);
    }
}

static void gnw_h7b0_rcc_notify_octospi_clock_change(
    GnwH7B0RccState *s, uint32_t old_hz, uint32_t new_hz)
{
    if (old_hz == new_hz) {
        return;
    }

    for (int i = 0; i < s->n_octospi_clock_observers; i++) {
        GnwH7B0RccOctospiClockObserver *observer =
            &s->octospi_clock_observers[i];

        observer->callback(observer->opaque, old_hz, new_hz);
    }
}

static void gnw_h7b0_rcc_notify_sai1_clock_change(
    GnwH7B0RccState *s, GnwH7B0RccSai1ClockInfo old_info,
    GnwH7B0RccSai1ClockInfo new_info)
{
    if (old_info.known == new_info.known &&
        old_info.enabled == new_info.enabled &&
        old_info.kernel_hz == new_info.kernel_hz) {
        return;
    }

    for (int i = 0; i < s->n_sai1_clock_observers; i++) {
        GnwH7B0RccSai1ClockObserver *observer = &s->sai1_clock_observers[i];

        observer->callback(observer->opaque);
    }
}

/*
 * Apply peripheral resets for any xxxRSTR bit going 0->1.
 *
 * Real silicon holds the peripheral in reset while the bit is set and
 * releases it when cleared; every user of this (HAL's
 * __HAL_RCC_xxx_FORCE_RESET/RELEASE_RESET pairs, including HAL_DeInit())
 * sets and immediately clears it, so asserting the whole reset on the
 * 0->1 edge is indistinguishable from the outside and avoids having to
 * model a held-in-reset state.
 *
 * Note DMA1 and DMA2 share one device object here, so either of their
 * reset bits resets both controllers. The only caller that matters,
 * HAL_DeInit(), sets every bit on the bus at once anyway.
 *
 * Only the buses whose peripherals we actually model a reset for are
 * routed here (AHB1 -> DMA1/DMA2/ADC, AHB3 -> OCTOSPI1/2,
 * APB2 -> SAI1). HAL_DeInit() also force-resets AHB2/4 and
 * APB1L/1H/3/4 (see
 * sdk/stm32h7xx-hal-driver/Src/stm32h7xx_hal.c); those stay inert
 * register shadows until something is shown to need them.
 */
static void gnw_h7b0_rcc_apply_periph_reset(GnwH7B0RccState *s, hwaddr addr,
                                             uint32_t old, uint32_t value)
{
    uint32_t rising = ~old & value;

    /*
     * Under GNW_DMA_TRACE=2 this is the reliable marker that firmware
     * really reached HAL_DeInit -- i.e. that a headless quit-to-menu
     * timeline actually got as far as quitting, rather than the guest
     * having been too slow to react to the scripted button presses.
     * Every quit-to-menu statistic this fix was measured against filters
     * on it; keep it.
     */
    if (rising) {
        gnw_dma_trace_event("RSTR", "0x%02x rising=0x%08x", (unsigned)addr,
                            rising);
    }

    for (int i = 0; i < s->n_reset_targets; i++) {
        GnwH7B0RccResetTarget *t = &s->reset_targets[i];

        if (t->rstr_offset == addr && (rising & (1U << t->bit))) {
            gnw_dma_trace_event("PRST", "rstr 0x%02x bit %u -> reset %s",
                                (unsigned)addr, t->bit,
                                object_get_typename(OBJECT(t->dev)));
            device_cold_reset(t->dev);
        }
    }
}

static void gnw_h7b0_rcc_write_register(void *opaque, hwaddr addr,
                                         uint64_t val64, unsigned int size,
                                         bool defer_clock_update)
{
    GnwH7B0RccState *s = GNW_H7B0_RCC(opaque);
    uint32_t value = val64;

    if (addr >= GNW_H7B0_RCC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    uint32_t mask = get_rcc_write_mask(addr);
    uint32_t old_value = s->regs[addr >> 2];
    value = (old_value & ~mask) | (value & mask);

    /* RM0455 makes each PLL's integer dividers, VCO/range configuration, and
     * output enables writable only while that PLL's ON and RDY bits are both
     * clear; the common PLLSRC is writable only while all PLLs are disabled.
     * FRACEN is deliberately excluded: live fractional retuning clears it,
     * writes FRACR, then sets it to latch the new value.  In particular,
     * accepting DIVM1=0 while PLL1 supplies SYSCLK would collapse the modeled
     * CPU clock to zero while the icount cycle clock remained nonzero.
     * Preserve fields independently in shared registers so an unlocked PLL
     * remains configurable while another PLL is running. */
    uint32_t cr = s->regs[GNW_H7B0_RCC_CR >> 2];
    bool pll1_locked = cr & (RCC_CR_PLL1ON | RCC_CR_PLL1RDY);
    bool pll2_locked = cr & (RCC_CR_PLL2ON | RCC_CR_PLL2RDY);
    bool pll3_locked = cr & (RCC_CR_PLL3ON | RCC_CR_PLL3RDY);

    if (addr == GNW_H7B0_RCC_PLLCKSELR_OFFSET) {
        uint32_t locked_mask = 0;

        if (pll1_locked || pll2_locked || pll3_locked) {
            locked_mask |= RCC_PLLCKSELR_PLLSRC_MASK;
        }
        if (pll1_locked) {
            locked_mask |= RCC_PLLCKSELR_DIVM1_MASK;
        }
        if (pll2_locked) {
            locked_mask |= RCC_PLLCKSELR_DIVM2_MASK;
        }
        if (pll3_locked) {
            locked_mask |= RCC_PLLCKSELR_DIVM3_MASK;
        }
        value = (value & ~locked_mask) | (old_value & locked_mask);
    } else if (addr == GNW_H7B0_RCC_PLLCFGR_OFFSET) {
        uint32_t locked_mask = 0;

        if (pll1_locked) {
            locked_mask |= 0xeU | (0x7U << 16);
        }
        if (pll2_locked) {
            locked_mask |= (0xeU << 4) | (0x7U << 19);
        }
        if (pll3_locked) {
            locked_mask |= (0xeU << 8) | (0x7U << 22);
        }
        value = (value & ~locked_mask) | (old_value & locked_mask);
    } else if (addr == GNW_H7B0_RCC_PLL1DIVR_OFFSET && pll1_locked) {
        value = old_value;
    } else if (addr == GNW_H7B0_RCC_PLL2DIVR_OFFSET && pll2_locked) {
        value = old_value;
    } else if (addr == GNW_H7B0_RCC_PLL3DIVR_OFFSET && pll3_locked) {
        value = old_value;
    }

    if (addr == GNW_H7B0_RCC_AHB3RSTR_OFFSET ||
        addr == GNW_H7B0_RCC_AHB1RSTR_OFFSET ||
        addr == GNW_H7B0_RCC_APB2RSTR_OFFSET) {
        /* Store first: device_cold_reset() must not see a stale RSTR. */
        s->regs[addr >> 2] = value;
        gnw_h7b0_rcc_apply_periph_reset(s, addr, old_value, value);
        return;
    }

    switch (addr) {
    case GNW_H7B0_RCC_CR:
        {
            uint32_t sws = (s->regs[GNW_H7B0_RCC_CFGR >> 2] &
                            RCC_CFGR_SWS_MASK) >> RCC_CFGR_SWS_SHIFT;

            /* RM0455 8.7.1: hardware refuses to stop the active SYSCLK
             * source. PLL1's reference oscillator is protected with it. */
            if (sws == RCC_CFGR_SWS_HSI) {
                value |= RCC_CR_HSION;
            } else if (sws == RCC_CFGR_SWS_CSI) {
                value |= RCC_CR_CSION;
            } else if (sws == RCC_CFGR_SWS_HSE) {
                value |= RCC_CR_HSEON;
            } else if (sws == RCC_CFGR_SWS_PLL1) {
                uint32_t pllsrc =
                    (s->regs[GNW_H7B0_RCC_PLLCKSELR_OFFSET >> 2] &
                     RCC_PLLCKSELR_PLLSRC_MASK) >>
                    RCC_PLLCKSELR_PLLSRC_SHIFT;

                value |= RCC_CR_PLL1ON;
                if (pllsrc == RCC_PLLCKSELR_PLLSRC_HSI) {
                    value |= RCC_CR_HSION;
                } else if (pllsrc == RCC_PLLCKSELR_PLLSRC_CSI) {
                    value |= RCC_CR_CSION;
                } else if (pllsrc == RCC_PLLCKSELR_PLLSRC_HSE) {
                    value |= RCC_CR_HSEON;
                }
            }
        }
        /*
         * Mirror each *ON bit into its *RDY bit instantly (not
         * cycle-accurate -- real hardware takes some cycles to lock).
         * Good enough to unblock a polling loop, which is this stub's
         * only job.
         */
        if (value & RCC_CR_HSION) {
            value |= RCC_CR_HSIRDY;
        } else {
            value &= ~RCC_CR_HSIRDY;
        }
        if (value & RCC_CR_HSEON) {
            value |= RCC_CR_HSERDY;
        } else {
            value &= ~RCC_CR_HSERDY;
        }
        /*
         * CSION -> CSIRDY, same instant-approximation as every other
         * oscillator here. Found missing entirely 2026-07-13 while
         * investigating Mario CFW running at ~half real-hardware
         * speed: firmware's SystemClock_Config() requests CSI ON
         * (confirmed live) as part of the same HAL_RCC_OscConfig()-
         * style call that also configures PLL1, and PLL1 never
         * reported ready in QEMU across several boot traces --
         * confirmed live that CSION was set (bit 7) while CSIRDY
         * (bit 8) stayed permanently clear, unlike every other
         * oscillator's RDY bit, which all mirror correctly. If
         * firmware's oscillator-enable loop waits on CSIRDY before
         * or interleaved with the PLL1 configuration steps, a CSI
         * readiness wait that can never succeed would plausibly
         * explain the run-to-run-inconsistent PLL1 failure observed
         * this session (sometimes never reaching the PLL1DIVR write,
         * sometimes reaching it with what looked like a corrupted
         * value) -- not yet confirmed as the complete fix, but a
         * real, concrete gap either way.
         */
        if (value & RCC_CR_CSION) {
            value |= RCC_CR_CSIRDY;
        } else {
            value &= ~RCC_CR_CSIRDY;
        }
        if (value & RCC_CR_PLL1ON) {
            value |= RCC_CR_PLL1RDY;
        } else {
            value &= ~RCC_CR_PLL1RDY;
        }
        if (value & RCC_CR_PLL2ON) {
            value |= RCC_CR_PLL2RDY;
        } else {
            value &= ~RCC_CR_PLL2RDY;
        }
        if (value & RCC_CR_PLL3ON) {
            value |= RCC_CR_PLL3RDY;
        } else {
            value &= ~RCC_CR_PLL3RDY;
        }
        /*
         * HSI48ON -> HSI48RDY, same instant-approximation as every other
         * oscillator here. Found missing entirely 2026-07-16 while
         * chasing stm32h7b0-diag's crypto_rng_sanity/crypto_rng_seed_error
         * cases (RNG's kernel clock source, RCC_D2CCIP2R.RNGSEL's reset
         * default, is HSI48): firmware's HSI48ON-then-wait-for-HSI48RDY
         * loop spun for its full bounded iteration count every time,
         * exactly the same "*ON set, *RDY never mirrors" gap the CSI fix
         * above describes -- HSI48 just never got the same mirroring
         * treatment as every other oscillator.
         */
        if (value & RCC_CR_HSI48ON) {
            value |= RCC_CR_HSI48RDY;
        } else {
            value &= ~RCC_CR_HSI48RDY;
        }
        s->regs[addr >> 2] = value;
        gnw_h7b0_rcc_complete_pending_sysclk_switch(s);
        if (!defer_clock_update) {
            gnw_h7b0_rcc_update_sysclk_clock(s);
        }
        return;
    case GNW_H7B0_RCC_CFGR:
        /* SW records the request. Hardware updates SWS only after that source
         * is ready; an unavailable request leaves the active clock intact. */
        value = (value & ~RCC_CFGR_SWS_MASK) |
                (s->regs[addr >> 2] & RCC_CFGR_SWS_MASK);
        s->regs[addr >> 2] = value;
        gnw_h7b0_rcc_complete_pending_sysclk_switch(s);
        if (!defer_clock_update) {
            gnw_h7b0_rcc_update_sysclk_clock(s);
        }
        return;
    case GNW_H7B0_RCC_CDCFGR1_OFFSET:
        /* CDCPRE affects CPU/DWT/SysTick and HPRE affects the AHB domain.
         * Recompute the propagated core clock on this register write. */
        s->regs[addr >> 2] = value;
        if (!defer_clock_update) {
            gnw_h7b0_rcc_update_sysclk_clock(s);
        }
        return;
    case GNW_H7B0_RCC_CSR:
        /* Mirror LSION into LSIRDY, same instant-approximation as CR. */
        if (value & RCC_CSR_LSION) {
            value |= RCC_CSR_LSIRDY;
        } else {
            value &= ~RCC_CSR_LSIRDY;
        }
        s->regs[addr >> 2] = value;
        return;
    case GNW_H7B0_RCC_BDCR:
        /*
         * Real G&W hardware may not even populate an LSE crystal, but
         * firmware spins on LSERDY regardless (sometimes with a timeout,
         * sometimes without -- confirmed both in retro-go and in a stock
         * firmware's HAL_RCCEx_PeriphCLKConfig RTCSEL-select path, which
         * clears LSEON to 0 via a BDRST pulse while reprogramming RTCSEL
         * and then waits on LSERDY in that same call), so this must
         * always come ready to avoid a permanent hang -- unconditionally,
         * not just mirroring LSEON, since real firmware's own BDRST
         * pulse legitimately clears LSEON right before this wait.
         */
        value |= RCC_BDCR_LSERDY;
        if (!(old_value & RCC_BDCR_BDRST) && (value & RCC_BDCR_BDRST) &&
            s->backup_domain_reset) {
            s->backup_domain_reset(s->backup_domain_reset_opaque);
        }
        s->regs[addr >> 2] = value;
        return;
    case GNW_H7B0_RCC_RSR:
        /*
         * Writing RMVF (bit16) clears the *RSTF cause flags on real
         * hardware -- keep RMVF itself readable as written (firmware
         * doesn't rely on it self-clearing here) but zero the rest.
         */
        s->regs[addr >> 2] = value & RCC_RSR_RMVF;
        return;
    case GNW_H7B0_RCC_PLLCKSELR_OFFSET:
        s->regs[addr >> 2] = value;
        if (!defer_clock_update) {
            gnw_h7b0_rcc_update_sysclk_clock(s);
        }
        return;
    case GNW_H7B0_RCC_PLLCFGR_OFFSET:
        if (!(old_value & RCC_PLLCFGR_PLL1FRACEN) &&
            (value & RCC_PLLCFGR_PLL1FRACEN)) {
            s->pll1_fracn_latched =
                (s->regs[GNW_H7B0_RCC_PLL1FRACR_OFFSET >> 2] &
                 RCC_PLL1FRACR_FRACN1_MASK) >>
                RCC_PLL1FRACR_FRACN1_SHIFT;
        }
        if (!(old_value & RCC_PLLCFGR_PLL2FRACEN) &&
            (value & RCC_PLLCFGR_PLL2FRACEN)) {
            s->pll2_fracn_latched =
                (s->regs[GNW_H7B0_RCC_PLL2FRACR_OFFSET >> 2] &
                 RCC_PLL2FRACR_FRACN2_MASK) >>
                RCC_PLL2FRACR_FRACN2_SHIFT;
        }
        if (!(old_value & RCC_PLLCFGR_PLL3FRACEN) &&
            (value & RCC_PLLCFGR_PLL3FRACEN)) {
            s->pll3_fracn_latched =
                (s->regs[GNW_H7B0_RCC_PLL3FRACR_OFFSET >> 2] &
                 RCC_PLL3FRACR_FRACN3_MASK) >>
                RCC_PLL3FRACR_FRACN3_SHIFT;
        }
        s->regs[addr >> 2] = value;
        if (!defer_clock_update) {
            gnw_h7b0_rcc_update_sysclk_clock(s);
        }
        return;
    case GNW_H7B0_RCC_PLL1DIVR_OFFSET:
    case GNW_H7B0_RCC_PLL1FRACR_OFFSET:
        /*
         * Otherwise plain read/write shadows (PLL2's half of PLLCKSELR/
         * PLLCFGR included -- gnw_h7b0_rcc_get_pll2p_hz() reads those
         * live, no recompute-and-cache needed there), but any of these
         * can change PLL1's output and thus SYSCLK -- recompute and
         * push the new rate out to the board's Clock.
         */
        s->regs[addr >> 2] = value;
        if (!defer_clock_update) {
            gnw_h7b0_rcc_update_sysclk_clock(s);
        }
        return;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: offset 0x%"HWADDR_PRIx" is a plain read/write "
                      "shadow, no real clock behavior modeled\n",
                      __func__, addr);
        s->regs[addr >> 2] = value;
    }
}

/* Keep cycle-domain consumers coherent around each RCC timing transition.
 * Prepare settles every consumer against one old icount stamp before the
 * register image or core-cycle denominator changes.  Commit rebases them to
 * one post-boundary stamp and the newly decoded peripheral rates. */
static void gnw_h7b0_rcc_write(void *opaque, hwaddr addr,
                                uint64_t val64, unsigned int size)
{
    GnwH7B0RccState *s = GNW_H7B0_RCC(opaque);
    uint32_t old_core_hz = gnw_h7b0_rcc_get_core_hz(s);
    GnwH7B0RccAdcClockInputs old_adc_inputs =
        gnw_h7b0_rcc_adc_clock_inputs(s);
    uint32_t old_apb1_hz = gnw_h7b0_rcc_timer_clock_hz(
        s, GNW_H7B0_RCC_TIMER_APB1);
    uint32_t old_apb2_hz = gnw_h7b0_rcc_timer_clock_hz(
        s, GNW_H7B0_RCC_TIMER_APB2);
    uint32_t old_octospi_hz = gnw_h7b0_rcc_get_octospi_kernel_hz(s);
    GnwH7B0RccSai1ClockInfo old_sai1_info =
        gnw_h7b0_rcc_sai1_clock_info_snapshot(s);
    GnwH7B0RccEngineClockInfo old_engines[GNW_H7B0_RCC_ENGINE_COUNT];
    IcountCycleStamp before = { 0 }, after = { 0 }, actual_before;
    bool timing_active = s->n_timing_observers && icount_enabled() &&
                         old_core_hz &&
                         gnw_h7b0_rcc_write_affects_timing(addr);

    for (int i = 0; i < GNW_H7B0_RCC_ENGINE_COUNT; i++) {
        old_engines[i] = gnw_h7b0_rcc_engine_clock_info(s, i);
    }

    if (timing_active) {
        icount_get_cycle_stamp(&before);
        g_assert(before.core_hz == old_core_hz);
        for (int i = 0; i < s->n_timing_observers; i++) {
            GnwH7B0RccTimingObserver *observer = &s->timing_observers[i];
            uint32_t old_hz = gnw_h7b0_rcc_timing_domain_hz(
                s, observer->domain);

            if (!observer->prepare(observer->opaque, &before, old_hz)) {
                for (int j = i - 1; j >= 0; j--) {
                    s->timing_observers[j].abort(
                        s->timing_observers[j].opaque);
                }
                return;
            }
        }
    }

    gnw_h7b0_rcc_write_register(opaque, addr, val64, size, timing_active);

    if (timing_active) {
        uint32_t new_core_hz = gnw_h7b0_rcc_get_core_hz(s);
        uint32_t new_apb1_hz = gnw_h7b0_rcc_timer_clock_hz(
            s, GNW_H7B0_RCC_TIMER_APB1);
        uint32_t new_apb2_hz = gnw_h7b0_rcc_timer_clock_hz(
            s, GNW_H7B0_RCC_TIMER_APB2);
        bool rates_changed = old_core_hz != new_core_hz ||
                             old_apb1_hz != new_apb1_hz ||
                             old_apb2_hz != new_apb2_hz;

        if (rates_changed && new_core_hz) {
            icount_cycle_clock_boundary(new_core_hz,
                                        old_core_hz == new_core_hz,
                                        &actual_before, &after);
            g_assert(actual_before.modeled_cycles == before.modeled_cycles &&
                     actual_before.idle_ns == before.idle_ns &&
                     actual_before.core_hz == before.core_hz &&
                     actual_before.generation == before.generation);
        } else {
            after = before;
        }

        /* Publish the new Clock rates only after icount has crossed the
         * matching cycle-frequency boundary. Clock callbacks are synchronous
         * and may sample virtual time, so they must never observe the new
         * CPU period with the old icount denominator. */
        gnw_h7b0_rcc_update_sysclk_clock(s);

        for (int i = 0; i < s->n_timing_observers; i++) {
            GnwH7B0RccTimingObserver *observer = &s->timing_observers[i];
            observer->commit(observer->opaque, &before, &after,
                gnw_h7b0_rcc_timing_domain_hz(s, observer->domain));
        }
    }

    if (!timing_active) {
        gnw_h7b0_rcc_notify_timer_clock_change(
            s, GNW_H7B0_RCC_TIMER_APB1, old_apb1_hz,
            gnw_h7b0_rcc_timer_clock_hz(s, GNW_H7B0_RCC_TIMER_APB1));
        gnw_h7b0_rcc_notify_timer_clock_change(
            s, GNW_H7B0_RCC_TIMER_APB2, old_apb2_hz,
            gnw_h7b0_rcc_timer_clock_hz(s, GNW_H7B0_RCC_TIMER_APB2));
    }
    gnw_h7b0_rcc_notify_adc_clock_change(
        s, old_adc_inputs, gnw_h7b0_rcc_adc_clock_inputs(s));
    gnw_h7b0_rcc_notify_octospi_clock_change(
        s, old_octospi_hz, gnw_h7b0_rcc_get_octospi_kernel_hz(s));
    gnw_h7b0_rcc_notify_sai1_clock_change(
        s, old_sai1_info, gnw_h7b0_rcc_sai1_clock_info_snapshot(s));
    gnw_h7b0_rcc_notify_engine_clocks(s, old_engines);
}

/*
 * PLL2P output frequency, decoded live from PLLCKSELR/PLL2DIVR/PLL2FRACR --
 * see gnw_h7b0_rcc.h for why (SAI1 sample-rate derivation). Matches
 * HAL_RCCEx_GetPLL2ClockFreq()'s fractional-N formula
 * (stm32h7xx_hal_rcc_ex.c): VCO = (HSE/DIVM2) * (N2_raw + FRACN2/8192 + 1),
 * PLL2P = VCO / (P2_raw + 1). N2_raw/P2_raw are the *raw register fields*
 * (PLL2DIVR bits, unmodified) -- do NOT add 1 when decoding them here: the
 * config side (__HAL_RCC_PLL2_CONFIG) stores (PLL2N - 1)/(PLL2P - 1) into
 * those fields, and this getter's own "+1" is what cancels that back out
 * for N (netting VCO's N term to the real PLL2N with no adjustment needed
 * here at all), while P's "+1" is applied explicitly at the division step
 * below, matching HAL_RCCEx_GetPLL2ClockFreq line-for-line. Getting this
 * backwards (adding an extra +1 to N2 here) silently produced numbers off
 * by roughly N/(N-1) -- confirmed against a live run where real firmware's
 * ACR1.MCKDIV readback only made sense against the *uncorrected* raw-field
 * formula, not the doubly-adjusted one.
 * Returns 0 for invalid/unconfigured dividers (DIVM2 == 0) rather than
 * dividing by zero -- callers fall back to their own default in that case.
 */

static uint32_t gnw_h7b0_rcc_get_hsi_ker_hz(GnwH7B0RccState *s);
static uint32_t gnw_h7b0_rcc_get_csi_ker_hz(GnwH7B0RccState *s);
static uint32_t gnw_h7b0_rcc_get_hse_hz(GnwH7B0RccState *s);

/*
 * PLLCKSELR.PLLSRC (bits[1:0]) is shared between PLL1 and PLL2 -- one
 * oscillator mux feeds both PLLs' input dividers. Factored out so
 * gnw_h7b0_rcc_get_pll1p_hz() doesn't duplicate gnw_h7b0_rcc_get_pll2p_hz()'s
 * switch statement.
 */
static uint32_t gnw_h7b0_rcc_get_osc_hz(GnwH7B0RccState *s)
{
    uint32_t sel = s->regs[GNW_H7B0_RCC_PLLCKSELR_OFFSET >> 2];
    uint32_t pllsrc = (sel & RCC_PLLCKSELR_PLLSRC_MASK) >> RCC_PLLCKSELR_PLLSRC_SHIFT;

    switch (pllsrc) {
    case RCC_PLLCKSELR_PLLSRC_HSE:
        return gnw_h7b0_rcc_get_hse_hz(s);
    case RCC_PLLCKSELR_PLLSRC_CSI:
        return gnw_h7b0_rcc_get_csi_ker_hz(s);
    case RCC_PLLCKSELR_PLLSRC_HSI:
        return gnw_h7b0_rcc_get_hsi_ker_hz(s);
    default:
        return 0;
    }
}

static uint32_t gnw_h7b0_rcc_get_hsi_ker_hz(GnwH7B0RccState *s)
{
    uint32_t cr = s->regs[GNW_H7B0_RCC_CR_OFFSET >> 2];

    if ((cr & (RCC_CR_HSION | RCC_CR_HSIRDY)) !=
        (RCC_CR_HSION | RCC_CR_HSIRDY)) {
        return 0;
    }
    /* RM0455 §8.7.1: hsi_ck and hsi_ker_ck follow HSIDIV[1:0]. */
    return GNW_H7B0_RCC_HSI_HZ >> ((cr >> 3) & 0x3);
}

static uint32_t gnw_h7b0_rcc_get_csi_ker_hz(GnwH7B0RccState *s)
{
    uint32_t cr = s->regs[GNW_H7B0_RCC_CR_OFFSET >> 2];

    return (cr & (RCC_CR_CSION | RCC_CR_CSIRDY)) ==
           (RCC_CR_CSION | RCC_CR_CSIRDY) ? GNW_H7B0_RCC_CSI_HZ : 0;
}

static uint32_t gnw_h7b0_rcc_get_hse_hz(GnwH7B0RccState *s)
{
    uint32_t cr = s->regs[GNW_H7B0_RCC_CR_OFFSET >> 2];

    return (cr & (RCC_CR_HSEON | RCC_CR_HSERDY)) ==
           (RCC_CR_HSEON | RCC_CR_HSERDY) ? GNW_H7B0_RCC_HSE_HZ : 0;
}

uint32_t gnw_h7b0_rcc_get_pll2p_hz(GnwH7B0RccState *s)
{
    uint32_t sel = s->regs[GNW_H7B0_RCC_PLLCKSELR_OFFSET >> 2];
    uint32_t cfgr = s->regs[GNW_H7B0_RCC_PLLCFGR_OFFSET >> 2];
    uint32_t divr = s->regs[GNW_H7B0_RCC_PLL2DIVR_OFFSET >> 2];

    uint32_t divm2 = (sel & RCC_PLLCKSELR_DIVM2_MASK) >> RCC_PLLCKSELR_DIVM2_SHIFT;
    uint32_t n2_raw = (divr & RCC_PLL2DIVR_N2_MASK) >> RCC_PLL2DIVR_N2_SHIFT;
    uint32_t p2_raw = (divr & RCC_PLL2DIVR_P2_MASK) >> RCC_PLL2DIVR_P2_SHIFT;
    uint32_t fracn2 = (cfgr & RCC_PLLCFGR_PLL2FRACEN) ?
                      s->pll2_fracn_latched : 0;
    uint32_t osc_hz = gnw_h7b0_rcc_get_osc_hz(s);

    /*
     * A stopped PLL produces no output clock -- pll2_p_ck is dead while
     * CR.PLL2ON is clear (RM0455 RCC: PLL2ON enables PLL2; PLL2RDY, which
     * this model already mirrors from it, reports the PLL locked).
     *
     * This is not a pedantic detail: RCCEx_PLL2_Config() (the path
     * HAL_RCCEx_PeriphCLKConfig() takes for RCC_PERIPHCLK_SAI1) *always*
     * disables PLL2 and spins on PLL2RDY going 0 before rewriting the
     * dividers, so any firmware that changes its SAI sample rate --
     * retro-go does exactly this every time it enters or leaves a game
     * core -- has a real window where the SAI kernel clock is stopped.
     * Reporting the newly-programmed frequency straight through that
     * window made the SAI look like it was still clocking and requesting
     * DMA, which kept gnw_h7b0_dma.c manufacturing half/full-transfer
     * interrupts into firmware that was mid-teardown and no longer
     * acknowledging them -- a permanent DMA1_Stream0 interrupt storm and
     * a black screen on "quit to main menu".
     */
    if (!(s->regs[GNW_H7B0_RCC_CR_OFFSET >> 2] & RCC_CR_PLL2ON)) {
        return 0;
    }

    if (divm2 == 0) {
        return 0;
    }

    /* VCO = osc * (N2_raw + FRACN2/8192 + 1) / DIVM2 -- numerator/
     * denominator kept as 64-bit integers scaled by 8192 to avoid
     * floating point. */
    uint64_t numerator = (uint64_t)osc_hz *
                          (n2_raw * 8192ULL + fracn2 + 8192ULL);
    uint64_t denominator = (uint64_t)divm2 * 8192ULL;
    uint64_t vco = numerator / denominator;

    return (uint32_t)(vco / (p2_raw + 1));
}

/*
 * PLL1P output frequency -- same VCO/output formula and same N/P raw-field
 * handling as gnw_h7b0_rcc_get_pll2p_hz() (see its comment for why N gets
 * no extra +1 here beyond the one already in the VCO term), just PLL1's
 * own registers/fields (DIVM1, PLL1FRACEN, PLL1DIVR, PLL1FRACR) instead of
 * PLL2's. This is the CPU/SYSCLK PLL -- see gnw_h7b0_rcc_get_sysclk_hz().
 */
uint32_t gnw_h7b0_rcc_get_pll1p_hz(GnwH7B0RccState *s)
{
    uint32_t sel = s->regs[GNW_H7B0_RCC_PLLCKSELR_OFFSET >> 2];
    uint32_t cfgr = s->regs[GNW_H7B0_RCC_PLLCFGR_OFFSET >> 2];
    uint32_t divr = s->regs[GNW_H7B0_RCC_PLL1DIVR_OFFSET >> 2];

    uint32_t divm1 = (sel & RCC_PLLCKSELR_DIVM1_MASK) >> RCC_PLLCKSELR_DIVM1_SHIFT;
    uint32_t n1_raw = (divr & RCC_PLL1DIVR_N1_MASK) >> RCC_PLL1DIVR_N1_SHIFT;
    uint32_t p1_raw = (divr & RCC_PLL1DIVR_P1_MASK) >> RCC_PLL1DIVR_P1_SHIFT;
    uint32_t fracn1 = (cfgr & RCC_PLLCFGR_PLL1FRACEN) ?
                      s->pll1_fracn_latched : 0;
    uint32_t osc_hz = gnw_h7b0_rcc_get_osc_hz(s);

    if (divm1 == 0) {
        return 0;
    }

    uint64_t numerator = (uint64_t)osc_hz *
                          (n1_raw * 8192ULL + fracn1 + 8192ULL);
    uint64_t denominator = (uint64_t)divm1 * 8192ULL;
    uint64_t vco = numerator / denominator;

    return (uint32_t)(vco / (p1_raw + 1));
}

/*
 * PLL3R output frequency (pll3_r_ck) -- LTDC's pixel clock, hardwired to
 * this PLL output with no clock-source mux (unlike SAI1/ADC). Same VCO/
 * output formula and same N-raw-field handling as gnw_h7b0_rcc_get_pll1p_hz()/
 * gnw_h7b0_rcc_get_pll2p_hz() (see the latter's comment for why N gets no
 * extra +1 beyond the one already folded into the VCO term); R's own "+1"
 * is applied explicitly at the division step below, same as P for PLL1/
 * PLL2. Used by gnw_h7b0_ltdc.c to derive the real vblank rate instead of
 * assuming a fixed 60Hz.
 */
uint32_t gnw_h7b0_rcc_get_pll3r_hz(GnwH7B0RccState *s)
{
    uint32_t sel = s->regs[GNW_H7B0_RCC_PLLCKSELR_OFFSET >> 2];
    uint32_t cfgr = s->regs[GNW_H7B0_RCC_PLLCFGR_OFFSET >> 2];
    uint32_t divr = s->regs[GNW_H7B0_RCC_PLL3DIVR_OFFSET >> 2];

    uint32_t divm3 = (sel & RCC_PLLCKSELR_DIVM3_MASK) >> RCC_PLLCKSELR_DIVM3_SHIFT;
    uint32_t n3_raw = (divr & RCC_PLL3DIVR_N3_MASK) >> RCC_PLL3DIVR_N3_SHIFT;
    uint32_t r3_raw = (divr & RCC_PLL3DIVR_R3_MASK) >> RCC_PLL3DIVR_R3_SHIFT;
    uint32_t fracn3 = (cfgr & RCC_PLLCFGR_PLL3FRACEN) ?
                      s->pll3_fracn_latched : 0;
    uint32_t osc_hz = gnw_h7b0_rcc_get_osc_hz(s);

    if (divm3 == 0) {
        return 0;
    }

    uint64_t numerator = (uint64_t)osc_hz *
                          (n3_raw * 8192ULL + fracn3 + 8192ULL);
    uint64_t denominator = (uint64_t)divm3 * 8192ULL;
    uint64_t vco = numerator / denominator;

    return (uint32_t)(vco / (r3_raw + 1));
}

/*
 * Effective SYSCLK, respecting CFGR.SWS -- real firmware's overclock
 * sequence briefly drops SW to HSI before reprogramming PLL1, then
 * switches back (see gnw_h7b0_rcc.h's plan-doc reference); RCC already
 * mirrors SW into SWS instantly in gnw_h7b0_rcc_write(), so this getter
 * naturally reflects that transient with no special-casing.
 */
uint32_t gnw_h7b0_rcc_get_sysclk_hz(GnwH7B0RccState *s)
{
    uint32_t cfgr = s->regs[GNW_H7B0_RCC_CFGR >> 2];
    uint32_t sws = (cfgr & RCC_CFGR_SWS_MASK) >> RCC_CFGR_SWS_SHIFT;

    switch (sws) {
    case RCC_CFGR_SWS_HSI:
        return gnw_h7b0_rcc_get_hsi_ker_hz(s);
    case RCC_CFGR_SWS_CSI:
        return gnw_h7b0_rcc_get_csi_ker_hz(s);
    case RCC_CFGR_SWS_HSE:
        return gnw_h7b0_rcc_get_hse_hz(s);
    case RCC_CFGR_SWS_PLL1:
    default:
        return gnw_h7b0_rcc_get_pll1p_hz(s);
    }
}

/* The Cortex-M7 clock and HCLK are distinct on H7B0. The HAL's
 * HAL_RCC_GetHCLKFreq() first divides SYSCLK by CDCPRE, then divides that
 * core rate by HPRE; see the D1CorePrescTable in the checked-in ST HAL. */
uint32_t gnw_h7b0_rcc_get_core_hz(GnwH7B0RccState *s)
{
    uint32_t cdcfgr1 = s->regs[GNW_H7B0_RCC_CDCFGR1_OFFSET >> 2];

    return gnw_h7b0_rcc_core_hz_from_regs(
        gnw_h7b0_rcc_get_sysclk_hz(s), cdcfgr1);
}

uint32_t gnw_h7b0_rcc_get_hclk_hz(GnwH7B0RccState *s)
{
    uint32_t cdcfgr1 = s->regs[GNW_H7B0_RCC_CDCFGR1_OFFSET >> 2];

    return gnw_h7b0_rcc_hclk_hz_from_regs(
        gnw_h7b0_rcc_get_sysclk_hz(s), cdcfgr1);
}

/*
 * TIM2-TIM7's kernel clock, decoded from the APB1 prescaler rather than
 * assumed equal to HCLK.
 *
 * STM32H7 timers do NOT simply run at PCLK. With CFGR.TIMPRE=0 the kernel
 * clock is HCLK when the APB prescaler is /1, and 2 x PCLK for any larger
 * prescaler. With TIMPRE=1 it is HCLK for /1, /2, and /4, and 4 x PCLK
 * for /8 and /16 (RM0455 "Timer clock prescaler selection").
 *
 * This used to be hardcoded as "timer_clk == HCLK", which is only correct
 * for retro-go's config (APB1 /2, where the x2 rule exactly cancels the
 * /2). Stock Nintendo firmware runs APB1 /8 with TIMPRE=0, so its real
 * timer clock is 2 x HCLK/8 = HCLK/4 -- and measuring the physical device
 * confirmed it: TIM5's CNT advances at 1.003MHz there (PSC=0x15, so a
 * 22MHz kernel clock), while this model produced 3.998MHz, exactly 4x too
 * fast.
 *
 * That mattered far beyond timer accuracy: stock firmware polls TIM5's CNT
 * ~540 times a second as its microsecond time base and synthesises audio
 * for however much time it believes has elapsed. Running the counter 4x
 * fast made it generate ~4x too much audio per DMA buffer -- audible as
 * the "crunchy, echoing, playing more than it should" corruption that this
 * whole investigation started from. Retro-go was unaffected precisely
 * because its APB1 /2 prescaler yields HCLK under either TIMPRE setting.
 */
static uint32_t gnw_h7b0_rcc_get_timer_apb_ker_hz(GnwH7B0RccState *s,
                                                  unsigned ppre_shift)
{
    uint32_t cfgr = s->regs[GNW_H7B0_RCC_CFGR_OFFSET >> 2];
    uint32_t cdcfgr2 = s->regs[GNW_H7B0_RCC_CDCFGR2_OFFSET >> 2];
    uint32_t hclk = gnw_h7b0_rcc_get_hclk_hz(s);

    return gnw_h7b0_timer_kernel_hz(hclk, cfgr, cdcfgr2, ppre_shift);
}

uint32_t gnw_h7b0_rcc_get_timer_ker_hz(GnwH7B0RccState *s)
{
    return gnw_h7b0_rcc_get_timer_apb_ker_hz(s, 4);
}

uint32_t gnw_h7b0_rcc_get_timer_apb2_ker_hz(GnwH7B0RccState *s)
{
    return gnw_h7b0_rcc_get_timer_apb_ker_hz(s, 8);
}

void gnw_h7b0_rcc_set_sysclk(GnwH7B0RccState *s, Clock *sysclk)
{
    s->sysclk = sysclk;
}

void gnw_h7b0_rcc_init_timing_clock(GnwH7B0RccState *s)
{
    uint32_t core_hz = gnw_h7b0_rcc_get_core_hz(s);

    g_assert(icount_enabled());
    /* The board wires SYSCLK at its real reset rate (HSI, 64 MHz), but RCC
     * reset registers are populated by the first machine reset, after device
     * realize. Before that reset, HSION/HSIRDY read as zero and the register
     * decoder intentionally reports no running oscillator. Seed icount from
     * the already-connected reset clock in this short pre-reset window. */
    if (!core_hz && s->sysclk) {
        core_hz = clock_get_hz(s->sysclk);
    }
    g_assert(core_hz);
    g_assert(s->n_timing_observers == 0);
    icount_set_cycle_clock_hz(core_hz);
}

void gnw_h7b0_rcc_set_systick_refclk(GnwH7B0RccState *s,
                                     Clock *systick_refclk)
{
    s->systick_refclk = systick_refclk;
}

/*
 * Push the core rate (SYSCLK / CDCPRE) and SysTick reference rate
 * (HCLK / 8) to their independent consumers. Called from every write case
 * that can change SYSCLK, CDCPRE, or HPRE; clock callbacks update SysTick
 * synchronously in the guest MMIO write handler.
 */
static void gnw_h7b0_rcc_update_sysclk_clock(GnwH7B0RccState *s)
{
    if (s->sysclk) {
        clock_update_hz(s->sysclk, gnw_h7b0_rcc_get_core_hz(s));
    }
    if (s->systick_refclk) {
        clock_update_hz(s->systick_refclk,
                        gnw_h7b0_rcc_get_hclk_hz(s) / 8);
    }
}

/* RCC_APB2ENR.SAI1EN (bit 22, RM0455 RCC register map) -- the SAI1
 * peripheral's own bus/kernel clock enable. */
#define RCC_APB2ENR_SAI1EN (1U << 22)
#define RCC_PLLCFGR_DIVP2EN (1U << 19)

bool gnw_h7b0_rcc_sai1_clock_enabled(GnwH7B0RccState *s)
{
    return (s->regs[GNW_H7B0_RCC_APB2ENR_OFFSET >> 2]
             & RCC_APB2ENR_SAI1EN) != 0;
}

bool gnw_h7b0_rcc_get_sai1_clock_info(GnwH7B0RccState *s,
                                      uint32_t *kernel_hz)
{
    uint32_t ccip1r = s->regs[GNW_H7B0_RCC_CDCCIP1R_OFFSET >> 2];
    uint32_t sai1sel = (ccip1r & RCC_CDCCIP1R_SAI1SEL_MASK) >>
                       RCC_CDCCIP1R_SAI1SEL_SHIFT;
    uint32_t cr = s->regs[GNW_H7B0_RCC_CR_OFFSET >> 2];
    uint32_t cfgr = s->regs[GNW_H7B0_RCC_PLLCFGR_OFFSET >> 2];

    if (!kernel_hz) {
        return false;
    }

    /* RM0455 §8.7.20: SAI1SEL is PLL1Q, PLL2P, PLL3P, external I2S_CKIN,
     * per_ck, then reserved encodings. The oscillator and PLL helpers check
     * ON/RDY and output-enable state; reserved selections are known stopped,
     * while the unmodeled external pin remains UNKNOWN. */
    switch (sai1sel) {
    case 0:
        *kernel_hz = gnw_h7b0_rcc_get_pll1q_hz(s);
        return true;
    case 1:
        if ((cr & (RCC_CR_PLL2ON | RCC_CR_PLL2RDY)) !=
                (RCC_CR_PLL2ON | RCC_CR_PLL2RDY) ||
            !(cfgr & RCC_PLLCFGR_DIVP2EN)) {
            *kernel_hz = 0;
        } else {
            *kernel_hz = gnw_h7b0_rcc_get_pll2p_hz(s);
        }
        return true;
    case 2:
        *kernel_hz = gnw_h7b0_rcc_get_pll3p_hz(s);
        return true;
    case 3: /* External I2S_CKIN is not modeled. */
        *kernel_hz = 0;
        return false;
    case 4: { /* per_ck: CKPERSEL chooses ready HSI, CSI, or HSE. */
        uint32_t ccipr = s->regs[GNW_H7B0_RCC_CDCCIPR_OFFSET >> 2];
        switch ((ccipr >> 28) & 0x3) {
        case 0:
            *kernel_hz = gnw_h7b0_rcc_get_hsi_ker_hz(s);
            break;
        case 1:
            *kernel_hz = gnw_h7b0_rcc_get_csi_ker_hz(s);
            break;
        case 2:
            *kernel_hz = gnw_h7b0_rcc_get_hse_hz(s);
            break;
        default:
            *kernel_hz = 0;
            break;
        }
        return true;
    }
    default:
        *kernel_hz = 0;
        return true;
    }
}

uint32_t gnw_h7b0_rcc_get_sai1_kernel_hz(GnwH7B0RccState *s)
{
    uint32_t ccip1r = s->regs[GNW_H7B0_RCC_CDCCIP1R_OFFSET >> 2];
    uint32_t sai1sel = (ccip1r & RCC_CDCCIP1R_SAI1SEL_MASK) >>
                        RCC_CDCCIP1R_SAI1SEL_SHIFT;

    if (sai1sel == RCC_CDCCIP1R_SAI1SEL_PLL2) {
        return gnw_h7b0_rcc_get_pll2p_hz(s);
    }

    qemu_log_mask(LOG_UNIMP,
                  "%s: SAI1SEL=%u not PLL2 -- not a path this firmware uses, "
                  "not modeled\n", __func__, sai1sel);
    return 0;
}

/* RM0455 §8.7.20, Figure 62 and §27.4.3. ADCSEL chooses PLL2P, PLL3R or
 * per_ck; per_ck is selected by CDCCIPR.CKPERSEL. Return zero for a stopped
 * source or a reserved mux value rather than inventing a usable clock. */
#define RCC_SRDCCIPR_ADCSEL_SHIFT 16
#define RCC_SRDCCIPR_ADCSEL_MASK  (0x3U << RCC_SRDCCIPR_ADCSEL_SHIFT)
#define RCC_CDCCIPR_CKPERSEL_SHIFT 28
#define RCC_CDCCIPR_CKPERSEL_MASK (0x3U << RCC_CDCCIPR_CKPERSEL_SHIFT)

uint32_t gnw_h7b0_rcc_get_adc_source_hz(GnwH7B0RccState *s)
{
    uint32_t selector =
        (s->regs[GNW_H7B0_RCC_SRDCCIPR_OFFSET >> 2] &
         RCC_SRDCCIPR_ADCSEL_MASK) >> RCC_SRDCCIPR_ADCSEL_SHIFT;

    switch (selector) {
    case 0: /* pll2_p_ck */
        return gnw_h7b0_rcc_get_pll2p_hz(s);
    case 1: /* pll3_r_ck */
        return gnw_h7b0_rcc_get_pll3r_hz(s);
    case 2: { /* per_ck */
        uint32_t ccipr = s->regs[GNW_H7B0_RCC_CDCCIPR_OFFSET >> 2];
        uint32_t per_selector = (ccipr & RCC_CDCCIPR_CKPERSEL_MASK) >>
                                RCC_CDCCIPR_CKPERSEL_SHIFT;

        switch (per_selector) {
        case 0: /* hsi_ker_ck */
            return gnw_h7b0_rcc_get_hsi_ker_hz(s);
        case 1: /* csi_ker_ck */
            return gnw_h7b0_rcc_get_csi_ker_hz(s);
        case 2: /* hse_ck */
            return gnw_h7b0_rcc_get_hse_hz(s);
        default:
            return 0;
        }
    }
    default: /* ADCSEL=3 is reserved and disables the kernel clock. */
        return 0;
    }
}

/* RM0455 Figure 62: adc_sclk is sys_ck_cpu when HPRE=/1, otherwise
 * sys_ck_cpu/2. sys_ck_cpu is the core rate after CDCPRE. */
static uint32_t gnw_h7b0_rcc_get_adc_sync_source_hz(GnwH7B0RccState *s)
{
    uint32_t cdcfgr1 = s->regs[GNW_H7B0_RCC_CDCFGR1_OFFSET >> 2];
    uint32_t hpre_code = cdcfgr1 & 0xf;
    uint32_t core_hz = gnw_h7b0_rcc_get_core_hz(s);

    return gnw_h7b0_rcc_prescaler_shift(hpre_code) == 0 ? core_hz :
                                                            core_hz / 2;
}

static GnwH7B0RccAdcClockInputs gnw_h7b0_rcc_adc_clock_inputs(
    GnwH7B0RccState *s)
{
    return (GnwH7B0RccAdcClockInputs) {
        .async_hz = gnw_h7b0_rcc_get_adc_source_hz(s),
        .sync_hz = gnw_h7b0_rcc_get_adc_sync_source_hz(s),
    };
}

/* RM0455 §27.4.3: the common prescaler is used only in asynchronous mode,
 * CKMODE selects async or sys_ck_cpu/HPRE divided by 1, 2 or 4, and a further
 * divide-by-two always feeds the analog ADC. */
uint32_t gnw_h7b0_rcc_get_adc_kernel_hz(GnwH7B0RccState *s,
                                        uint32_t common_ccr)
{
    static const uint16_t async_divisors[12] = {
        1, 2, 4, 6, 8, 10, 12, 16, 32, 64, 128, 256,
    };
    uint32_t ckmode = (common_ccr >> 16) & 0x3;

    if (ckmode == 0) {
        uint32_t presc = (common_ccr >> 18) & 0xf;
        uint32_t source_hz;

        if (presc >= ARRAY_SIZE(async_divisors)) {
            return 0;
        }
        source_hz = gnw_h7b0_rcc_get_adc_source_hz(s);
        return source_hz / (async_divisors[presc] * 2U);
    }

    uint32_t sync_divisor = 1U << (ckmode - 1);
    return gnw_h7b0_rcc_get_adc_sync_source_hz(s) /
           (sync_divisor * 2U);
}

bool gnw_h7b0_rcc_get_adc_clock_info(GnwH7B0RccState *s,
                                     uint32_t common_ccr,
                                     uint32_t *kernel_hz)
{
    static const uint16_t async_divisors[] = {
        1, 2, 4, 6, 8, 10, 12, 16, 32, 64, 128, 256,
    };
    uint32_t ckmode = (common_ccr >> 16) & 0x3;

    if (ckmode) {
        /* CKMODE selects the modeled core-derived synchronous input and
         * does not depend on ADCSEL/CKPERSEL. */
        *kernel_hz = gnw_h7b0_rcc_get_adc_kernel_hz(s, common_ccr);
        return true;
    }

    uint32_t presc = (common_ccr >> 18) & 0xf;
    uint32_t selector =
        (s->regs[GNW_H7B0_RCC_SRDCCIPR_OFFSET >> 2] >> 16) & 0x3;
    bool source_known = selector <= 2;

    if (selector == 2) {
        uint32_t ccipr = s->regs[GNW_H7B0_RCC_CDCCIPR_OFFSET >> 2];
        source_known = ((ccipr >> 28) & 0x3) <= 2;
    }
    if (!source_known || presc >= ARRAY_SIZE(async_divisors)) {
        *kernel_hz = 0;
        return false;
    }

    *kernel_hz = gnw_h7b0_rcc_get_adc_kernel_hz(s, common_ccr);
    return true;
}

/* RM0455 §8.7.20: OCTOSPISEL selects rcc_hclk3, pll1_q_ck, pll2_r_ck,
 * or per_ck. Only outputs with both their PLL and output divider enabled are
 * live. Keep the existing SYSCLK/PLL getters' semantics unchanged. */
#define RCC_CDCCIP2R_OCTOSPISEL_SHIFT 4
#define RCC_CDCCIP2R_OCTOSPISEL_MASK  (0x3U << RCC_CDCCIP2R_OCTOSPISEL_SHIFT)
#define RCC_PLLCFGR_DIVQ1EN           (1U << 17)
#define RCC_PLLCFGR_DIVR2EN           (1U << 21)
#define RCC_PLL1DIVR_Q1_SHIFT         16
#define RCC_PLL1DIVR_Q1_MASK          (0x7fU << RCC_PLL1DIVR_Q1_SHIFT)
#define RCC_PLL2DIVR_R2_SHIFT         24
#define RCC_PLL2DIVR_R2_MASK          (0x7fU << RCC_PLL2DIVR_R2_SHIFT)

static uint32_t gnw_h7b0_rcc_get_pll1q_hz(GnwH7B0RccState *s)
{
    uint32_t cr = s->regs[GNW_H7B0_RCC_CR_OFFSET >> 2];
    uint32_t cfgr = s->regs[GNW_H7B0_RCC_PLLCFGR_OFFSET >> 2];
    uint32_t sel = s->regs[GNW_H7B0_RCC_PLLCKSELR_OFFSET >> 2];
    uint32_t divr = s->regs[GNW_H7B0_RCC_PLL1DIVR_OFFSET >> 2];
    uint32_t divm = (sel & RCC_PLLCKSELR_DIVM1_MASK) >>
                    RCC_PLLCKSELR_DIVM1_SHIFT;
    uint32_t n_raw = (divr & RCC_PLL1DIVR_N1_MASK) >>
                     RCC_PLL1DIVR_N1_SHIFT;
    uint32_t q_raw = (divr & RCC_PLL1DIVR_Q1_MASK) >>
                     RCC_PLL1DIVR_Q1_SHIFT;
    uint32_t fracn = (cfgr & RCC_PLLCFGR_PLL1FRACEN) ?
                     s->pll1_fracn_latched : 0;

    if ((cr & (RCC_CR_PLL1ON | RCC_CR_PLL1RDY)) !=
        (RCC_CR_PLL1ON | RCC_CR_PLL1RDY) ||
        !(cfgr & RCC_PLLCFGR_DIVQ1EN) || !divm) {
        return 0;
    }

    uint64_t numerator = (uint64_t)gnw_h7b0_rcc_get_osc_hz(s) *
                         (n_raw * 8192ULL + fracn + 8192ULL);
    uint64_t denominator = (uint64_t)divm * 8192ULL;
    return (uint32_t)((numerator / denominator) / (q_raw + 1));
}

/* PLL3P is one of SAI1's internal clock mux sources. The formula and
 * readiness/output-enable checks match the PLL3R decoder above. */
static uint32_t gnw_h7b0_rcc_get_pll3p_hz(GnwH7B0RccState *s)
{
    const uint32_t divp3en = 1U << 22;
    const uint32_t p3_shift = 9;
    const uint32_t p3_mask = 0x7fU << p3_shift;
    uint32_t cr = s->regs[GNW_H7B0_RCC_CR_OFFSET >> 2];
    uint32_t cfgr = s->regs[GNW_H7B0_RCC_PLLCFGR_OFFSET >> 2];
    uint32_t sel = s->regs[GNW_H7B0_RCC_PLLCKSELR_OFFSET >> 2];
    uint32_t divr = s->regs[GNW_H7B0_RCC_PLL3DIVR_OFFSET >> 2];
    uint32_t divm3 = (sel & RCC_PLLCKSELR_DIVM3_MASK) >>
                     RCC_PLLCKSELR_DIVM3_SHIFT;
    uint32_t n3_raw = (divr & RCC_PLL3DIVR_N3_MASK) >>
                      RCC_PLL3DIVR_N3_SHIFT;
    uint32_t p3_raw = (divr & p3_mask) >> p3_shift;
    uint32_t fracn3 = (cfgr & RCC_PLLCFGR_PLL3FRACEN) ?
                      s->pll3_fracn_latched : 0;

    if ((cr & (RCC_CR_PLL3ON | RCC_CR_PLL3RDY)) !=
            (RCC_CR_PLL3ON | RCC_CR_PLL3RDY) ||
        !(cfgr & divp3en) || !divm3) {
        return 0;
    }

    uint64_t numerator = (uint64_t)gnw_h7b0_rcc_get_osc_hz(s) *
                         (n3_raw * 8192ULL + fracn3 + 8192ULL);
    uint64_t denominator = (uint64_t)divm3 * 8192ULL;
    return (uint32_t)((numerator / denominator) / (p3_raw + 1));
}

static uint32_t gnw_h7b0_rcc_get_pll2r_hz(GnwH7B0RccState *s)
{
    uint32_t cr = s->regs[GNW_H7B0_RCC_CR_OFFSET >> 2];
    uint32_t cfgr = s->regs[GNW_H7B0_RCC_PLLCFGR_OFFSET >> 2];
    uint32_t sel = s->regs[GNW_H7B0_RCC_PLLCKSELR_OFFSET >> 2];
    uint32_t divr = s->regs[GNW_H7B0_RCC_PLL2DIVR_OFFSET >> 2];
    uint32_t divm = (sel & RCC_PLLCKSELR_DIVM2_MASK) >>
                    RCC_PLLCKSELR_DIVM2_SHIFT;
    uint32_t n_raw = (divr & RCC_PLL2DIVR_N2_MASK) >>
                     RCC_PLL2DIVR_N2_SHIFT;
    uint32_t r_raw = (divr & RCC_PLL2DIVR_R2_MASK) >>
                     RCC_PLL2DIVR_R2_SHIFT;
    uint32_t fracn = (cfgr & RCC_PLLCFGR_PLL2FRACEN) ?
                     s->pll2_fracn_latched : 0;

    if ((cr & (RCC_CR_PLL2ON | RCC_CR_PLL2RDY)) !=
        (RCC_CR_PLL2ON | RCC_CR_PLL2RDY) ||
        !(cfgr & RCC_PLLCFGR_DIVR2EN) || !divm) {
        return 0;
    }

    uint64_t numerator = (uint64_t)gnw_h7b0_rcc_get_osc_hz(s) *
                         (n_raw * 8192ULL + fracn + 8192ULL);
    uint64_t denominator = (uint64_t)divm * 8192ULL;
    return (uint32_t)((numerator / denominator) / (r_raw + 1));
}

uint32_t gnw_h7b0_rcc_get_octospi_kernel_hz(GnwH7B0RccState *s)
{
    uint32_t cdcip2r = s->regs[GNW_H7B0_RCC_CDCCIP2R_OFFSET >> 2];
    uint32_t selector = (cdcip2r & RCC_CDCCIP2R_OCTOSPISEL_MASK) >>
                        RCC_CDCCIP2R_OCTOSPISEL_SHIFT;

    switch (selector) {
    case 0: /* rcc_hclk3 */
        return gnw_h7b0_rcc_get_hclk_hz(s);
    case 1: /* pll1_q_ck */
        return gnw_h7b0_rcc_get_pll1q_hz(s);
    case 2: /* pll2_r_ck */
        return gnw_h7b0_rcc_get_pll2r_hz(s);
    case 3: { /* per_ck, selected by CKPERSEL */
        uint32_t cdcipr = s->regs[GNW_H7B0_RCC_CDCCIPR_OFFSET >> 2];
        uint32_t per_selector = (cdcipr & RCC_CDCCIPR_CKPERSEL_MASK) >>
                                RCC_CDCCIPR_CKPERSEL_SHIFT;

        switch (per_selector) {
        case 0:
            return gnw_h7b0_rcc_get_hsi_ker_hz(s);
        case 1:
            return gnw_h7b0_rcc_get_csi_ker_hz(s);
        case 2:
            return gnw_h7b0_rcc_get_hse_hz(s);
        default:
            return 0;
        }
    }
    default:
        return 0;
    }
}

static const MemoryRegionOps gnw_h7b0_rcc_ops = {
    .read = gnw_h7b0_rcc_read,
    .write = gnw_h7b0_rcc_write,
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

static void gnw_h7b0_rcc_test_core_hz_get(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    GnwH7B0RccState *s = GNW_H7B0_RCC(obj);
    uint32_t hz = gnw_h7b0_rcc_get_core_hz(s);

    visit_type_uint32(v, name, &hz, errp);
}

static void gnw_h7b0_rcc_init(Object *obj)
{
    GnwH7B0RccState *s = GNW_H7B0_RCC(obj);

    object_property_add(obj, "x-core-hz", "uint32",
                        gnw_h7b0_rcc_test_core_hz_get,
                        NULL, NULL, NULL);
    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_rcc_ops, s,
                           TYPE_GNW_H7B0_RCC, GNW_H7B0_RCC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const VMStateDescription vmstate_gnw_h7b0_rcc = {
    .name = TYPE_GNW_H7B0_RCC,
    .version_id = 3,
    .minimum_version_id = 1,
    .post_load_errp = gnw_h7b0_rcc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0RccState, GNW_H7B0_RCC_SIZE / 4),
        VMSTATE_UINT32_V(pll1_fracn_latched, GnwH7B0RccState, 3),
        VMSTATE_UINT32_V(pll2_fracn_latched, GnwH7B0RccState, 3),
        VMSTATE_UINT32_V(pll3_fracn_latched, GnwH7B0RccState, 3),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_rcc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_rcc;
    rc->phases.hold = gnw_h7b0_rcc_reset_hold;
}

static const TypeInfo gnw_h7b0_rcc_info = {
    .name          = TYPE_GNW_H7B0_RCC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0RccState),
    .instance_init = gnw_h7b0_rcc_init,
    .class_init    = gnw_h7b0_rcc_class_init,
};

static void gnw_h7b0_rcc_register_types(void)
{
    type_register_static(&gnw_h7b0_rcc_info);
}

type_init(gnw_h7b0_rcc_register_types)
