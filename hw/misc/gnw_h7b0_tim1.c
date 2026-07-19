/*
 * Stub for TIM1 (advanced-control timer), extended with a real counter
 * for CR1.CEN -- see gnw_h7b0_tim1.h. Was a plain register shadow with
 * no counting/UIF side effects at all (unlike TIM2-TIM14's block model
 * in gnw_h7b0_tim2.c, which already got this treatment); found via
 * stm32h7b0-diag's tim1_pwm_correct case timing out forever busy-waiting
 * on SR.UIF after CR1.CEN, and separately checking that CNT visibly
 * advances between two closely-spaced reads -- neither ever happened on
 * a plain shadow.
 *
 * Unlike gnw_h7b0_tim2.c's model (which only sets SR.UIF once a full
 * period elapses and never touches CNT itself), CNT here is computed
 * live on every read from elapsed virtual-clock time since the last
 * counter (re)start, so even a short busy-wait between two CNT reads
 * -- well under one full ARR period -- observes real advancement.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/misc/gnw_h7b0_tim1.h"
#include "hw/misc/gnw_h7b0_regs_tim1.h"

#define GNW_H7B0_TIM1_APPROX_CLK_HZ 340512000ULL
/* Same clamp rationale as gnw_h7b0_tim2.c's MIN/MAX_PERIOD_NS. */
#define GNW_H7B0_TIM1_MIN_PERIOD_NS 1000ULL
#define GNW_H7B0_TIM1_MAX_PERIOD_NS (2 * NANOSECONDS_PER_SECOND)

void gnw_h7b0_tim1_set_rcc(GnwH7B0Tim1State *s, GnwH7B0RccState *rcc)
{
    s->rcc = rcc;
}

static uint64_t gnw_h7b0_tim1_period_ns(GnwH7B0Tim1State *s)
{
    uint32_t psc = s->regs[GNW_H7B0_TIM1_PSC_OFFSET >> 2];
    uint32_t arr = s->regs[GNW_H7B0_TIM1_ARR_OFFSET >> 2];
    uint64_t clk_hz = GNW_H7B0_TIM1_APPROX_CLK_HZ;
    uint64_t period_ns;

    if (s->rcc) {
        uint32_t live_hz = gnw_h7b0_rcc_get_hclk_hz(s->rcc);
        if (live_hz != 0) {
            clk_hz = live_hz;
        }
    }

    period_ns = (uint64_t)(arr + 1) * (psc + 1)
                * NANOSECONDS_PER_SECOND / clk_hz;

    period_ns = MAX(period_ns, GNW_H7B0_TIM1_MIN_PERIOD_NS);
    period_ns = MIN(period_ns, GNW_H7B0_TIM1_MAX_PERIOD_NS);
    return period_ns;
}

/* Single tick-period (PSC+1 kernel clocks), same clock source as
 * gnw_h7b0_tim1_period_ns() -- used to convert elapsed real time into a
 * live CNT value. */
static uint64_t gnw_h7b0_tim1_tick_ns(GnwH7B0Tim1State *s)
{
    uint32_t psc = s->regs[GNW_H7B0_TIM1_PSC_OFFSET >> 2];
    uint64_t clk_hz = GNW_H7B0_TIM1_APPROX_CLK_HZ;

    if (s->rcc) {
        uint32_t live_hz = gnw_h7b0_rcc_get_hclk_hz(s->rcc);
        if (live_hz != 0) {
            clk_hz = live_hz;
        }
    }

    uint64_t tick_ns = (uint64_t)(psc + 1) * NANOSECONDS_PER_SECOND / clk_hz;
    return MAX(tick_ns, 1);
}

static void gnw_h7b0_tim1_start_counting(GnwH7B0Tim1State *s)
{
    s->count_start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_mod(s->count_timer, s->count_start_ns + gnw_h7b0_tim1_period_ns(s));
}

static void gnw_h7b0_tim1_timer_tick(void *opaque)
{
    GnwH7B0Tim1State *s = GNW_H7B0_TIM1(opaque);
    uint32_t cr1 = s->regs[GNW_H7B0_TIM1_CR1_OFFSET >> 2];

    s->regs[GNW_H7B0_TIM1_SR_OFFSET >> 2] |= 0x1u; /* UIF */

    if (cr1 & (1u << 3)) {
        /* OPM (one-pulse mode): counter stops itself after the update
         * event, same as real hardware. */
        s->regs[GNW_H7B0_TIM1_CR1_OFFSET >> 2] = cr1 & ~0x1u;
        return;
    }

    if (cr1 & 0x1u) {
        gnw_h7b0_tim1_start_counting(s);
    }
}

static void gnw_h7b0_tim1_reset(DeviceState *dev)
{
    GnwH7B0Tim1State *s = GNW_H7B0_TIM1(dev);
    for (int i = 0; i < (GNW_H7B0_TIM1_SIZE / 4); i++) {
        s->regs[i] = get_tim1_reset_value(i * 4);
    }
    timer_del(s->count_timer);
    s->count_start_ns = 0;
}

static uint64_t gnw_h7b0_tim1_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0Tim1State *s = GNW_H7B0_TIM1(opaque);
    if (addr >= GNW_H7B0_TIM1_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return 0;
    }

    if (addr == GNW_H7B0_TIM1_CNT_OFFSET
        && (s->regs[GNW_H7B0_TIM1_CR1_OFFSET >> 2] & 0x1u)) {
        /* Live-compute CNT while counting, instead of the static
         * register value: elapsed time since the last (re)start, mod
         * the tick period, capped at ARR (matches real up-counter
         * behavior between reload events). */
        uint32_t arr = s->regs[GNW_H7B0_TIM1_ARR_OFFSET >> 2];
        uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        uint64_t elapsed_ns = now - s->count_start_ns;
        uint64_t ticks = elapsed_ns / gnw_h7b0_tim1_tick_ns(s);
        uint32_t cnt = (uint32_t)MIN(ticks, (uint64_t)arr);
        s->regs[GNW_H7B0_TIM1_CNT_OFFSET >> 2] = cnt;
        return cnt;
    }

    return s->regs[addr >> 2];
}

static void gnw_h7b0_tim1_write(void *opaque, hwaddr addr, uint64_t val64, unsigned int size)
{
    GnwH7B0Tim1State *s = GNW_H7B0_TIM1(opaque);
    if (addr >= GNW_H7B0_TIM1_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return;
    }
    uint32_t mask = get_tim1_write_mask(addr);
    uint32_t old_value = s->regs[addr >> 2];
    s->regs[addr >> 2] = (old_value & ~mask) | ((uint32_t)val64 & mask);
    if (addr == GNW_H7B0_TIM1_EGR_OFFSET) {
        /* EGR.UG (bit 0): force an immediate update event -- reset CNT's
         * live-counting epoch and set UIF, same as real hardware's
         * "forced update" side effect (and mirrors gnw_h7b0_tim2.c's
         * generic EGR.UG -> SR.UIF handling for the TIM2-TIM14 block).
         *
         * Also reschedule count_timer via gnw_h7b0_tim1_start_counting()
         * when CEN is currently set, not just reset count_start_ns/UIF
         * for CNT-readback purposes -- otherwise the *next* real UIF
         * still fires at whatever absolute virtual-clock time was
         * originally scheduled back at CR1.CEN, silently ignoring the
         * forced reload. Found via stm32h7b0-diag's tim1_pwm_correct
         * case: it clears SR then re-issues EGR.UG immediately before
         * timing the next update event specifically to get a fresh full
         * period, and without this reschedule that measurement still
         * randomly came up short (whatever fraction of the original
         * period had already elapsed before the EGR.UG write), exactly
         * as if this fix were never applied on the firmware side. Real
         * silicon genuinely restarts the countdown on a forced update;
         * the model needs to actually do that too, not just fake the
         * CNT/UIF side effects. */
        if (val64 & 0x1u) {
            s->regs[GNW_H7B0_TIM1_SR_OFFSET >> 2] |= 0x1u;
            if (s->regs[GNW_H7B0_TIM1_CR1_OFFSET >> 2] & 0x1u) {
                gnw_h7b0_tim1_start_counting(s);
            } else {
                s->count_start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            }
        }
        s->regs[addr >> 2] = 0; /* EGR is write-only; real hardware/SVD always read it back as 0 */
    }

    if (addr == GNW_H7B0_TIM1_CR1_OFFSET) {
        uint32_t new_cr1 = s->regs[addr >> 2];
        if ((new_cr1 & 0x1u) && !(old_value & 0x1u)) {
            gnw_h7b0_tim1_start_counting(s);
        } else if (!(new_cr1 & 0x1u)) {
            timer_del(s->count_timer);
        }
    }
}

static const MemoryRegionOps gnw_h7b0_tim1_ops = {
    .read = gnw_h7b0_tim1_read,
    .write = gnw_h7b0_tim1_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void gnw_h7b0_tim1_init(Object *obj)
{
    GnwH7B0Tim1State *s = GNW_H7B0_TIM1(obj);
    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_tim1_ops, s, TYPE_GNW_H7B0_TIM1, GNW_H7B0_TIM1_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    s->count_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gnw_h7b0_tim1_timer_tick, s);
}

static const VMStateDescription vmstate_gnw_h7b0_tim1 = {
    .name = TYPE_GNW_H7B0_TIM1,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0Tim1State, GNW_H7B0_TIM1_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_tim1_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &vmstate_gnw_h7b0_tim1;
    device_class_set_legacy_reset(dc, gnw_h7b0_tim1_reset);
}

static const TypeInfo gnw_h7b0_tim1_info = {
    .name          = TYPE_GNW_H7B0_TIM1,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0Tim1State),
    .instance_init = gnw_h7b0_tim1_init,
    .class_init    = gnw_h7b0_tim1_class_init,
};

static void gnw_h7b0_tim1_register_types(void)
{
    type_register_static(&gnw_h7b0_tim1_info);
}
type_init(gnw_h7b0_tim1_register_types)
