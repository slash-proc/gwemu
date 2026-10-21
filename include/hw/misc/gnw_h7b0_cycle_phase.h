/*
 * Exact peripheral phase accounting from the icount cycle timeline.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_GNW_H7B0_CYCLE_PHASE_H
#define HW_MISC_GNW_H7B0_CYCLE_PHASE_H

#include <stdbool.h>
#include <stdint.h>

#include "exec/icount.h"

typedef struct GnwH7B0CyclePhase {
    IcountCycleStamp stamp;
    uint64_t fraction_q64;
} GnwH7B0CyclePhase;

static inline bool gnw_h7b0_cycle_phase_stamp_equal(
    const IcountCycleStamp *a, const IcountCycleStamp *b)
{
    return a->modeled_cycles == b->modeled_cycles &&
           a->idle_ns == b->idle_ns &&
           a->core_hz == b->core_hz &&
           a->generation == b->generation;
}

/* Split value * rate / denominator into whole ticks and a Q64 fraction. */
static inline bool gnw_h7b0_cycle_phase_scale(uint64_t value, uint64_t rate,
                                               uint64_t denominator,
                                               uint64_t *whole,
                                               uint64_t *fraction_q64)
{
    unsigned __int128 product = (unsigned __int128)value * rate;
    unsigned __int128 quotient = product / denominator;
    uint64_t remainder = product % denominator;

    if (quotient > UINT64_MAX) {
        return false;
    }
    *whole = quotient;
    *fraction_q64 = ((unsigned __int128)remainder << 64) / denominator;
    return true;
}

static inline bool gnw_h7b0_cycle_phase_init(GnwH7B0CyclePhase *state,
                                              IcountCycleStamp stamp)
{
    if (!stamp.core_hz) {
        return false;
    }
    state->stamp = stamp;
    state->fraction_q64 = 0;
    return true;
}

static inline bool gnw_h7b0_cycle_phase_settle(GnwH7B0CyclePhase *state,
                                                IcountCycleStamp now,
                                                uint64_t peripheral_hz,
                                                uint64_t *whole_ticks)
{
    uint64_t cycle_whole, cycle_fraction;
    uint64_t idle_whole, idle_fraction;
    uint64_t cycle_delta, idle_delta;
    unsigned __int128 whole, fraction;

    if (!whole_ticks || now.generation != state->stamp.generation ||
        now.core_hz != state->stamp.core_hz || !now.core_hz ||
        now.modeled_cycles < state->stamp.modeled_cycles ||
        now.idle_ns < state->stamp.idle_ns) {
        return false;
    }

    /* Unsigned subtraction avoids signed overflow across the full int64_t
     * coordinate range; the signed comparisons above establish monotonicity. */
    cycle_delta = (uint64_t)now.modeled_cycles -
                  (uint64_t)state->stamp.modeled_cycles;
    idle_delta = (uint64_t)now.idle_ns - (uint64_t)state->stamp.idle_ns;
    if (!peripheral_hz) {
        *whole_ticks = 0;
        state->stamp = now;
        return true;
    }
    if (!gnw_h7b0_cycle_phase_scale(cycle_delta, peripheral_hz,
                                     now.core_hz, &cycle_whole,
                                     &cycle_fraction) ||
        !gnw_h7b0_cycle_phase_scale(idle_delta, peripheral_hz,
                                     1000000000ULL, &idle_whole,
                                     &idle_fraction)) {
        return false;
    }

    whole = (unsigned __int128)cycle_whole + idle_whole;
    fraction = (unsigned __int128)state->fraction_q64 + cycle_fraction +
               idle_fraction;
    whole += fraction >> 64;
    if (whole > UINT64_MAX) {
        return false;
    }

    *whole_ticks = whole;
    state->fraction_q64 = fraction;
    state->stamp = now;
    return true;
}

static inline bool gnw_h7b0_cycle_phase_rebase(GnwH7B0CyclePhase *state,
                                                IcountCycleStamp before,
                                                IcountCycleStamp after)
{
    bool next_generation = before.generation != UINT64_MAX &&
                           after.generation == before.generation + 1;

    if (!gnw_h7b0_cycle_phase_stamp_equal(&state->stamp, &before) ||
        after.modeled_cycles != before.modeled_cycles ||
        after.idle_ns != before.idle_ns ||
        (!next_generation && after.generation != before.generation) ||
        (after.core_hz != before.core_hz && !next_generation) ||
        !after.core_hz) {
        return false;
    }
    state->stamp = after;
    return true;
}

#endif
