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
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/misc/gnw_h7b0_tim1.h"
#include "hw/misc/gnw_h7b0_regs_tim1.h"
#include "hw/misc/gnw_h7b0_timer_clock.h"

#define GNW_H7B0_TIM1_APPROX_CLK_HZ 340512000ULL
/* Same clamp rationale as gnw_h7b0_tim2.c's MIN/MAX_PERIOD_NS. */
#define GNW_H7B0_TIM1_MIN_PERIOD_NS 1000ULL
#define GNW_H7B0_TIM1_MAX_PERIOD_NS (2 * NANOSECONDS_PER_SECOND)

static void gnw_h7b0_tim1_clock_changed(void *opaque, uint32_t old_hz,
                                         uint32_t new_hz);

static void gnw_h7b0_tim1_update_irq(GnwH7B0Tim1State *s)
{
    uint32_t dier = s->regs[GNW_H7B0_TIM1_DIER_OFFSET >> 2];
    uint32_t sr = s->regs[GNW_H7B0_TIM1_SR_OFFSET >> 2];

    qemu_set_irq(s->irq, (dier & sr & 1u) != 0);
}

/* Kernel-clock source shared by the update-period and live-CNT paths. */
static uint32_t gnw_h7b0_tim1_kernel_hz(GnwH7B0Tim1State *s)
{
    if (s->rcc) {
        /* A wired zero is a stopped clock, not an unavailable clock model. */
        return gnw_h7b0_rcc_get_timer_apb2_ker_hz(s->rcc);
    }
    return GNW_H7B0_TIM1_APPROX_CLK_HZ;
}

static void gnw_h7b0_tim1_add_kernel_ticks(GnwH7B0Tim1State *s,
                                           uint32_t *cnt_base,
                                           uint32_t *psc_remainder,
                                           uint64_t kernel_ticks)
{
    uint32_t psc = s->regs[GNW_H7B0_TIM1_PSC_OFFSET >> 2] &
                   GNW_H7B0_TIM1_COUNTER_MASK;
    uint32_t arr = s->regs[GNW_H7B0_TIM1_ARR_OFFSET >> 2] &
                   GNW_H7B0_TIM1_COUNTER_MASK;
    uint64_t prescale = (uint64_t)psc + 1;
    uint64_t total;

    g_assert(kernel_ticks <= UINT64_MAX - *psc_remainder);
    total = kernel_ticks + *psc_remainder;
    *psc_remainder = total % prescale;
    *cnt_base = (*cnt_base + (total / prescale) % ((uint64_t)arr + 1)) %
                ((uint64_t)arr + 1);
}

static void gnw_h7b0_tim1_settle(GnwH7B0Tim1State *s, uint64_t now,
                                  uint32_t kernel_hz)
{
    uint64_t elapsed_ns, kernel_cycles, prescaled_ticks, modulus;
    uint64_t remainder;
    uint32_t psc, arr;

    if (s->timing_mode && icount_enabled()) {
        IcountCycleStamp stamp;
        uint64_t ticks;
        bool counting = s->regs[GNW_H7B0_TIM1_CR1_OFFSET >> 2] & 1U;

        icount_get_cycle_stamp(&stamp);
        g_assert(gnw_h7b0_cycle_phase_settle(
            &s->cycle_phase, stamp, counting ? kernel_hz : 0, &ticks));
        if (counting) {
            gnw_h7b0_tim1_add_kernel_ticks(s, &s->cnt_base,
                                           &s->psc_remainder, ticks);
        }
        s->regs[GNW_H7B0_TIM1_CNT_OFFSET >> 2] = s->cnt_base;
        s->cnt_ref_ns = now;
        return;
    }
    if (!(s->regs[GNW_H7B0_TIM1_CR1_OFFSET >> 2] & 0x1u) ||
        now <= s->cnt_ref_ns) {
        return;
    }
    elapsed_ns = now - s->cnt_ref_ns;
    kernel_cycles = gnw_h7b0_timer_elapsed_kernel_cycles(
        elapsed_ns, kernel_hz, &s->kernel_fraction);
    psc = s->regs[GNW_H7B0_TIM1_PSC_OFFSET >> 2] &
          GNW_H7B0_TIM1_COUNTER_MASK;
    arr = s->regs[GNW_H7B0_TIM1_ARR_OFFSET >> 2] &
          GNW_H7B0_TIM1_COUNTER_MASK;
    modulus = (uint64_t)arr + 1;
    prescaled_ticks = kernel_cycles / ((uint64_t)psc + 1);
    remainder = kernel_cycles % ((uint64_t)psc + 1);
    remainder += s->psc_remainder;
    if (remainder >= (uint64_t)psc + 1) {
        remainder -= (uint64_t)psc + 1;
        prescaled_ticks++;
    }
    s->psc_remainder = remainder;
    s->cnt_base = (s->cnt_base + prescaled_ticks % modulus) % modulus;
    s->regs[GNW_H7B0_TIM1_CNT_OFFSET >> 2] = s->cnt_base;
    s->cnt_ref_ns = now;
}

static uint64_t gnw_h7b0_tim1_remaining_ns_exact(GnwH7B0Tim1State *s,
                                                  uint32_t kernel_hz)
{
    uint32_t psc = s->regs[GNW_H7B0_TIM1_PSC_OFFSET >> 2] &
                   GNW_H7B0_TIM1_COUNTER_MASK;
    uint32_t arr = s->regs[GNW_H7B0_TIM1_ARR_OFFSET >> 2] &
                   GNW_H7B0_TIM1_COUNTER_MASK;
    uint64_t modulus = (uint64_t)arr + 1;
    uint64_t remaining_ticks = modulus - (s->cnt_base % modulus);
    uint64_t kernel_cycles = remaining_ticks * ((uint64_t)psc + 1) -
                             s->psc_remainder;

    if (s->timing_mode && icount_enabled()) {
        return gnw_h7b0_timer_kernel_q64_to_ns_ceil(
            kernel_cycles, s->cycle_phase.fraction_q64, kernel_hz);
    }
    return gnw_h7b0_timer_kernel_cycles_to_ns_ceil(
        kernel_cycles, s->kernel_fraction, kernel_hz);
}

static uint64_t gnw_h7b0_tim1_remaining_ns(GnwH7B0Tim1State *s,
                                           uint32_t kernel_hz)
{
    uint64_t interval = gnw_h7b0_tim1_remaining_ns_exact(s, kernel_hz);

    return gnw_h7b0_timer_apply_period_limits(
        interval, s->timing_mode, GNW_H7B0_TIM1_MIN_PERIOD_NS,
        GNW_H7B0_TIM1_MAX_PERIOD_NS);
}

static void gnw_h7b0_tim1_schedule(GnwH7B0Tim1State *s, uint32_t kernel_hz)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!kernel_hz) {
        timer_del(s->count_timer);
        return;
    }
    timer_mod(s->count_timer, gnw_h7b0_timer_deadline_sat(
                  now, gnw_h7b0_tim1_remaining_ns(s, kernel_hz)));
}

static void gnw_h7b0_tim1_start_counting(GnwH7B0Tim1State *s)
{
    s->cnt_ref_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (s->timing_mode && icount_enabled()) {
        IcountCycleStamp stamp;

        icount_get_cycle_stamp(&stamp);
        g_assert(gnw_h7b0_cycle_phase_init(&s->cycle_phase, stamp));
    }
    gnw_h7b0_tim1_schedule(s, gnw_h7b0_tim1_kernel_hz(s));
}

static bool gnw_h7b0_tim1_timing_prepare(
    void *opaque, const IcountCycleStamp *before, uint32_t old_hz)
{
    GnwH7B0Tim1State *s = opaque;
    bool counting = s->regs[GNW_H7B0_TIM1_CR1_OFFSET >> 2] & 1U;
    uint64_t ticks;

    s->pending_cycle_phase = s->cycle_phase;
    s->pending_cnt_base = s->cnt_base;
    s->pending_psc_remainder = s->psc_remainder;
    if (!gnw_h7b0_cycle_phase_settle(&s->pending_cycle_phase, *before,
                                      counting ? old_hz : 0, &ticks)) {
        return false;
    }
    if (counting) {
        gnw_h7b0_tim1_add_kernel_ticks(s, &s->pending_cnt_base,
                                       &s->pending_psc_remainder, ticks);
    }
    s->pending_phase_valid = true;
    return true;
}

static void gnw_h7b0_tim1_timing_commit(
    void *opaque, const IcountCycleStamp *before,
    const IcountCycleStamp *after, uint32_t new_hz)
{
    GnwH7B0Tim1State *s = opaque;

    g_assert(s->pending_phase_valid);
    g_assert(gnw_h7b0_cycle_phase_rebase(&s->pending_cycle_phase,
                                          *before, *after));
    s->cycle_phase = s->pending_cycle_phase;
    s->cnt_base = s->pending_cnt_base;
    s->psc_remainder = s->pending_psc_remainder;
    s->regs[GNW_H7B0_TIM1_CNT_OFFSET >> 2] = s->cnt_base;
    s->pending_phase_valid = false;
    if (s->regs[GNW_H7B0_TIM1_CR1_OFFSET >> 2] & 1U) {
        gnw_h7b0_tim1_schedule(s, new_hz);
    }
}

static void gnw_h7b0_tim1_timing_abort(void *opaque)
{
    GnwH7B0Tim1State *s = opaque;

    s->pending_phase_valid = false;
}

static void gnw_h7b0_tim1_clock_changed(void *opaque, uint32_t old_hz,
                                         uint32_t new_hz)
{
    GnwH7B0Tim1State *s = opaque;
    uint64_t now;

    if (!(s->regs[GNW_H7B0_TIM1_CR1_OFFSET >> 2] & 0x1u)) {
        return;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    gnw_h7b0_tim1_settle(s, now, old_hz);
    gnw_h7b0_tim1_schedule(s, new_hz);
}

void gnw_h7b0_tim1_set_rcc(GnwH7B0Tim1State *s, GnwH7B0RccState *rcc)
{
    s->rcc = rcc;
    gnw_h7b0_rcc_add_timer_clock_observer(
        rcc, GNW_H7B0_RCC_TIMER_APB2, gnw_h7b0_tim1_clock_changed, s);
    if (s->timing_mode && icount_enabled()) {
        gnw_h7b0_rcc_add_timing_observer(
            rcc, GNW_H7B0_RCC_TIMING_APB2,
            gnw_h7b0_tim1_timing_prepare,
            gnw_h7b0_tim1_timing_commit,
            gnw_h7b0_tim1_timing_abort, s);
    }
}

static void gnw_h7b0_tim1_timer_tick(void *opaque)
{
    GnwH7B0Tim1State *s = GNW_H7B0_TIM1(opaque);
    uint32_t cr1 = s->regs[GNW_H7B0_TIM1_CR1_OFFSET >> 2];
    uint32_t kernel_hz = gnw_h7b0_tim1_kernel_hz(s);
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool elapsed_update = !s->timing_mode && now > s->cnt_ref_ns &&
        now - s->cnt_ref_ns >=
            gnw_h7b0_tim1_remaining_ns_exact(s, kernel_hz);

    /* Default mode caps host scheduling intervals at two seconds. A timer
     * deadline beyond that cap is only a checkpoint, not an update event.
     * Preserve the elapsed CNT and defer UIF/IRQ until the real ARR expiry. */
    if (!s->timing_mode && !elapsed_update &&
        gnw_h7b0_tim1_remaining_ns_exact(s, kernel_hz) >
            GNW_H7B0_TIM1_MAX_PERIOD_NS) {
        gnw_h7b0_tim1_settle(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                              kernel_hz);
        gnw_h7b0_tim1_schedule(s, kernel_hz);
        return;
    }

    if (s->timing_mode || elapsed_update) {
        gnw_h7b0_tim1_settle(s,
            now,
            kernel_hz);
    } else {
        s->cnt_base = 0;
        s->regs[GNW_H7B0_TIM1_CNT_OFFSET >> 2] = 0;
        s->kernel_fraction = 0;
        s->psc_remainder = 0;
        s->cnt_ref_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    s->regs[GNW_H7B0_TIM1_SR_OFFSET >> 2] |= 0x1u; /* UIF */
    gnw_h7b0_tim1_update_irq(s);

    if (cr1 & (1u << 3)) {
        /* OPM (one-pulse mode): counter stops itself after the update
         * event, same as real hardware. */
        if (elapsed_update) {
            s->cnt_base = 0;
            s->regs[GNW_H7B0_TIM1_CNT_OFFSET >> 2] = 0;
            s->kernel_fraction = 0;
            s->psc_remainder = 0;
        }
        s->regs[GNW_H7B0_TIM1_CR1_OFFSET >> 2] = cr1 & ~0x1u;
        return;
    }

    if (cr1 & 0x1u) {
        gnw_h7b0_tim1_schedule(s, kernel_hz);
    }
}

static void gnw_h7b0_tim1_reset(DeviceState *dev)
{
    GnwH7B0Tim1State *s = GNW_H7B0_TIM1(dev);
    for (int i = 0; i < (GNW_H7B0_TIM1_SIZE / 4); i++) {
        s->regs[i] = get_tim1_reset_value(i * 4);
    }
    timer_del(s->count_timer);
    s->cnt_ref_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->kernel_fraction = 0;
    s->psc_remainder = 0;
    s->cnt_base = 0;
    if (s->timing_mode && icount_enabled()) {
        IcountCycleStamp stamp;

        icount_get_cycle_stamp(&stamp);
        g_assert(gnw_h7b0_cycle_phase_init(&s->cycle_phase, stamp));
    }
    gnw_h7b0_tim1_update_irq(s);
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
        gnw_h7b0_tim1_settle(s,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
            gnw_h7b0_tim1_kernel_hz(s));
        return s->cnt_base;
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
    bool was_counting = s->regs[GNW_H7B0_TIM1_CR1_OFFSET >> 2] & 0x1u;

    if (was_counting && (addr == GNW_H7B0_TIM1_CNT_OFFSET ||
                         addr == GNW_H7B0_TIM1_PSC_OFFSET ||
                         addr == GNW_H7B0_TIM1_ARR_OFFSET ||
                         addr == GNW_H7B0_TIM1_CR1_OFFSET ||
                         (addr == GNW_H7B0_TIM1_EGR_OFFSET &&
                          (val64 & 1u)))) {
        gnw_h7b0_tim1_settle(s,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
            gnw_h7b0_tim1_kernel_hz(s));
    }
    if (addr == GNW_H7B0_TIM1_SR_OFFSET) {
        /* STM32 timer status flags are rc_w0: writing 0 clears a set
         * flag, while writing 1 leaves it unchanged. */
        s->regs[addr >> 2] = old_value & ((uint32_t)val64 | ~mask);
    } else {
        s->regs[addr >> 2] = (old_value & ~mask) | ((uint32_t)val64 & mask);
    }

    if (addr == GNW_H7B0_TIM1_CNT_OFFSET) {
        s->cnt_base = s->regs[addr >> 2] & GNW_H7B0_TIM1_COUNTER_MASK;
        s->cnt_ref_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    } else if (addr == GNW_H7B0_TIM1_PSC_OFFSET) {
        /* A new prescaler starts with an empty prescaler counter. */
        s->psc_remainder = 0;
    } else if (addr == GNW_H7B0_TIM1_ARR_OFFSET) {
        uint64_t modulus =
            (uint64_t)(s->regs[addr >> 2] & GNW_H7B0_TIM1_COUNTER_MASK) + 1;
        s->cnt_base %= modulus;
        s->regs[GNW_H7B0_TIM1_CNT_OFFSET >> 2] = s->cnt_base;
    }
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
            gnw_h7b0_tim1_update_irq(s);
            s->cnt_base = 0;
            s->regs[GNW_H7B0_TIM1_CNT_OFFSET >> 2] = 0;
            s->psc_remainder = 0;
            s->cnt_ref_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            if (s->timing_mode && icount_enabled()) {
                IcountCycleStamp stamp;

                icount_get_cycle_stamp(&stamp);
                g_assert(gnw_h7b0_cycle_phase_init(&s->cycle_phase, stamp));
            }
            if (s->regs[GNW_H7B0_TIM1_CR1_OFFSET >> 2] & 0x1u) {
                gnw_h7b0_tim1_schedule(s, gnw_h7b0_tim1_kernel_hz(s));
            }
        }
        s->regs[addr >> 2] = 0; /* EGR is write-only; real hardware/SVD always read it back as 0 */
    }

    if (addr == GNW_H7B0_TIM1_DIER_OFFSET ||
        addr == GNW_H7B0_TIM1_SR_OFFSET) {
        gnw_h7b0_tim1_update_irq(s);
    }

    if (addr == GNW_H7B0_TIM1_CR1_OFFSET) {
        uint32_t new_cr1 = s->regs[addr >> 2];
        if ((new_cr1 & 0x1u) && !(old_value & 0x1u)) {
            gnw_h7b0_tim1_start_counting(s);
        } else if (!(new_cr1 & 0x1u)) {
            timer_del(s->count_timer);
        }
    } else if (was_counting &&
               (addr == GNW_H7B0_TIM1_CNT_OFFSET ||
                addr == GNW_H7B0_TIM1_PSC_OFFSET ||
                addr == GNW_H7B0_TIM1_ARR_OFFSET)) {
        gnw_h7b0_tim1_schedule(s, gnw_h7b0_tim1_kernel_hz(s));
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
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->count_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gnw_h7b0_tim1_timer_tick, s);
}

static const Property gnw_h7b0_tim1_properties[] = {
    DEFINE_PROP_BOOL("timing-mode", GnwH7B0Tim1State, timing_mode, false),
};

static int gnw_h7b0_tim1_pre_save(void *opaque)
{
    GnwH7B0Tim1State *s = opaque;

    gnw_h7b0_tim1_settle(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                          gnw_h7b0_tim1_kernel_hz(s));
    return 0;
}

static int gnw_h7b0_tim1_post_load(void *opaque, int version_id)
{
    GnwH7B0Tim1State *s = opaque;
    uint32_t psc = s->regs[GNW_H7B0_TIM1_PSC_OFFSET >> 2] &
                   GNW_H7B0_TIM1_COUNTER_MASK;

    if (version_id < 2) {
        s->cnt_base = s->regs[GNW_H7B0_TIM1_CNT_OFFSET >> 2] &
                      GNW_H7B0_TIM1_COUNTER_MASK;
        s->cnt_ref_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        s->kernel_fraction = 0;
        s->psc_remainder = 0;
        if (s->regs[GNW_H7B0_TIM1_CR1_OFFSET >> 2] & 0x1u) {
            gnw_h7b0_tim1_schedule(s, gnw_h7b0_tim1_kernel_hz(s));
        }
        gnw_h7b0_tim1_update_irq(s);
        return 0;
    }
    if (s->kernel_fraction >= NANOSECONDS_PER_SECOND ||
        s->psc_remainder > psc) {
        return -EINVAL;
    }
    if (s->timing_mode && icount_enabled()) {
        IcountCycleStamp stamp;

        icount_get_cycle_stamp(&stamp);
        if (version_id >= 3) {
            if (!gnw_h7b0_cycle_phase_stamp_equal(
                    &s->cycle_phase.stamp, &stamp)) {
                return -EINVAL;
            }
        } else if (!gnw_h7b0_cycle_phase_init(&s->cycle_phase, stamp)) {
            return -EINVAL;
        }
    }
    /*
     * Virtual-clock epochs are local to each qtest/migration destination;
     * pre_save settled all source elapsed time into the phase fields.
     */
    s->cnt_ref_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (s->timing_mode &&
        (s->regs[GNW_H7B0_TIM1_CR1_OFFSET >> 2] & 0x1u)) {
        gnw_h7b0_tim1_schedule(s, gnw_h7b0_tim1_kernel_hz(s));
    }
    gnw_h7b0_tim1_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_gnw_h7b0_cycle_phase = {
    .name = "gnw-h7b0-cycle-phase",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(stamp.modeled_cycles, GnwH7B0CyclePhase),
        VMSTATE_INT64(stamp.idle_ns, GnwH7B0CyclePhase),
        VMSTATE_UINT64(stamp.core_hz, GnwH7B0CyclePhase),
        VMSTATE_UINT64(stamp.generation, GnwH7B0CyclePhase),
        VMSTATE_UINT64(fraction_q64, GnwH7B0CyclePhase),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_gnw_h7b0_tim1 = {
    .name = TYPE_GNW_H7B0_TIM1,
    .version_id = 3,
    .minimum_version_id = 1,
    .pre_save = gnw_h7b0_tim1_pre_save,
    .post_load = gnw_h7b0_tim1_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0Tim1State, GNW_H7B0_TIM1_SIZE / 4),
        VMSTATE_TIMER_PTR_V(count_timer, GnwH7B0Tim1State, 2),
        VMSTATE_UINT64_V(cnt_ref_ns, GnwH7B0Tim1State, 2),
        VMSTATE_UINT64_V(kernel_fraction, GnwH7B0Tim1State, 2),
        VMSTATE_UINT32_V(psc_remainder, GnwH7B0Tim1State, 2),
        VMSTATE_UINT32_V(cnt_base, GnwH7B0Tim1State, 2),
        VMSTATE_VSTRUCT_V(cycle_phase, GnwH7B0Tim1State, 3,
                          vmstate_gnw_h7b0_cycle_phase,
                          GnwH7B0CyclePhase, 1),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_tim1_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_props(dc, gnw_h7b0_tim1_properties);
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
