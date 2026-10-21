/*
 * Experimental completed-instruction cycle producer.
 *
 * This interface is intentionally target-neutral.  A target begins one
 * instruction from generated code; cpu-exec finishes it only after the
 * one-instruction TB returns normally, or aborts it on unwind.
 */
#ifndef EXEC_GNW_CYCLE_MODEL_H
#define EXEC_GNW_CYCLE_MODEL_H

#include <string.h>
#include "qemu/typedefs.h"
#include "exec/gnw-m7-runtime-profile.h"

#define GNW_CYCLE_FP_SHIFT 32
#define GNW_CYCLE_FP_ONE (1ULL << GNW_CYCLE_FP_SHIFT)

/* FIT-only candidate from cpu-runtime-fetch-fit-v1-6a32e18861a3.
 * This identity remains experimental until runtime-generalization-v1 passes. */
#define GNW_M7_FETCH_RESIDUAL_PROFILE_OFF 0U
#define GNW_M7_FETCH_RESIDUAL_PROFILE_V1 1U
#define GNW_M7_FETCH_RESIDUAL_DENOMINATOR 7291066873ULL
#define GNW_M7_MEMORY_RESIDUAL_PROFILE_OFF 0U
#define GNW_M7_MEMORY_RESIDUAL_PROFILE_V1 0x4d525631U
#define GNW_M7_SCHEDULE_HISTORY_PROFILE_OFF 0U
#define GNW_M7_SCHEDULE_HISTORY_PROFILE_V4 0x53485634U
#define GNW_CYCLE_STORE_SLOTS 8
#define GNW_M7_DCACHE_LINE_SIZE 32
#define GNW_M7_DCACHE_WAYS 4
#define GNW_M7_DCACHE_SETS 128
#define GNW_M7_DCACHE_LINES \
    (GNW_M7_DCACHE_WAYS * GNW_M7_DCACHE_SETS)
#define GNW_M7_DCACHE_PENDING_EVENTS 4
#define GNW_M7_FLASH_FETCH_LINES 8
#define GNW_M7_FLASH_FETCH_PENDING_LINES 2
#define GNW_M7_INTERNAL_FLASH_BASE 0x08000000ULL
#define GNW_M7_INTERNAL_FLASH_END  0x08140000ULL
#define GNW_M7_XIP_BASE 0x90000000ULL
#define GNW_M7_XIP_END  0x94000000ULL
#define GNW_M7_XIP_UNSEEN_END 0x90004000ULL
#define GNW_M7_XIP_SERVICE_PROFILE_OFF 0
#define GNW_M7_XIP_SERVICE_PROFILE_TEST_FIXED 0x47585031U
#define GNW_M7_XIP_SERVICE_PROFILE_UNSEEN_V1 0x47585032U
#define GNW_M7_XIP_TEST_SERVICE_CYCLES 3
#define GNW_M7_XIP_UNSEEN_DENOMINATOR 13898703780000ULL
#define GNW_M7_XIP_UNSEEN_INTERCEPT_NUMERATOR 3620318767259610LL
#define GNW_M7_XIP_UNSEEN_BYTE_NUMERATOR 49874446988704LL
#define GNW_M7_XIP_UNSEEN_WINDOW_NUMERATOR 3223927450094494LL
#define GNW_M7_XIP_UNSEEN_OFFSET_NUMERATOR 83457642257405LL
#define GNW_M7_BUS_CONTENTION_PROFILE_OFF 0
#define GNW_M7_BUS_CONTENTION_PROFILE_TEST_FIXED 0x47424331U
#define GNW_M7_BUS_CONTENTION_PROFILE_MDMA_AXI1_V1 0x47424332U
#define GNW_M7_BUS_CONTENTION_PROFILE_V37_BWM_COMPARE 0x47424333U
#define GNW_M7_BUS_CONTENTION_PROFILE_V46_BWM_17_4 0x47424334U
#define GNW_M7_BUS_CONTENTION_TEST_CYCLES 2
#define GNW_M7_BUS_MASTER_MDMA  (1U << 0)
#define GNW_M7_BUS_MASTER_DMA2D (1U << 1)
#define GNW_M7_BUS_MASTER_MDMA_LARGE (1U << 2)
#define GNW_M7_BUS_MASTER_MDMA_BWM_COMPARE (1U << 3)
#define GNW_M7_BUS_CONTENTION_V37_BWM_COMPARE_NUMERATOR 9U
#define GNW_M7_BUS_CONTENTION_V37_BWM_COMPARE_DENOMINATOR 2U
/* V46 FIT-frozen, independently held-out clean-loop BWM coefficient. */
#define GNW_M7_BUS_CONTENTION_V46_BWM_NUMERATOR 17U
#define GNW_M7_BUS_CONTENTION_V46_BWM_DENOMINATOR 4U
/* Frozen topology-v7 same-subdivision FIT: 6.8037109375 cycles/load. */
#define GNW_M7_BUS_CONTENTION_MDMA_AXI1_NUMERATOR 6967U
#define GNW_M7_BUS_CONTENTION_MDMA_AXI1_DENOMINATOR 1024U
#define GNW_M7_BUS_CONTENTION_MDMA_AXI1_VICTIM_BASE 0x240c0000ULL
#define GNW_M7_BUS_CONTENTION_MDMA_AXI1_VICTIM_END  0x240d0000ULL
#define GNW_M7_BUS_CONTENTION_MDMA_AXI1_PMSA_ATTRS  0x08U
#define GNW_M7_LOAD_PATH_PROFILE_V5 0x4c503501U
#define GNW_M7_REGION_LOAD_LATENCY_DTCM 1U
#define GNW_M7_REGION_LOAD_LATENCY_AXI1 2U
#define GNW_M7_REGION_LOAD_LATENCY_DTCM_CYCLES 1U
#define GNW_M7_REGION_LOAD_LATENCY_AXI1_CYCLES 6U
#define GNW_M7_LOAD_PATH_CYCLES_FP 0x3001b7fc9ULL
#define GNW_M7_LOAD_PATH_MISS_CYCLES_FP 0x51a000000ULL
#define GNW_M7_CPU_RAM_ITCM_PROFILE_V1 0x43524931U
#define GNW_M7_CPU_RAM_ITCM_DEPENDENT_CYCLES_NUMERATOR 5U
#define GNW_M7_CPU_RAM_ITCM_DEPENDENT_CYCLES_DENOMINATOR 1U
#define GNW_M7_EXCEPTION_TRANSITION_PROFILE_V5_LEGACY 0x45585635U
#define GNW_M7_EXCEPTION_TRANSITION_PROFILE_V6 0x45585636U
/*
 * Incremental costs committed by the transition hook.  The ordinary issue
 * model already accounts for 8/14/20 cycles in the measured entry, return,
 * and tail-chain request bodies, respectively; adding the full physical
 * 27/26/31-cycle slopes here double-counts those cycles.
 */
#define GNW_M7_EXCEPTION_ENTRY_CYCLES 19U
#define GNW_M7_EXCEPTION_RETURN_CYCLES 12U
#define GNW_M7_EXCEPTION_TAIL_CHAIN_CYCLES 11U

/*
 * Stable packing for GnwCycleInsnMetadata.memory_attributes.  The typed
 * runtime observation remains authoritative; this compact form preserves the
 * completed instruction's physical-memory provenance in producer state.
 */
#define GNW_CYCLE_MEMATTR_PMSA_MASK       0x3fU
#define GNW_CYCLE_MEMATTR_SECURE          (1U << 6)
#define GNW_CYCLE_MEMATTR_USER            (1U << 7)
#define GNW_CYCLE_MEMATTR_REGION_SHIFT    8
#define GNW_CYCLE_MEMATTR_MMU_IDX_SHIFT   16
#define GNW_CYCLE_MEMATTR_SOURCE_SHIFT    24
#define GNW_CYCLE_MEMATTR_PMSA_VALID      (1U << 31)

typedef enum GnwCycleInsnClass {
    GNW_CYCLE_INSN_UNSUPPORTED,
    GNW_CYCLE_INSN_ALU,
    GNW_CYCLE_INSN_EARLY_ADD_SUB,
    GNW_CYCLE_INSN_MAC,
    GNW_CYCLE_INSN_NOP,
    GNW_CYCLE_INSN_BRANCH,
} GnwCycleInsnClass;

enum {
    GNW_CYCLE_META_SUPPORTED       = 1U << 0,
    GNW_CYCLE_META_BRANCH          = 1U << 1,
    GNW_CYCLE_META_DIRECT_BRANCH   = 1U << 2,
    GNW_CYCLE_META_CONDITIONAL     = 1U << 3,
    GNW_CYCLE_META_MEMORY          = 1U << 4,
    GNW_CYCLE_META_SYSTEM          = 1U << 5,
};

typedef enum GnwCycleMemoryKind {
    GNW_CYCLE_MEMORY_NONE,
    GNW_CYCLE_MEMORY_LOAD,
    GNW_CYCLE_MEMORY_STORE,
} GnwCycleMemoryKind;

typedef enum GnwCycleExceptionTransition {
    GNW_CYCLE_EXCEPTION_ENTRY,
    GNW_CYCLE_EXCEPTION_RETURN,
    GNW_CYCLE_EXCEPTION_TAIL_CHAIN,
    GNW_CYCLE_EXCEPTION_TRANSITION_COUNT,
} GnwCycleExceptionTransition;

enum {
    GNW_CYCLE_MEMORY_ACCESS_EXCLUSIVE = 1U << 0,
    GNW_CYCLE_MEMORY_ACCESS_CONDITIONAL_STORE = 1U << 1,
};

/*
 * Provenance for the PMSAv7 attributes attached to a successful access.
 * Only REGION carries a valid RASR TEX/S/C/B value and region number.  The
 * other values make the architectural fallback path explicit instead of
 * manufacturing an MPU region or memory attributes for it.
 */
typedef enum GnwCyclePMSASource {
    GNW_CYCLE_PMSA_NONE,
    GNW_CYCLE_PMSA_REGION,
    GNW_CYCLE_PMSA_DISABLED,
    GNW_CYCLE_PMSA_BACKGROUND,
    GNW_CYCLE_PMSA_PPB,
} GnwCyclePMSASource;

/* Exact PMSAv7 TEX/C/B/S encoding admitted by the current D-cache shadow. */
#define GNW_CYCLE_PMSA_ATTR_NORMAL_WBWA_NSH 0x0b

typedef struct GnwCycleMemoryObservation {
    uint64_t virtual_address;
    uint64_t physical_address;
    uint16_t address_reads;
    uint16_t store_data_reads;
    uint32_t memop;
    uint32_t mmu_idx;
    GnwCycleMemoryKind kind;
    uint8_t size;
    uint8_t access_index;
    uint8_t access_count;
    uint8_t access_flags;
    uint8_t pmsa_region;
    uint8_t pmsa_attrs;
    GnwCyclePMSASource pmsa_source;
    bool translation_valid;
    bool pmsa_valid;
    bool secure;
    bool user;
    bool store_succeeded;
    bool dcache_enabled;
} GnwCycleMemoryObservation;

/* A contiguous physical segment of a decoded instruction fetch. */
typedef struct GnwCycleFetchObservation {
    uint64_t virtual_address;
    uint64_t physical_address;
    uint8_t size;
    uint8_t bytes[4];
    uint8_t exact_body_region;
    uint8_t pmsa_region;
    uint8_t pmsa_attrs;
    GnwCyclePMSASource pmsa_source;
    bool pmsa_valid;
    bool known_cacheable;
    bool instruction_cache_enabled;
    bool secure;
    bool user;
    bool exact_body_region_state_verified;
    bool disfold_set;
    /* Execution-time argument snapshot used only by exact-kernel profiles. */
    uint32_t gpr0;
    uint32_t gpr1;
    uint32_t gpr2;
} GnwCycleFetchObservation;

/* Experimental 16 KiB / 32-byte / 2-way LRU I-cache shadow. */
#define GNW_M7_ICACHE_SETS 256U
#define GNW_M7_ICACHE_WAYS 2U
#define GNW_M7_ICACHE_PENDING_LINES 2U
#define GNW_M7_ICACHE_LINE_SIZE 32U
#define GNW_M7_ICACHE_MISS_CYCLES 4U
#define GNW_M7_ICACHE_CHARGE_BASE 0x240c0000ULL
#define GNW_M7_ICACHE_CHARGE_END  0x240c6000ULL
#define GNW_M7_ICACHE_PHYSICAL_PMSA_REGION 14U

typedef struct GnwM7ICachePendingLine {
    uint64_t line;
    bool charge_miss;
    bool instruction_cache_enabled;
    bool known_cacheable;
} GnwM7ICachePendingLine;

typedef struct GnwM7ICacheState {
    uint64_t tags[GNW_M7_ICACHE_SETS * GNW_M7_ICACHE_WAYS];
    uint8_t valid[GNW_M7_ICACHE_SETS];
    uint8_t lru_way[GNW_M7_ICACHE_SETS];
    GnwM7ICachePendingLine pending[GNW_M7_ICACHE_PENDING_LINES];
    uint64_t accesses, hits, misses, invalidations_all, invalidations_mva;
    uint32_t pending_cycles;
    uint8_t pending_count;
    bool enabled, attempt_active, pending_overflow;
    /* Disabled-fetch penalty de-duplicates repeated fetches from one AXI
     * line. This candidate is bounded to the measured WBWA AXI window. */
    uint64_t disabled_fetch_line;
    bool disabled_fetch_line_valid;
} GnwM7ICacheState;

#define GNW_M7_MEMORY_RESIDUAL_ITERATION_OFFSET 0x0eULL
#define GNW_M7_MEMORY_RESIDUAL_RETURN_OFFSET    0x24ULL
#define GNW_M7_MEMORY_RESIDUAL_BODY_SIZE        38U
#define GNW_M7_MEMORY_RESIDUAL_FETCH_BASE       0x24000000ULL
#define GNW_M7_MEMORY_RESIDUAL_FETCH_END        0x24100000ULL

static inline bool gnw_m7_memory_residual_fetch_provenance(
    const GnwCycleFetchObservation *observation)
{
    return observation->virtual_address == observation->physical_address &&
           observation->physical_address >=
               GNW_M7_MEMORY_RESIDUAL_FETCH_BASE &&
           observation->physical_address < GNW_M7_MEMORY_RESIDUAL_FETCH_END &&
           observation->pmsa_valid &&
           observation->pmsa_source == GNW_CYCLE_PMSA_REGION &&
           observation->pmsa_region == 3 && observation->pmsa_attrs == 0 &&
           !observation->known_cacheable;
}

static inline bool gnw_m7_memory_residual_entry_matches(
    const GnwCycleFetchObservation *observation)
{
    return gnw_m7_memory_residual_fetch_provenance(observation) &&
           observation->size == 2 && observation->bytes[0] == 0xf0 &&
           observation->bytes[1] == 0xb5;
}

static inline bool gnw_m7_memory_residual_body_matches(
    const uint8_t *body, size_t size)
{
    static const uint8_t expected[GNW_M7_MEMORY_RESIDUAL_BODY_SIZE] = {
        0xf0, 0xb5, 0x04, 0x46, 0x0d, 0x46, 0x16, 0x46,
        0x00, 0x20, 0x00, 0x23, 0x00, 0x27, 0x9b, 0x19,
        0xab, 0x42, 0x00, 0xd3, 0x5b, 0x1b, 0xe2, 0x5c,
        0x4f, 0xea, 0xf0, 0x60, 0x50, 0x40, 0x01, 0x37,
        0xaf, 0x42, 0xf4, 0xd1, 0xf0, 0xbd,
    };

    return size == sizeof(expected) && !memcmp(body, expected,
                                                sizeof(expected));
}

static inline bool gnw_m7_memory_residual_instruction_matches(
    const GnwCycleFetchObservation *observation, uint64_t entry_pc,
    uint64_t offset, uint8_t byte0, uint8_t byte1)
{
    return observation->virtual_address == entry_pc + offset &&
           gnw_m7_memory_residual_fetch_provenance(observation) &&
           observation->size == 2 && observation->bytes[0] == byte0 &&
           observation->bytes[1] == byte1;
}

typedef struct GnwM7FlashScheduleClassifierState {
    uint64_t accepted[2][2][2][2];
    uint64_t accepted_total;
    uint64_t rejected;
    uint8_t candidate;
    uint8_t next_offset;
    bool awaiting_back_edge;
    bool disfold_set;
} GnwM7FlashScheduleClassifierState;

static inline void gnw_flash_schedule_reject(
    GnwM7FlashScheduleClassifierState *state)
{
    if (state->candidate && state->rejected != UINT64_MAX) {
        state->rejected++;
    }
    state->candidate = 0;
    state->next_offset = 0;
    state->awaiting_back_edge = false;
    state->disfold_set = false;
}

/* Observe one completely translated instruction fetch in execution order.
 * A match is diagnostic only and is accepted after the exact loop body is
 * followed by its exact back edge. */
static inline void gnw_flash_schedule_observe(
    GnwM7FlashScheduleClassifierState *state,
    const GnwM7FlashScheduleIdentity
        identities[GNW_M7_FLASH_SCHEDULE_IDENTITIES],
    const GnwCycleFetchObservation *observation)
{
    const GnwM7FlashScheduleIdentity *identity;
    unsigned index, i;

retry:
    if (!state || !identities || !observation || !observation->size ||
        observation->size > sizeof(observation->bytes)) {
        return;
    }
    if (state->candidate) {
        identity = &identities[state->candidate - 1];
        if (state->awaiting_back_edge) {
            if (observation->virtual_address == identity->virtual_address &&
                observation->physical_address == identity->physical_address &&
                observation->disfold_set == state->disfold_set) {
                unsigned kind = identity->schedule_kind - 5;
                unsigned phase = identity->loop_start_phase == 6;
                unsigned disfold = state->disfold_set;
                unsigned role = identity->role;

                if (state->accepted[role][kind][phase][disfold] !=
                    UINT64_MAX) {
                    state->accepted[role][kind][phase][disfold]++;
                }
                if (state->accepted_total != UINT64_MAX) {
                    state->accepted_total++;
                }
                state->candidate = 0;
                state->next_offset = 0;
                state->awaiting_back_edge = false;
                state->disfold_set = false;
                goto retry;
            }
            gnw_flash_schedule_reject(state);
            return;
        }
        if (observation->disfold_set != state->disfold_set ||
            observation->virtual_address !=
                identity->virtual_address + state->next_offset ||
            observation->physical_address !=
                identity->physical_address + state->next_offset ||
            observation->size > GNW_M7_FLASH_SCHEDULE_BODY_BYTES -
                state->next_offset) {
            gnw_flash_schedule_reject(state);
            return;
        }
        for (i = 0; i < observation->size; i++) {
            if (observation->bytes[i] !=
                identity->bytes[state->next_offset + i]) {
                gnw_flash_schedule_reject(state);
                return;
            }
        }
        state->next_offset += observation->size;
        /* The registered schedule-6 body contains a taken 32-bit B.W at
         * offset 12. Its target is offset 18, so the halfword at 16 is data
         * in the sequential identity but is never fetched. Accept only that
         * exact virtual/physical transition, with DISFOLD unchanged. */
        if (identity->schedule_kind == 6 && state->next_offset == 16 &&
            observation->virtual_address == identity->virtual_address + 12 &&
            observation->physical_address == identity->physical_address + 12 &&
            observation->size == 4) {
            state->next_offset = 18;
        }
        state->awaiting_back_edge =
            state->next_offset == GNW_M7_FLASH_SCHEDULE_BODY_BYTES;
        return;
    }
    for (index = 0; index < GNW_M7_FLASH_SCHEDULE_IDENTITIES; index++) {
        identity = &identities[index];
        if (gnw_m7_flash_schedule_identity_valid(identity) &&
            observation->virtual_address == identity->virtual_address &&
            observation->physical_address == identity->physical_address) {
            state->candidate = index + 1;
            state->next_offset = 0;
            state->disfold_set = observation->disfold_set;
            goto retry;
        }
    }
}

/* Bridge only metadata the live fetch path actually observes.  The exact-loop
 * diagnostic classifier above records schedule observations, but it carries
 * no timing coefficient and cannot authorize a runtime profile. */
static inline GnwM7InternalFlashPhaseDiagnostic
gnw_flash_fetch_phase_diagnose(
    const GnwM7InternalFlashPhaseProfileCandidate *profile,
    const GnwCycleFetchObservation *observation, bool disfold_set)
{
    if (!observation) {
        return GNW_M7_INTERNAL_FLASH_PHASE_OUTSIDE_FLASH;
    }
    return gnw_m7_internal_flash_phase_diagnose_fetch(
        profile, observation->physical_address, observation->size,
        disfold_set, false, 0, 0);
}

/* Diagnostic adapter only: it cannot charge cycles or authorize a profile. */
static inline bool gnw_cycle_exact_body_match_fetch(
    GnwM7ExactBodyMatchState *state,
    const GnwM7ExactBodyIdentity *identity,
    const GnwCycleFetchObservation *observation)
{
    if (!state || !identity || !observation ||
        observation->exact_body_region != identity->region ||
        !observation->exact_body_region_state_verified) {
        if (state) {
            state->active = false;
            state->rejected = true;
        }
        return false;
    }
    return gnw_m7_exact_body_match_bytes(
        state, identity, observation->virtual_address,
        observation->physical_address, observation->bytes,
        observation->size);
}

typedef enum GnwCycleCacheMaintenanceOp {
    GNW_CYCLE_CACHE_ICIALLU,
    GNW_CYCLE_CACHE_ICIMVAU,
    GNW_CYCLE_CACHE_DCIMVAC,
    GNW_CYCLE_CACHE_DCISW,
    GNW_CYCLE_CACHE_DCCMVAU,
    GNW_CYCLE_CACHE_DCCMVAC,
    GNW_CYCLE_CACHE_DCCSW,
    GNW_CYCLE_CACHE_DCCIMVAC,
    GNW_CYCLE_CACHE_DCCISW,
    GNW_CYCLE_CACHE_BPIALL,
} GnwCycleCacheMaintenanceOp;

typedef struct GnwCycleCacheMaintenanceObservation {
    GnwCycleCacheMaintenanceOp op;
    uint32_t operand;
    bool instruction_cache_enabled;
    bool data_cache_enabled;
} GnwCycleCacheMaintenanceObservation;

typedef struct GnwCycleCacheControlObservation {
    bool old_instruction_cache_enabled;
    bool new_instruction_cache_enabled;
    bool old_data_cache_enabled;
    bool new_data_cache_enabled;
} GnwCycleCacheControlObservation;

/*
 * Zero-cost shadow policies.  Values other than OFF are explicit uncertainty
 * or replacement hypotheses; none claims to reproduce Cortex-M7 hardware.
 */
typedef enum GnwM7DCachePolicy {
    GNW_M7_DCACHE_OFF,
    GNW_M7_DCACHE_TAGS_NO_REPLACEMENT,
    GNW_M7_DCACHE_ROUND_ROBIN_HYPOTHESIS,
    GNW_M7_DCACHE_TREE_PLRU_HYPOTHESIS,
    GNW_M7_DCACHE_TRUE_LRU_HYPOTHESIS,
    GNW_M7_DCACHE_POLICY_COUNT,
} GnwM7DCachePolicy;

typedef struct GnwM7DCacheLine {
    uint64_t line_base;
    bool valid;
    bool dirty;
    uint8_t reserved[6];
} GnwM7DCacheLine;

typedef struct GnwM7DCacheSet {
    uint8_t replacement_state;
    uint8_t invalidated_way_mask;
    uint8_t cleaned_way_mask;
    uint8_t unclean_invalidated_way_mask;
    bool association_unknown;
    bool membership_unknown;
    uint8_t reserved[2];
} GnwM7DCacheSet;

typedef enum GnwM7DCachePendingType {
    GNW_M7_DCACHE_PENDING_MEMORY,
    GNW_M7_DCACHE_PENDING_MAINTENANCE,
    GNW_M7_DCACHE_PENDING_CONTROL,
} GnwM7DCachePendingType;

typedef struct GnwM7DCachePendingEvent {
    uint8_t type;
    uint8_t reserved[7];
    union {
        GnwCycleMemoryObservation memory;
        GnwCycleCacheMaintenanceObservation maintenance;
        GnwCycleCacheControlObservation control;
    } u;
} GnwM7DCachePendingEvent;

typedef struct GnwM7DCacheState {
    uint8_t policy;
    bool instruction_cache_enabled;
    bool data_cache_enabled;
    uint8_t pending_count;
    GnwM7DCacheLine lines[GNW_M7_DCACHE_LINES];
    GnwM7DCacheSet sets[GNW_M7_DCACHE_SETS];
    GnwM7DCachePendingEvent pending[GNW_M7_DCACHE_PENDING_EVENTS];

    uint64_t valid_lines;
    uint64_t dirty_lines;
    uint64_t uncertain_sets;
    uint64_t accesses;
    uint64_t hits;
    uint64_t misses;
    uint64_t allocations;
    uint64_t dirty_evictions;
    uint64_t ambiguous_accesses;
    uint64_t rejected_disabled;
    uint64_t rejected_path;
    uint64_t rejected_attributes;
    uint64_t dirty_discards;
    uint64_t maintenance[GNW_CYCLE_CACHE_BPIALL + 1];
    uint64_t external_events;
} GnwM7DCacheState;

/*
 * Diagnostic-only internal-flash fetch shadow.  It deliberately has no
 * timing coefficient: the small fully-associative tag store records exact
 * line and phase observations without pretending to model the M7 I-cache.
 * An attempt is staged until every byte translated, so a fault cannot seed
 * cache state.
 */
typedef struct GnwM7FlashFetchState {
    uint64_t lines[GNW_M7_FLASH_FETCH_LINES];
    uint64_t pending_lines[GNW_M7_FLASH_FETCH_PENDING_LINES];
    uint64_t phase_segments[8];
    uint64_t accesses;
    uint64_t hits;
    uint64_t misses;
    uint64_t bytes;
    uint64_t invalidations_all;
    uint64_t invalidations_mva;
    uint16_t pending_bytes;
    uint8_t valid_mask;
    uint8_t next_replace;
    uint8_t pending_count;
    uint8_t pending_phase_segments[8];
} GnwM7FlashFetchState;

static inline void gnw_flash_fetch_cancel(GnwM7FlashFetchState *f)
{
    unsigned i;

    f->pending_bytes = 0;
    f->pending_count = 0;
    for (i = 0; i < GNW_M7_FLASH_FETCH_PENDING_LINES; i++) {
        f->pending_lines[i] = 0;
    }
    for (i = 0; i < 8; i++) {
        f->pending_phase_segments[i] = 0;
    }
}

static inline void gnw_flash_fetch_observe(GnwM7FlashFetchState *f,
                                           uint64_t address, unsigned size)
{
    uint64_t first, last, line;

    if (!size || address < GNW_M7_INTERNAL_FLASH_BASE ||
        address >= GNW_M7_INTERNAL_FLASH_END ||
        size > GNW_M7_INTERNAL_FLASH_END - address) {
        return;
    }
    f->pending_bytes += size;
    if (f->pending_phase_segments[address & 7] != UINT8_MAX) {
        f->pending_phase_segments[address & 7]++;
    }
    first = address & ~(uint64_t)(GNW_M7_DCACHE_LINE_SIZE - 1);
    last = (address + size - 1) &
           ~(uint64_t)(GNW_M7_DCACHE_LINE_SIZE - 1);
    for (line = first; ; line += GNW_M7_DCACHE_LINE_SIZE) {
        unsigned i;

        for (i = 0; i < f->pending_count; i++) {
            if (f->pending_lines[i] == line) {
                break;
            }
        }
        if (i == f->pending_count &&
            f->pending_count < GNW_M7_FLASH_FETCH_PENDING_LINES) {
            f->pending_lines[f->pending_count++] = line;
        }
        if (line == last) {
            break;
        }
    }
}

static inline void gnw_flash_fetch_commit(GnwM7FlashFetchState *f)
{
    unsigned i;

    for (i = 0; i < f->pending_count; i++) {
        uint64_t line = f->pending_lines[i];
        unsigned slot;

        f->accesses++;
        for (slot = 0; slot < GNW_M7_FLASH_FETCH_LINES; slot++) {
            if ((f->valid_mask & (1U << slot)) && f->lines[slot] == line) {
                break;
            }
        }
        if (slot < GNW_M7_FLASH_FETCH_LINES) {
            f->hits++;
            continue;
        }
        f->misses++;
        for (slot = 0; slot < GNW_M7_FLASH_FETCH_LINES; slot++) {
            if (!(f->valid_mask & (1U << slot))) {
                break;
            }
        }
        if (slot == GNW_M7_FLASH_FETCH_LINES) {
            slot = f->next_replace;
            f->next_replace = (f->next_replace + 1) %
                              GNW_M7_FLASH_FETCH_LINES;
        }
        f->lines[slot] = line;
        f->valid_mask |= 1U << slot;
    }
    for (i = 0; i < 8; i++) {
        f->phase_segments[i] += f->pending_phase_segments[i];
    }
    f->bytes += f->pending_bytes;
    gnw_flash_fetch_cancel(f);
}

static inline void gnw_flash_fetch_invalidate(GnwM7FlashFetchState *f,
                                              bool all, uint64_t address)
{
    unsigned i;

    if (all) {
        for (i = 0; i < GNW_M7_FLASH_FETCH_LINES; i++) {
            f->lines[i] = 0;
        }
        f->valid_mask = 0;
        f->next_replace = 0;
        f->invalidations_all++;
        return;
    }
    address &= ~(uint64_t)(GNW_M7_DCACHE_LINE_SIZE - 1);
    for (i = 0; i < GNW_M7_FLASH_FETCH_LINES; i++) {
        if ((f->valid_mask & (1U << i)) && f->lines[i] == address) {
            f->lines[i] = 0;
            f->valid_mask &= ~(1U << i);
        }
    }
    f->invalidations_mva++;
}

enum {
    GNW_CYCLE_APSR_N = 1U << 0,
    GNW_CYCLE_APSR_Z = 1U << 1,
    GNW_CYCLE_APSR_C = 1U << 2,
    GNW_CYCLE_APSR_V = 1U << 3,
    GNW_CYCLE_APSR_NZCV = 0xf,
};

typedef enum GnwCycleFallbackReason {
    GNW_CYCLE_FALLBACK_NONE,
    GNW_CYCLE_FALLBACK_UNSUPPORTED_DECODE,
    GNW_CYCLE_FALLBACK_UNSUPPORTED_CLASS,
    GNW_CYCLE_FALLBACK_MEMORY,
    GNW_CYCLE_FALLBACK_SYSTEM,
    GNW_CYCLE_FALLBACK_PREDICATE_SKIP,
    GNW_CYCLE_FALLBACK_EXCEPTION_ATTEMPT,
    GNW_CYCLE_FALLBACK_COUNT,
} GnwCycleFallbackReason;

typedef struct GnwCycleInsnMetadata {
    uint64_t pc;
    uint32_t encoding;
    uint16_t gpr_reads;
    uint16_t gpr_writes;
    uint16_t store_data_reads;
    uint16_t address_reads;
    uint8_t size;
    uint8_t insn_class;
    uint8_t flags;
    uint8_t fallback_reason;
    uint8_t apsr_reads;
    uint8_t apsr_writes;
    uint8_t memory_kind;
    uint8_t memory_size;
    /* Region class for an eligible completed scalar word load. */
    uint8_t region_load_latency_class;
    uint64_t branch_target;
    uint64_t memory_address;
    uint32_t memory_attributes;
    /* Entry argument snapshot used only by exact-kernel profiles. */
    uint32_t argument_gpr0;
    bool branch_taken;
    uint8_t reserved[3];
} GnwCycleInsnMetadata;

/* Classify completed scalar word loads from the measured SRAM classes. The
 * candidate uses only successful, aligned, single-access loads with one GPR
 * destination; other transfer shapes and memory classes retain generic cost.
 */
static inline uint8_t gnw_m7_region_load_latency_class(
    const GnwCycleInsnMetadata *metadata,
    const GnwCycleMemoryObservation *observation)
{
    if (!observation->translation_valid ||
        observation->kind != GNW_CYCLE_MEMORY_LOAD ||
        observation->size != 4 || (observation->physical_address & 3) ||
        observation->access_count != 1 || observation->access_index != 0 ||
        observation->access_flags ||
        metadata->memory_kind != GNW_CYCLE_MEMORY_LOAD ||
        metadata->memory_size != 4 ||
        !(metadata->flags & GNW_CYCLE_META_MEMORY) ||
        (metadata->flags & GNW_CYCLE_META_SYSTEM) ||
        __builtin_popcount((unsigned)metadata->gpr_writes) != 1 ||
        (metadata->gpr_writes & (1U << 15))) {
        return 0;
    }
    if (observation->physical_address >= 0x20000000ULL &&
        observation->physical_address < 0x20020000ULL) {
        return GNW_M7_REGION_LOAD_LATENCY_DTCM;
    }
    if (observation->physical_address >= 0x24000000ULL &&
        observation->physical_address < 0x24100000ULL &&
        observation->pmsa_valid &&
        observation->pmsa_source == GNW_CYCLE_PMSA_REGION &&
        observation->pmsa_attrs == 0x08) {
        return GNW_M7_REGION_LOAD_LATENCY_AXI1;
    }
    return 0;
}

static inline uint8_t gnw_m7_region_load_latency_cycles(uint8_t region_class)
{
    if (region_class == GNW_M7_REGION_LOAD_LATENCY_DTCM) {
        return GNW_M7_REGION_LOAD_LATENCY_DTCM_CYCLES;
    }
    if (region_class == GNW_M7_REGION_LOAD_LATENCY_AXI1) {
        return GNW_M7_REGION_LOAD_LATENCY_AXI1_CYCLES;
    }
    return 3;
}

typedef struct GnwCycleStoreVisibility {
    uint64_t address;
    uint64_t ready_fp;
    uint32_t attributes;
    uint8_t valid;
    uint8_t reserved[3];
} GnwCycleStoreVisibility;

/*
 * Diagnostic replay of the provisional v5 register-offset LDR.W profile.
 * This state never contributes to cycle_fp or icount.  The fitted scale
 * absorbs omitted loop work and therefore must not be treated as an
 * architectural load latency.
 */
typedef struct GnwM7LoadPathState {
    uint64_t gpr_ready_fp[16];
    uint64_t issue_next_fp;
    uint64_t miss_slot_ready_fp;
    uint64_t critical_path_fp;
    uint64_t epoch_cycles;
    uint64_t last_issue_fp;
    uint64_t last_ready_fp;
    uint64_t accepted;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t rejected_shape;
    uint64_t rejected_region;
    uint32_t profile_identity;
    uint32_t reserved;
} GnwM7LoadPathState;

static inline bool gnw_m7_load_path_observation_eligible(
    const GnwCycleInsnMetadata *metadata,
    const GnwCycleMemoryObservation *observation)
{
    uint16_t first = metadata->encoding >> 16;
    uint16_t second = metadata->encoding;
    unsigned rn = first & 0xf;
    unsigned rt = (second >> 12) & 0xf;
    unsigned rm = second & 0xf;

    return observation->translation_valid &&
           observation->kind == GNW_CYCLE_MEMORY_LOAD &&
           observation->size == 4 &&
           !(observation->physical_address & 3) &&
           observation->access_count == 1 &&
           observation->access_index == 0 &&
           !observation->access_flags && metadata->size == 4 &&
           metadata->memory_size == 4 &&
           (metadata->flags & GNW_CYCLE_META_MEMORY) &&
           !(metadata->flags & GNW_CYCLE_META_SYSTEM) &&
           (first & 0xfff0) == 0xf850 &&
           !(second & 0x0fc0) &&
           rn != 15 && rt != 15 && rm != 15 &&
           metadata->address_reads == ((1U << rn) | (1U << rm)) &&
           metadata->gpr_writes == (1U << rt) &&
           __builtin_popcount((unsigned)metadata->address_reads) == 2 &&
           __builtin_popcount((unsigned)metadata->gpr_writes) == 1;
}

/* Exact dependent load emitted by the frozen topology-v7 payload:
 * `ldr r4, [r4, #0]`.  Its 16-bit decoder deliberately carries only generic
 * memory metadata, so keep this separate from the calibrated LDR.W load-path
 * profile and admit only the single retained instruction encoding. */
static inline bool gnw_m7_bus_contention_load_eligible(
    const GnwCycleInsnMetadata *metadata,
    const GnwCycleMemoryObservation *observation)
{
    return gnw_m7_load_path_observation_eligible(metadata, observation) ||
           (observation->translation_valid &&
            observation->kind == GNW_CYCLE_MEMORY_LOAD &&
            observation->size == 4 &&
            !(observation->physical_address & 3) &&
            observation->access_count == 1 &&
            observation->access_index == 0 &&
            !observation->access_flags && metadata->size == 2 &&
            metadata->encoding == UINT32_C(0x6824) &&
            (metadata->flags & GNW_CYCLE_META_MEMORY) &&
            !(metadata->flags & GNW_CYCLE_META_SYSTEM));
}

/* Comparison-only v37 BWM victim: only the exact T16 dependent LDR used by
 * the frozen synthetic FIT workload. Keep this narrower than the existing
 * generalized calibrated AXI1 load predicate. */
static inline bool gnw_m7_bus_contention_v37_bwm_load_eligible(
    const GnwCycleInsnMetadata *metadata,
    const GnwCycleMemoryObservation *observation)
{
    return observation->translation_valid &&
           observation->kind == GNW_CYCLE_MEMORY_LOAD &&
           observation->size == 4 &&
           !(observation->physical_address & 3) &&
           observation->physical_address >= 0x240c0000ULL &&
           observation->physical_address < 0x240d0000ULL &&
           observation->access_count == 1 &&
           observation->access_index == 0 &&
           !observation->access_flags && metadata->size == 2 &&
           metadata->encoding == UINT32_C(0x6824) &&
           (metadata->flags & GNW_CYCLE_META_MEMORY) &&
           !(metadata->flags & GNW_CYCLE_META_SYSTEM) &&
           metadata->address_reads == (1U << 4) &&
           metadata->gpr_writes == (1U << 4);
}

/* V46 validates this same exact dependent T16 load and AXI1 Normal-NC
 * region, with the coefficient selected from FIT-only data and tested on a
 * separate 13-cell hardware HOLDOUT. Keep its admission distinct from the
 * earlier 9/2 comparison profile. */
static inline bool gnw_m7_bus_contention_v46_bwm_load_eligible(
    const GnwCycleInsnMetadata *metadata,
    const GnwCycleMemoryObservation *observation)
{
    return gnw_m7_bus_contention_v37_bwm_load_eligible(metadata, observation) &&
           observation->pmsa_valid &&
           observation->pmsa_source == GNW_CYCLE_PMSA_REGION &&
           observation->pmsa_region == 7 &&
           observation->pmsa_attrs ==
               GNW_M7_BUS_CONTENTION_MDMA_AXI1_PMSA_ATTRS;
}

static inline void gnw_m7_load_path_rebase(GnwM7LoadPathState *s)
{
    unsigned i;

    if (s->critical_path_fp < (1ULL << 62)) {
        return;
    }
    s->epoch_cycles += (1ULL << 30);
    s->issue_next_fp = s->issue_next_fp >= (1ULL << 62) ?
        s->issue_next_fp - (1ULL << 62) : 0;
    s->miss_slot_ready_fp = s->miss_slot_ready_fp >= (1ULL << 62) ?
        s->miss_slot_ready_fp - (1ULL << 62) : 0;
    s->critical_path_fp -= 1ULL << 62;
    s->last_issue_fp = s->last_issue_fp >= (1ULL << 62) ?
        s->last_issue_fp - (1ULL << 62) : 0;
    s->last_ready_fp = s->last_ready_fp >= (1ULL << 62) ?
        s->last_ready_fp - (1ULL << 62) : 0;
    for (i = 0; i < 16; i++) {
        s->gpr_ready_fp[i] = s->gpr_ready_fp[i] >= (1ULL << 62) ?
            s->gpr_ready_fp[i] - (1ULL << 62) : 0;
    }
}

static inline bool gnw_m7_load_path_observe(GnwM7LoadPathState *s,
                                             uint16_t address_reads,
                                             uint16_t destination_writes,
                                             bool cacheable,
                                             bool cache_miss)
{
    uint64_t issue, ready;
    unsigned base, index, destination;

    if (s->profile_identity != GNW_M7_LOAD_PATH_PROFILE_V5 ||
        __builtin_popcount((unsigned)address_reads) != 2 ||
        __builtin_popcount((unsigned)destination_writes) != 1 ||
        (cache_miss && !cacheable)) {
        s->rejected_shape++;
        return false;
    }
    gnw_m7_load_path_rebase(s);
    base = __builtin_ctz((unsigned)address_reads);
    address_reads &= address_reads - 1;
    index = __builtin_ctz((unsigned)address_reads);
    destination = __builtin_ctz((unsigned)destination_writes);
    issue = s->issue_next_fp;
    if (issue < s->gpr_ready_fp[base]) {
        issue = s->gpr_ready_fp[base];
    }
    if (issue < s->gpr_ready_fp[index]) {
        issue = s->gpr_ready_fp[index];
    }
    if (cache_miss && issue < s->miss_slot_ready_fp) {
        issue = s->miss_slot_ready_fp;
    }
    ready = issue + GNW_M7_LOAD_PATH_CYCLES_FP +
            (cache_miss ? GNW_M7_LOAD_PATH_MISS_CYCLES_FP : 0);
    s->gpr_ready_fp[destination] = ready;
    s->last_issue_fp = issue;
    s->last_ready_fp = ready;
    s->issue_next_fp = issue + GNW_CYCLE_FP_ONE;
    if (cache_miss) {
        s->miss_slot_ready_fp = ready;
        s->cache_misses++;
    } else if (cacheable) {
        s->cache_hits++;
    }
    if (s->critical_path_fp < ready) {
        s->critical_path_fp = ready;
    }
    s->accepted++;
    return true;
}

typedef struct GnwCycleModelState {
    bool enabled;
    bool pending_valid;
    uint8_t reserved[6];
    GnwCycleInsnMetadata pending;
    GnwCycleInsnMetadata previous;
    uint64_t epoch_cycles;
    uint64_t cycle_fp;
    uint64_t previous_issue_fp;
    uint64_t gpr_ready_fp[16];
    uint64_t flags_ready_fp[4];
    uint64_t branch_flags_ready_fp[4];
    uint64_t commit_fraction_fp;
    GnwCycleStoreVisibility store_visible[GNW_CYCLE_STORE_SLOTS];
    uint64_t completed_instructions;
    uint64_t modeled_whole_cycles;
    uint64_t issued_pairs;
    uint64_t folded_nops;
    uint64_t aborted_attempts;
    /* Diagnostic-only fetch observations; never affect modeled timing. */
    uint64_t instruction_fetch_attempts;
    uint64_t instruction_fetch_segments;
    uint64_t instruction_fetch_bytes;
    uint64_t instruction_fetch_incomplete;
    uint64_t nc_axi_fetch_line;
    uint64_t nc_axi_fetch_transitions;
    uint32_t pending_nc_axi_fetch_cycles;
    uint32_t pending_icache_maintenance_cycles;
    bool nc_axi_fetch_valid;
    /* One synchronous translation attempt rollback snapshot; never migrated. */
    uint64_t nc_axi_fetch_saved_line;
    uint64_t nc_axi_fetch_saved_transitions;
    uint32_t nc_axi_fetch_saved_cycles;
    bool nc_axi_fetch_saved_valid;
    bool nc_axi_fetch_attempt_active;
    GnwM7FlashFetchState flash_fetch;
    /* Transient diagnostic classifier; no timing or authorization effect. */
    GnwM7FlashScheduleIdentity
        flash_schedule_identity[GNW_M7_FLASH_SCHEDULE_IDENTITIES];
    GnwM7FlashScheduleClassifierState flash_schedule;
    GnwM7FlashScheduleClassifierState flash_schedule_pending;
    bool flash_schedule_configured;
    bool flash_schedule_attempt_pending;
    /* Transient diagnostics: deliberately excluded from VMState. */
    GnwM7ExactBodyIdentity exact_body_identity;
    GnwM7ExactBodyMatchState exact_body_match;
    uint64_t exact_body_accepted;
    uint64_t exact_body_rejected;
    bool exact_body_identity_configured;
    /* Hardware-held-out admission for one exact ITCM body only. */
    uint32_t cpu_ram_itcm_profile_identity;
    uint64_t cpu_ram_itcm_profile_accepted;
    uint64_t cpu_ram_itcm_profile_rejected;
    /* Opt-in coefficients from the independently held-out v5 campaign. */
    uint32_t exception_transition_profile_identity;
    uint64_t exception_transition_count[GNW_CYCLE_EXCEPTION_TRANSITION_COUNT];
    /* Exact matched-kernel-v5 AXI fetch residual candidate. */
    uint32_t fetch_residual_profile_identity;
    uint64_t fetch_residual_fraction;
    uint64_t fetch_residual_charges;
    uint64_t fetch_residual_cycles;
    uint32_t pending_fetch_residual_cycles;
    bool fetch_residual_set_conflict_active;
    /* One instruction-fetch attempt rollback snapshot; never migrated. */
    uint64_t fetch_residual_saved_fraction;
    uint64_t fetch_residual_saved_charges;
    uint64_t fetch_residual_saved_cycles;
    uint32_t fetch_residual_saved_pending_cycles;
    bool fetch_residual_saved_set_conflict_active;
    bool fetch_residual_attempt_active;
    /* Exact gnw_memory_u8 residual derived from retained matched-v5 FIT. */
    uint32_t memory_residual_profile_identity;
    uint64_t memory_residual_entry_pc;
    uint32_t memory_residual_base;
    uint32_t memory_residual_bytes;
    uint32_t memory_residual_stride;
    uint32_t memory_residual_remaining;
    uint32_t pending_memory_residual_cycles;
    uint64_t memory_residual_charges;
    uint64_t memory_residual_cycles;
    uint64_t memory_residual_accepted;
    uint64_t memory_residual_rejected;
    bool memory_residual_active;
    uint64_t memory_residual_saved_entry_pc;
    uint32_t memory_residual_saved_base;
    uint32_t memory_residual_saved_bytes;
    uint32_t memory_residual_saved_stride;
    uint32_t memory_residual_saved_remaining;
    uint32_t memory_residual_saved_pending_cycles;
    uint64_t memory_residual_saved_charges;
    uint64_t memory_residual_saved_cycles;
    uint64_t memory_residual_saved_accepted;
    uint64_t memory_residual_saved_rejected;
    bool memory_residual_saved_active;
    /* FIT-validated history residual for two exact matched-v5 kernels. */
    uint32_t schedule_history_profile_identity;
    uint32_t schedule_history_outcomes;
    uint32_t schedule_history_credit_cycles;
    uint32_t schedule_history_applied_credit_cycles;
    uint32_t schedule_history_compensation_cycles;
    uint64_t schedule_history_accepted;
    uint64_t schedule_history_rejected;
    uint8_t schedule_history_pattern;
    uint8_t schedule_history_kind;
    bool schedule_history_active;
    /* Hidden mechanism test only; no hardware timing meaning. */
    uint64_t xip_service_cursor_fp;
    uint64_t xip_service_requests;
    uint64_t xip_service_cycles;
    uint64_t xip_service_remaining_cycles;
    bool xip_service_deferred_finish;
    /* Experimental v1 XIP fit: only a contiguous byte-load stream under the
     * exact measured OSPI/RCC/no-contention predicate may accrue cycles. */
    uint64_t xip_unseen_next_address;
    uint64_t xip_unseen_fraction;
    uint64_t xip_unseen_qualified_bytes;
    uint64_t xip_unseen_streams;
    bool xip_unseen_stream_active;
    bool (*xip_unseen_scope_probe)(void *opaque);
    void *xip_unseen_scope_opaque;
    /* Hidden mechanism test only; no hardware arbitration meaning. */
    uint64_t bus_contention_observations;
    uint64_t bus_contention_matches;
    uint64_t bus_contention_cycles;
    uint32_t bus_contention_fraction;
    uint64_t pending_bus_contention_cycles;
    uint32_t (*bus_contention_probe)(void *opaque, uint64_t address,
                                     unsigned size, int64_t guest_time_ns);
    void *bus_contention_probe_opaque;
    uint64_t fallback[GNW_CYCLE_FALLBACK_COUNT];
    uint32_t config_identity[8];
    uint32_t implementation_version;
    uint32_t xip_service_profile_identity;
    uint32_t bus_contention_profile_identity;
    /* Not migrated until a reviewed admission protocol exists.  OFF is the
     * sole valid state and is reconstructed by the existing zeroed reset. */
    GnwM7ProductionFlashProfileState production_flash_profiles;
    bool previous_valid;
    bool previous_was_store;
    bool disfold_set;
    bool test_near_rebase;
    /* Clear issue state after an in-flight exception-boundary instruction. */
    bool exception_scoreboard_flush_pending;
    GnwM7LoadPathState load_path;
    GnwM7DCacheState dcache;
    GnwM7ICacheState icache;
} GnwCycleModelState;

static inline void gnw_m7_icache_clear(GnwM7ICacheState *s)
{
    memset(s->tags, 0, sizeof(s->tags));
    memset(s->valid, 0, sizeof(s->valid));
    memset(s->lru_way, 0, sizeof(s->lru_way));
    s->disabled_fetch_line = 0;
    s->disabled_fetch_line_valid = false;
}

static inline void gnw_m7_icache_end_disabled_fetch_run(GnwM7ICacheState *s)
{
    s->disabled_fetch_line = 0;
    s->disabled_fetch_line_valid = false;
}

static inline void gnw_m7_icache_invalidate_line(GnwM7ICacheState *s,
                                                  uint64_t address)
{
    uint64_t line = address & ~(uint64_t)(GNW_M7_ICACHE_LINE_SIZE - 1);
    unsigned set = (line / GNW_M7_ICACHE_LINE_SIZE) % GNW_M7_ICACHE_SETS;
    unsigned way;
    for (way = 0; way < GNW_M7_ICACHE_WAYS; way++) {
        unsigned index = set * GNW_M7_ICACHE_WAYS + way;
        if ((s->valid[set] & (1U << way)) && s->tags[index] == line) {
            s->valid[set] &= ~(1U << way);
            s->tags[index] = 0;
            if (s->invalidations_mva != UINT64_MAX) s->invalidations_mva++;
            break;
        }
    }
}

static inline void gnw_m7_icache_invalidate_all(GnwM7ICacheState *s)
{
    gnw_m7_icache_clear(s);
    if (s->invalidations_all != UINT64_MAX) s->invalidations_all++;
}

static inline void gnw_m7_icache_stage_fetch(
    GnwM7ICacheState *ic, const GnwCycleFetchObservation *o)
{
    uint64_t first, last, line;
    bool charge;
    if (!ic->enabled) return;
    if (!o->size) {
        if (ic->pending_count < GNW_M7_ICACHE_PENDING_LINES) {
            ic->pending[ic->pending_count++] = (GnwM7ICachePendingLine) {
                .instruction_cache_enabled = o->instruction_cache_enabled,
            };
        } else {
            ic->pending_overflow = true;
        }
        return;
    }
    first = o->physical_address & ~(uint64_t)(GNW_M7_ICACHE_LINE_SIZE - 1);
    last = (o->physical_address + o->size - 1) &
           ~(uint64_t)(GNW_M7_ICACHE_LINE_SIZE - 1);
    charge = o->known_cacheable &&
        o->virtual_address == o->physical_address && o->pmsa_valid &&
        o->pmsa_source == GNW_CYCLE_PMSA_REGION &&
        o->pmsa_attrs == GNW_CYCLE_PMSA_ATTR_NORMAL_WBWA_NSH &&
        first >= GNW_M7_ICACHE_CHARGE_BASE &&
        last < GNW_M7_ICACHE_CHARGE_END;
    for (line = first;; line += GNW_M7_ICACHE_LINE_SIZE) {
        unsigned i;
        for (i = 0; i < ic->pending_count; i++) {
            if (ic->pending[i].line == line &&
                ic->pending[i].charge_miss == charge &&
                ic->pending[i].instruction_cache_enabled ==
                    o->instruction_cache_enabled &&
                ic->pending[i].known_cacheable == o->known_cacheable) {
                ic->pending[i].charge_miss |= charge;
                break;
            }
        }
        if (i == ic->pending_count) {
            if (ic->pending_count < GNW_M7_ICACHE_PENDING_LINES) {
                ic->pending[ic->pending_count++] = (GnwM7ICachePendingLine) {
                    .line = line, .charge_miss = charge,
                    .instruction_cache_enabled =
                        o->instruction_cache_enabled,
                    .known_cacheable = o->known_cacheable,
                };
            } else {
                ic->pending_overflow = true;
            }
        }
        if (line == last) break;
    }
}

static inline bool gnw_m7_icache_touch(GnwM7ICacheState *s,
                                       uint64_t line, bool charge)
{
    unsigned set = (line / GNW_M7_ICACHE_LINE_SIZE) % GNW_M7_ICACHE_SETS;
    unsigned way, victim;
    uint8_t mask = s->valid[set];
    bool hit = false;

    for (way = 0; way < GNW_M7_ICACHE_WAYS; way++) {
        unsigned index = set * GNW_M7_ICACHE_WAYS + way;
        if ((mask & (1U << way)) && s->tags[index] == line) {
            hit = true;
            s->lru_way[set] = way ^ 1U;
            break;
        }
    }
    if (s->accesses != UINT64_MAX) s->accesses++;
    if (hit) {
        if (s->hits != UINT64_MAX) s->hits++;
        return false;
    }
    if (s->misses != UINT64_MAX) s->misses++;
    if ((mask & 1U) == 0) victim = 0;
    else if ((mask & 2U) == 0) victim = 1;
    else victim = s->lru_way[set];
    s->tags[set * GNW_M7_ICACHE_WAYS + victim] = line;
    s->valid[set] = mask | (1U << victim);
    s->lru_way[set] = victim ^ 1U;
    if (charge) {
        if (s->pending_cycles <= UINT32_MAX -
                                 GNW_M7_ICACHE_MISS_CYCLES) {
            s->pending_cycles += GNW_M7_ICACHE_MISS_CYCLES;
        } else {
            s->pending_cycles = UINT32_MAX;
        }
    }
    return true;
}

static inline void gnw_m7_icache_finish_attempt(GnwM7ICacheState *s,
                                                 bool commit)
{
    unsigned i;
    if (commit && !s->pending_overflow) {
        for (i = 0; i < s->pending_count; i++) {
            const GnwM7ICachePendingLine *pending = &s->pending[i];

            if (pending->instruction_cache_enabled) {
                s->disabled_fetch_line_valid = false;
                if (!pending->known_cacheable) {
                    continue;
                }
                gnw_m7_icache_touch(s, s->pending[i].line,
                                    s->pending[i].charge_miss);
            } else if (pending->charge_miss) {
                if (!s->disabled_fetch_line_valid ||
                    s->disabled_fetch_line != pending->line) {
                    if (s->pending_cycles <= UINT32_MAX -
                                             GNW_M7_ICACHE_MISS_CYCLES) {
                        s->pending_cycles += GNW_M7_ICACHE_MISS_CYCLES;
                    } else {
                        s->pending_cycles = UINT32_MAX;
                    }
                }
                s->disabled_fetch_line = pending->line;
                s->disabled_fetch_line_valid = true;
            } else {
                s->disabled_fetch_line_valid = false;
            }
        }
    } else if (commit) {
        /* An unobserved fetch sequence cannot safely preserve line continuity. */
        s->disabled_fetch_line_valid = false;
    }
    memset(s->pending, 0, sizeof(s->pending));
    s->pending_count = 0;
    s->attempt_active = false;
    s->pending_overflow = false;
}

static inline bool gnw_m7_icache_state_valid(const GnwM7ICacheState *s,
                                              bool expected_enabled)
{
    unsigned set, way;
    if (s->enabled != expected_enabled || s->pending_count ||
        s->attempt_active || s->pending_overflow || s->pending_cycles ||
        (s->disabled_fetch_line_valid &&
         ((s->disabled_fetch_line & (GNW_M7_ICACHE_LINE_SIZE - 1)) ||
          s->disabled_fetch_line < GNW_M7_ICACHE_CHARGE_BASE ||
          s->disabled_fetch_line >= GNW_M7_ICACHE_CHARGE_END)) ||
        s->hits > s->accesses || s->misses != s->accesses - s->hits ||
        s->pending_count > GNW_M7_ICACHE_PENDING_LINES) return false;
    for (set = 0; set < GNW_M7_ICACHE_SETS; set++) {
        if ((s->valid[set] & ~3U) || s->lru_way[set] > 1) return false;
        for (way = 0; way < GNW_M7_ICACHE_WAYS; way++) {
            uint64_t tag = s->tags[set * GNW_M7_ICACHE_WAYS + way];
            if (!(s->valid[set] & (1U << way)) && tag) return false;
            if ((s->valid[set] & (1U << way)) &&
                ((tag & (GNW_M7_ICACHE_LINE_SIZE - 1)) ||
                 ((tag / GNW_M7_ICACHE_LINE_SIZE) % GNW_M7_ICACHE_SETS) != set))
                return false;
        }
        if ((s->valid[set] & 3U) == 3U &&
            s->tags[set * GNW_M7_ICACHE_WAYS] ==
            s->tags[set * GNW_M7_ICACHE_WAYS + 1]) return false;
    }
    return true;
}

static inline bool gnw_cycle_exact_body_diagnostic_valid(
    const GnwCycleModelState *s)
{
    static const GnwM7ExactBodyIdentity zero_identity;
    static const GnwM7ExactBodyMatchState zero_match;
    const GnwM7ExactBodyMatchState *match = &s->exact_body_match;

    if (!s->exact_body_identity_configured) {
        return !memcmp(&s->exact_body_identity, &zero_identity,
                       sizeof(zero_identity)) &&
               !memcmp(match, &zero_match, sizeof(zero_match)) &&
               !s->exact_body_accepted && !s->exact_body_rejected;
    }
    if (!gnw_m7_exact_body_identity_valid(&s->exact_body_identity) ||
        (match->active && match->rejected) ||
        match->next_offset > s->exact_body_identity.size) {
        return false;
    }
    if (!match->active && !match->rejected && !match->next_offset) {
        return true;
    }
    if (match->rejected && !match->next_offset) {
        return !memcmp(match->artifact_sha256,
                       s->exact_body_identity.artifact_sha256,
                       GNW_M7_RUNTIME_DIGEST_BYTES) ||
               !memcmp(match, &(GnwM7ExactBodyMatchState) {
                   .rejected = true,
               }, sizeof(*match));
    }
    if (!match->active && !match->rejected &&
        match->next_offset != s->exact_body_identity.size) {
        return false;
    }
    return match->region_state_verified &&
           match->region == s->exact_body_identity.region &&
           !memcmp(match->artifact_sha256,
                   s->exact_body_identity.artifact_sha256,
                   GNW_M7_RUNTIME_DIGEST_BYTES);
}

/* Scalar invariants required before accepting migrated producer state. */
static inline bool gnw_cycle_model_migration_scalars_valid(
    const GnwCycleModelState *s)
{
    const GnwM7LoadPathState *load = &s->load_path;
    unsigned i;

    if (!gnw_cycle_exact_body_diagnostic_valid(s) ||
        !gnw_m7_icache_state_valid(&s->icache, s->enabled) ||
        !gnw_m7_production_flash_profiles_off(
            &s->production_flash_profiles) ||
        s->commit_fraction_fp >= GNW_CYCLE_FP_ONE ||
        (s->previous_valid && s->previous_issue_fp > s->cycle_fp) ||
        (load->profile_identity != 0 &&
         load->profile_identity != GNW_M7_LOAD_PATH_PROFILE_V5) ||
        load->critical_path_fp < load->issue_next_fp ||
        load->critical_path_fp < load->miss_slot_ready_fp ||
        load->last_issue_fp > load->last_ready_fp ||
        load->last_ready_fp > load->critical_path_fp ||
        load->cache_hits > load->accepted ||
        load->cache_misses > load->accepted - load->cache_hits ||
        s->flash_fetch.pending_count >
            GNW_M7_FLASH_FETCH_PENDING_LINES ||
        s->flash_fetch.valid_mask >> GNW_M7_FLASH_FETCH_LINES ||
        s->flash_fetch.next_replace >= GNW_M7_FLASH_FETCH_LINES ||
        s->flash_fetch.hits > s->flash_fetch.accesses ||
        s->flash_fetch.misses >
            s->flash_fetch.accesses - s->flash_fetch.hits) {
        return false;
    }
    for (i = 0; i < sizeof(load->gpr_ready_fp) /
                    sizeof(load->gpr_ready_fp[0]); i++) {
        if (load->gpr_ready_fp[i] > load->critical_path_fp) {
            return false;
        }
    }
    if ((s->xip_service_profile_identity != GNW_M7_XIP_SERVICE_PROFILE_OFF &&
         s->xip_service_profile_identity !=
             GNW_M7_XIP_SERVICE_PROFILE_TEST_FIXED &&
         s->xip_service_profile_identity !=
             GNW_M7_XIP_SERVICE_PROFILE_UNSEEN_V1) ||
        (s->xip_service_profile_identity == GNW_M7_XIP_SERVICE_PROFILE_OFF &&
         (s->xip_service_cursor_fp || s->xip_service_requests ||
          s->xip_service_cycles || s->xip_service_remaining_cycles ||
          s->xip_service_deferred_finish || s->xip_unseen_next_address ||
          s->xip_unseen_fraction || s->xip_unseen_qualified_bytes ||
          s->xip_unseen_streams || s->xip_unseen_stream_active)) ||
        s->xip_service_remaining_cycles > s->xip_service_cycles) {
        return false;
    }
    if (s->xip_service_profile_identity ==
            GNW_M7_XIP_SERVICE_PROFILE_TEST_FIXED) {
        const uint64_t service_cycles = GNW_M7_XIP_TEST_SERVICE_CYCLES;

        if (s->xip_service_requests > UINT64_MAX / service_cycles ||
            s->xip_service_cycles !=
                s->xip_service_requests * service_cycles ||
            s->xip_unseen_next_address || s->xip_unseen_fraction ||
            s->xip_unseen_qualified_bytes || s->xip_unseen_streams ||
            s->xip_unseen_stream_active) {
            return false;
        }
    }
    if (s->xip_service_profile_identity ==
            GNW_M7_XIP_SERVICE_PROFILE_UNSEEN_V1 &&
        (s->xip_service_requests != s->xip_unseen_qualified_bytes ||
         s->xip_unseen_fraction >= GNW_M7_XIP_UNSEEN_DENOMINATOR ||
         (s->xip_unseen_stream_active &&
          (s->xip_unseen_next_address < GNW_M7_XIP_BASE ||
          s->xip_unseen_next_address >= GNW_M7_XIP_UNSEEN_END)) ||
         (!s->xip_unseen_stream_active && s->xip_unseen_next_address))) {
        return false;
    }
    if ((s->bus_contention_profile_identity !=
             GNW_M7_BUS_CONTENTION_PROFILE_OFF &&
         s->bus_contention_profile_identity != GNW_M7_BUS_CONTENTION_PROFILE_TEST_FIXED &&
         s->bus_contention_profile_identity != GNW_M7_BUS_CONTENTION_PROFILE_MDMA_AXI1_V1 &&
         s->bus_contention_profile_identity != GNW_M7_BUS_CONTENTION_PROFILE_V37_BWM_COMPARE &&
         s->bus_contention_profile_identity != GNW_M7_BUS_CONTENTION_PROFILE_V46_BWM_17_4) ||
        s->bus_contention_matches > s->bus_contention_observations ||
        s->bus_contention_matches >
            UINT64_MAX / GNW_M7_BUS_CONTENTION_TEST_CYCLES ||
        (s->bus_contention_profile_identity ==
             GNW_M7_BUS_CONTENTION_PROFILE_OFF &&
         (s->bus_contention_observations || s->bus_contention_matches ||
          s->bus_contention_cycles || s->bus_contention_fraction)) ||
        s->pending_bus_contention_cycles) {
        return false;
    }
    if (s->bus_contention_profile_identity == GNW_M7_BUS_CONTENTION_PROFILE_TEST_FIXED &&
        (s->bus_contention_matches > UINT64_MAX / GNW_M7_BUS_CONTENTION_TEST_CYCLES ||
         s->bus_contention_cycles != s->bus_contention_matches * GNW_M7_BUS_CONTENTION_TEST_CYCLES ||
         s->bus_contention_fraction)) return false;
    if (s->bus_contention_profile_identity == GNW_M7_BUS_CONTENTION_PROFILE_MDMA_AXI1_V1) {
        if (s->bus_contention_matches > UINT64_MAX / GNW_M7_BUS_CONTENTION_MDMA_AXI1_NUMERATOR) return false;
        uint64_t total=s->bus_contention_matches * GNW_M7_BUS_CONTENTION_MDMA_AXI1_NUMERATOR;
        if (s->bus_contention_cycles != total / GNW_M7_BUS_CONTENTION_MDMA_AXI1_DENOMINATOR ||
            s->bus_contention_fraction != total % GNW_M7_BUS_CONTENTION_MDMA_AXI1_DENOMINATOR) return false;
    }
    if (s->bus_contention_profile_identity ==
            GNW_M7_BUS_CONTENTION_PROFILE_V37_BWM_COMPARE &&
        s->bus_contention_matches > UINT64_MAX /
            GNW_M7_BUS_CONTENTION_V37_BWM_COMPARE_NUMERATOR) {
        return false;
    }
    if (s->bus_contention_profile_identity ==
            GNW_M7_BUS_CONTENTION_PROFILE_V37_BWM_COMPARE) {
        uint64_t total = s->bus_contention_matches *
            GNW_M7_BUS_CONTENTION_V37_BWM_COMPARE_NUMERATOR;

        if (s->bus_contention_cycles != total /
                GNW_M7_BUS_CONTENTION_V37_BWM_COMPARE_DENOMINATOR ||
            s->bus_contention_fraction != total %
                GNW_M7_BUS_CONTENTION_V37_BWM_COMPARE_DENOMINATOR) {
            return false;
        }
    }
    if (s->bus_contention_profile_identity ==
            GNW_M7_BUS_CONTENTION_PROFILE_V46_BWM_17_4) {
        uint64_t total;

        if (s->bus_contention_matches > UINT64_MAX /
            GNW_M7_BUS_CONTENTION_V46_BWM_NUMERATOR) {
            return false;
        }
        total = s->bus_contention_matches *
            GNW_M7_BUS_CONTENTION_V46_BWM_NUMERATOR;
        if (s->bus_contention_cycles != total /
                GNW_M7_BUS_CONTENTION_V46_BWM_DENOMINATOR ||
            s->bus_contention_fraction != total %
                GNW_M7_BUS_CONTENTION_V46_BWM_DENOMINATOR) {
            return false;
        }
    }
    return true;
}

void gnw_cycle_model_enable(CPUState *cpu, bool test_near_rebase,
                            GnwM7DCachePolicy dcache_policy,
                            bool xip_service_test_profile,
                            bool xip_service_unseen_v1_profile,
                            bool bus_contention_test_profile,
                            bool bus_contention_mdma_axi1_profile,
                            bool bus_contention_v37_bwm_comparison,
                            bool bus_contention_v46_bwm_17_4,
                            bool cpu_ram_itcm_profile,
                            bool exception_transition_profile,
                            bool fetch_residual_profile,
                            bool memory_residual_profile,
                            bool schedule_history_profile,
                            bool load_path_candidate);
void gnw_cycle_model_set_bus_contention_probe(
    CPUState *cpu,
    uint32_t (*probe)(void *opaque, uint64_t address, unsigned size,
                      int64_t guest_time_ns),
    void *opaque);
void gnw_cycle_model_set_xip_unseen_scope_probe(
    CPUState *cpu, bool (*probe)(void *opaque), void *opaque);
bool gnw_cycle_model_set_exact_body_diagnostic(
    CPUState *cpu, const GnwM7ExactBodyIdentity *identity);
bool gnw_cycle_model_set_flash_schedule_diagnostic(
    CPUState *cpu, const GnwM7FlashScheduleIdentity
        identities[GNW_M7_FLASH_SCHEDULE_IDENTITIES]);
bool gnw_cycle_model_set_flash_schedule_diagnostic_artifact(
    CPUState *cpu, const char *identity_sha256);
void gnw_cycle_model_reset(CPUState *cpu);
void gnw_cycle_model_begin(CPUState *cpu,
                           const GnwCycleInsnMetadata *metadata,
                           bool disfold_set);
void gnw_cycle_model_refine(CPUState *cpu,
                            const GnwCycleInsnMetadata *metadata);
void gnw_cycle_model_memory(CPUState *cpu,
                            const GnwCycleMemoryObservation *observation);
/* Record one completed data access immediately; does not finish the insn. */
void gnw_cycle_model_memory_completed(
    CPUState *cpu, const GnwCycleMemoryObservation *observation);
/* Record a successful execution-time instruction-fetch segment immediately. */
void gnw_cycle_model_instruction_fetch_attempt(CPUState *cpu);
void gnw_cycle_model_instruction_fetch(
    CPUState *cpu, const GnwCycleFetchObservation *observation);
void gnw_cycle_model_instruction_fetch_complete(CPUState *cpu);
void gnw_cycle_model_instruction_fetch_incomplete(CPUState *cpu);
void gnw_cycle_model_cache_maintenance(
    CPUState *cpu, const GnwCycleCacheMaintenanceObservation *observation);
void gnw_cycle_model_cache_control(
    CPUState *cpu, const GnwCycleCacheControlObservation *observation);
void gnw_cycle_model_finish(CPUState *cpu);
void gnw_cycle_model_abort(CPUState *cpu, bool attempt_counted);
bool gnw_cycle_model_enabled(const CPUState *cpu);
bool gnw_cycle_model_identity_valid(const CPUState *cpu);
bool gnw_cycle_model_cpu_ram_itcm_profile_valid(const CPUState *cpu,
                                                 bool enabled);
bool gnw_cycle_model_exception_transition_profile_valid(const CPUState *cpu,
                                                         bool enabled);
bool gnw_cycle_model_fetch_residual_profile_valid(const CPUState *cpu,
                                                   bool enabled);
bool gnw_cycle_model_memory_residual_profile_valid(const CPUState *cpu,
                                                    bool enabled);
bool gnw_cycle_model_schedule_history_profile_valid(const CPUState *cpu,
                                                     bool enabled);
bool gnw_cycle_model_exception_transition(
    CPUState *cpu, GnwCycleExceptionTransition transition);
bool gnw_cycle_model_xip_service_profile_valid(const CPUState *cpu);
bool gnw_cycle_model_xip_service_pending(const CPUState *cpu);
bool gnw_cycle_model_xip_service_needs_defer(const CPUState *cpu);
bool gnw_cycle_model_drain_xip_service(CPUState *cpu);
void gnw_cycle_model_defer_finish(CPUState *cpu);

#endif
