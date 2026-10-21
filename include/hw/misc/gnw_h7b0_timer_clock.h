/*
 * STM32H7 timer kernel clock selection shared by RCC and its regression.
 */
#ifndef HW_MISC_GNW_H7B0_TIMER_CLOCK_H
#define HW_MISC_GNW_H7B0_TIMER_CLOCK_H

#include <stdbool.h>
#include <limits.h>
#include <stdint.h>

#define GNW_H7B0_TIM1_COUNTER_MASK 0xffffU
#define GNW_H7B0_TIM2_PRESCALER_MASK 0xffffU
#define GNW_H7B0_TIMER_NS_PER_SECOND 1000000000ULL

/* Compute floor(value * multiplier / divisor), saturating at INT64_MAX.
 * The quotient/remainder split keeps both intermediate products in range:
 * divisor and multiplier are uint32_t, so remainder * multiplier fits u64. */
static inline uint64_t gnw_h7b0_timer_muldiv_ns_sat(
    uint64_t value, uint32_t multiplier, uint32_t divisor)
{
    uint64_t quotient, remainder, whole, fraction;

    if (!divisor || !multiplier) {
        return divisor ? 0 : INT64_MAX;
    }

    quotient = value / divisor;
    remainder = value % divisor;
    if (quotient > (uint64_t)INT64_MAX / multiplier) {
        return INT64_MAX;
    }
    whole = quotient * multiplier;
    fraction = (remainder * multiplier) / divisor;
    if (fraction > (uint64_t)INT64_MAX - whole) {
        return INT64_MAX;
    }
    return whole + fraction;
}

/*
 * Convert elapsed virtual nanoseconds into whole timer-kernel cycles while
 * retaining the sub-cycle numerator.  Keeping the remainder in units of
 * 1/NANOSECONDS_PER_SECOND of a cycle lets a live RCC rate change settle the
 * old interval exactly, then continue at the new rate without losing phase.
 */
static inline uint64_t gnw_h7b0_timer_elapsed_kernel_cycles(
    uint64_t elapsed_ns, uint32_t kernel_hz, uint64_t *fraction)
{
    uint64_t seconds = elapsed_ns / GNW_H7B0_TIMER_NS_PER_SECOND;
    uint64_t nanoseconds = elapsed_ns % GNW_H7B0_TIMER_NS_PER_SECOND;
    uint64_t subsecond = nanoseconds * kernel_hz + *fraction;
    uint64_t cycles;

    *fraction = subsecond % GNW_H7B0_TIMER_NS_PER_SECOND;
    subsecond /= GNW_H7B0_TIMER_NS_PER_SECOND;
    if (kernel_hz && seconds > (UINT64_MAX - subsecond) / kernel_hz) {
        return UINT64_MAX;
    }
    cycles = seconds * kernel_hz + subsecond;
    return cycles;
}

/*
 * Earliest integral-nanosecond delay that supplies kernel_cycles, accounting
 * for an already accumulated sub-cycle numerator.  Timer deadlines are
 * signed, so calculations that cannot be represented saturate at INT64_MAX.
 */
static inline uint64_t gnw_h7b0_timer_kernel_cycles_to_ns_ceil(
    uint64_t kernel_cycles, uint64_t fraction, uint32_t kernel_hz)
{
    uint64_t quotient, remainder, partial, whole;

    if (!kernel_hz) {
        return INT64_MAX;
    }
    if (!kernel_cycles) {
        return 0;
    }
    fraction %= GNW_H7B0_TIMER_NS_PER_SECOND;
    quotient = kernel_cycles / kernel_hz;
    remainder = kernel_cycles % kernel_hz;

    if (remainder * GNW_H7B0_TIMER_NS_PER_SECOND < fraction) {
        /*
         * A sub-cycle fraction is smaller than one full kernel cycle, so
         * this borrow can only occur once quotient is nonzero.
         */
        quotient--;
        remainder += kernel_hz;
    }
    if (quotient > (uint64_t)INT64_MAX /
                   GNW_H7B0_TIMER_NS_PER_SECOND) {
        return INT64_MAX;
    }
    whole = quotient * GNW_H7B0_TIMER_NS_PER_SECOND;
    partial = remainder * GNW_H7B0_TIMER_NS_PER_SECOND - fraction;
    partial = partial / kernel_hz + (partial % kernel_hz != 0);
    if (partial > (uint64_t)INT64_MAX - whole) {
        return INT64_MAX;
    }
    return whole + partial;
}

/* Convert an integer kernel-cycle remainder minus its Q64 phase to a
 * saturating virtual-time deadline.  Splitting the division before applying
 * the fractional correction keeps the intermediate within 128 bits even
 * for TIM2's 48-bit maximum cycle remainder. */
static inline uint64_t gnw_h7b0_timer_kernel_q64_to_ns_ceil(
    uint64_t whole_cycles, uint64_t fraction_q64, uint32_t kernel_hz)
{
    unsigned __int128 numerator, quotient;
    uint64_t remainder;

    if (!kernel_hz) {
        return (uint64_t)INT64_MAX;
    }
    numerator = (unsigned __int128)whole_cycles *
                GNW_H7B0_TIMER_NS_PER_SECOND;
    quotient = numerator / kernel_hz;
    remainder = numerator % kernel_hz;
    if (((unsigned __int128)remainder << 64) >
        (unsigned __int128)fraction_q64 *
            GNW_H7B0_TIMER_NS_PER_SECOND) {
        quotient++;
    }
    return quotient > INT64_MAX ? (uint64_t)INT64_MAX : quotient;
}

/* Add an unsigned interval to a signed virtual-clock timestamp without
 * invoking signed overflow. QEMU timer deadlines cannot exceed INT64_MAX. */
static inline int64_t gnw_h7b0_timer_deadline_sat(int64_t now,
                                                  uint64_t interval)
{
    uint64_t until_zero;

    if (now >= 0) {
        if (interval > (uint64_t)(INT64_MAX - now)) {
            return INT64_MAX;
        }
        return now + (int64_t)interval;
    }

    /* This form also handles INT64_MIN without negating it directly. */
    until_zero = (uint64_t)(-(now + 1)) + 1;
    if (interval < until_zero) {
        return now + (int64_t)interval;
    }
    interval -= until_zero;
    return interval > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)interval;
}

/* Decode the selected APB PPRE field and RM0455 CFGR.TIMPRE rule. */
static inline uint32_t gnw_h7b0_timer_kernel_hz(
    uint32_t hclk, uint32_t cfgr, uint32_t cdcfgr2, unsigned ppre_shift)
{
    uint32_t ppre = (cdcfgr2 >> ppre_shift) & 0x7;
    bool timpre = (cfgr >> 15) & 0x1;
    uint32_t div = (ppre & 0x4) ? (2u << (ppre & 0x3)) : 1;

    if (!timpre) {
        return div == 1 ? hclk : (hclk / div) * 2;
    }
    return div <= 4 ? hclk : (hclk / div) * 4;
}

/* TIM1 is a 16-bit counter on H7B0 (TIM2/TIM5 are the 32-bit instances).
 * Apply implemented widths before incrementing or calculating a period. */
static inline uint64_t gnw_h7b0_tim1_period_from_regs_ns(
    uint32_t arr, uint32_t psc, uint32_t kernel_hz)
{
    uint64_t ticks = ((uint64_t)(arr & GNW_H7B0_TIM1_COUNTER_MASK) + 1) *
                     ((uint64_t)(psc & GNW_H7B0_TIM1_COUNTER_MASK) + 1);

    return gnw_h7b0_timer_muldiv_ns_sat(ticks, 1000000000U, kernel_hz);
}

/* Keep the legacy scheduling guardrails in default mode. Timing mode keeps
 * the computed virtual-clock deadline, including sub-microsecond and long
 * periods, so host scheduling does not silently change guest timer cadence. */
static inline uint64_t gnw_h7b0_timer_apply_period_limits(
    uint64_t period_ns, bool timing_mode, uint64_t min_period_ns,
    uint64_t max_period_ns)
{
    if (timing_mode) {
        /* QEMU deadlines have nanosecond resolution. A rounded-zero
         * interval must still advance time rather than reschedule forever. */
        return period_ns ? period_ns : 1;
    }
    if (period_ns < min_period_ns) {
        period_ns = min_period_ns;
    }
    if (period_ns > max_period_ns) {
        period_ns = max_period_ns;
    }
    return period_ns;
}

#endif
