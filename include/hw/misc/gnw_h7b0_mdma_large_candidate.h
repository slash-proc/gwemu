/* Frozen, independently held-out MDMA large-transfer timing profile. */
#ifndef HW_MISC_GNW_H7B0_MDMA_LARGE_CANDIDATE_H
#define HW_MISC_GNW_H7B0_MDMA_LARGE_CANDIDATE_H

#include <stdbool.h>
#include <stdint.h>

#define GNW_MDMA_LARGE_CANDIDATE_REFERENCE_HZ UINT32_C(280000000)
#define GNW_MDMA_LARGE_CANDIDATE_SETUP_NUM UINT64_C(966656)
#define GNW_MDMA_LARGE_CANDIDATE_SLOPE_NUM UINT32_C(25725)
#define GNW_MDMA_LARGE_CANDIDATE_DEN UINT32_C(24576)
#define GNW_MDMA_LARGE_CANDIDATE_SERVICE_BYTES UINT32_C(128)
#define GNW_MDMA_LARGE_CANDIDATE_CHANNEL UINT32_C(15)
#define GNW_MDMA_LARGE_CANDIDATE_SOURCE UINT32_C(0x240a0000)
#define GNW_MDMA_LARGE_CANDIDATE_DESTINATION UINT32_C(0x240b0000)

/* The retained HOLDOUT and candidate replay authorize only this exact scope.
 * Their frozen CTCR_VALUE leaves BWM clear. */
#define GNW_MDMA_LARGE_CANDIDATE_RUNTIME_AUTHORIZED true

static inline bool gnw_mdma_large_candidate_size(uint32_t bytes)
{
    return bytes == 40960 || bytes == 45056 || bytes == 49152 ||
           bytes == 53248 || bytes == 57344;
}

static inline bool gnw_mdma_large_candidate_scope(uint32_t channel,
                                                   uint32_t source,
                                                   uint32_t destination,
                                                   uint32_t bytes,
                                                   uint32_t width,
                                                   uint32_t tlen,
                                                   bool source_increment,
                                                   bool destination_increment,
                                                   bool full_trigger,
                                                   bool bufferable,
                                                   uint32_t clock_hz)
{
    return channel == GNW_MDMA_LARGE_CANDIDATE_CHANNEL &&
           source == GNW_MDMA_LARGE_CANDIDATE_SOURCE &&
           destination == GNW_MDMA_LARGE_CANDIDATE_DESTINATION &&
           gnw_mdma_large_candidate_size(bytes) && width == 4 &&
           tlen == GNW_MDMA_LARGE_CANDIDATE_SERVICE_BYTES &&
           source_increment && destination_increment && full_trigger &&
           !bufferable &&
           clock_hz == GNW_MDMA_LARGE_CANDIDATE_REFERENCE_HZ;
}

static inline uint64_t gnw_mdma_large_candidate_numerator(uint32_t bytes)
{
    return GNW_MDMA_LARGE_CANDIDATE_SETUP_NUM +
           (uint64_t)bytes * GNW_MDMA_LARGE_CANDIDATE_SLOPE_NUM;
}

#endif
