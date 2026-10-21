/*
 * Pure signed-rational affine evaluation for reviewed M7 profile consumers.
 *
 * This helper admits no profile and charges no cycles.  A future consumer may
 * call it only after its own scope, observation, and receipt checks.  In
 * particular, a fitted per-loop unit is not a generic instruction CPI.
 */
#ifndef EXEC_GNW_M7_AFFINE_EVAL_H
#define EXEC_GNW_M7_AFFINE_EVAL_H

#include <stdbool.h>
#include <stdint.h>

#include "exec/gnw-m7-runtime-profile.h"

typedef enum GnwM7AffineEvalStatus {
    GNW_M7_AFFINE_EVAL_VALID,
    GNW_M7_AFFINE_EVAL_INVALID_COEFFICIENT,
    GNW_M7_AFFINE_EVAL_ARITHMETIC_OVERFLOW,
    GNW_M7_AFFINE_EVAL_OUTPUT_OVERFLOW,
    GNW_M7_AFFINE_EVAL_NEGATIVE_CLAMPED_INVALID,
} GnwM7AffineEvalStatus;

typedef struct GnwM7AffineEvalResult {
    uint64_t cycles;
    GnwM7AffineEvalStatus status;
} GnwM7AffineEvalResult;

static inline uint64_t gnw_m7_affine_gcd_u64(uint64_t a, uint64_t b)
{
    while (b) {
        uint64_t remainder = a % b;

        a = b;
        b = remainder;
    }
    return a;
}

/*
 * Evaluate setup + slope * units exactly, then round to the nearest cycle.
 * Exact half-cycle ties round upward.  This makes evaluation independent of
 * host floating-point mode.  All intermediate overflow is detected.
 *
 * A negative exact total returns zero with NEGATIVE_CLAMPED_INVALID.  Zero is
 * therefore never ambiguous: callers must inspect status before consuming it.
 */
static inline GnwM7AffineEvalResult gnw_m7_affine_eval(
    GnwM7LinearTimingTerm term, uint64_t units)
{
    GnwM7AffineEvalResult result = { 0, GNW_M7_AFFINE_EVAL_VALID };
    uint64_t setup_den = term.setup_cycles.denominator;
    uint64_t slope_den = term.cycles_per_unit.denominator;
    uint64_t divisor;
    __int128 setup_scaled, slope_product, slope_scaled, numerator;
    __uint128_t denominator, positive, quotient, remainder, half;

    if (!setup_den || !slope_den) {
        result.status = GNW_M7_AFFINE_EVAL_INVALID_COEFFICIENT;
        return result;
    }

    divisor = gnw_m7_affine_gcd_u64(setup_den, slope_den);
    denominator = (__uint128_t)(setup_den / divisor) * slope_den;

    if (__builtin_mul_overflow((__int128)term.setup_cycles.numerator,
                               (__int128)(slope_den / divisor),
                               &setup_scaled) ||
        __builtin_mul_overflow((__int128)term.cycles_per_unit.numerator,
                               (__int128)units, &slope_product) ||
        __builtin_mul_overflow(slope_product,
                               (__int128)(setup_den / divisor),
                               &slope_scaled) ||
        __builtin_add_overflow(setup_scaled, slope_scaled, &numerator)) {
        result.status = GNW_M7_AFFINE_EVAL_ARITHMETIC_OVERFLOW;
        return result;
    }

    if (numerator < 0) {
        result.status = GNW_M7_AFFINE_EVAL_NEGATIVE_CLAMPED_INVALID;
        return result;
    }

    positive = (__uint128_t)numerator;
    quotient = positive / denominator;
    remainder = positive % denominator;
    half = denominator / 2 + (denominator & 1);
    if (remainder >= half) {
        quotient++;
    }
    if (quotient > UINT64_MAX) {
        result.status = GNW_M7_AFFINE_EVAL_OUTPUT_OVERFLOW;
        return result;
    }
    result.cycles = (uint64_t)quotient;
    return result;
}

#endif /* EXEC_GNW_M7_AFFINE_EVAL_H */
