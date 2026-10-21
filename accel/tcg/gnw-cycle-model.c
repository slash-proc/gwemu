/*
 * Experimental Cortex-M7 completed-instruction cycle producer.
 *
 * This is deliberately hidden behind an x- property.  Its configuration is
 * not integration-eligible and it must not be confused with the baseline
 * one-instruction-per-cycle clock mechanics.  That baseline is deterministic;
 * it is not a claim of hardware timing accuracy.
 */
#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "hw/core/cpu.h"
#include "exec/gnw-cycle-model.h"
#include "exec/memop.h"

static void xip_unseen_end_stream(GnwCycleModelState *s);
#include "exec/icount.h"
#include "qemu/timer.h"
#include "trace.h"

#define GNW_CYCLE_MODEL_IMPLEMENTATION_VERSION 25
#define GNW_CYCLE_REBASE_FP (1ULL << 62)
#define GNW_CYCLE_REBASE_CYCLES \
    (GNW_CYCLE_REBASE_FP >> GNW_CYCLE_FP_SHIFT)

/*
 * SHA-256 of:
 * gnw-h7b0-experimental-c-cycle-model-v25;dcache=zero-cost;
 * geometry=16384/32/4/128;eligibility=axisram1+xip-pmsa-region-raw0b;
 * state=clean-sweep-v2;
 * nc-axi-fetch=32-byte-last-line-4-cycles-per-transition;
 * logical-t16=and-eor-nz;load-result=3-cycles;
 * v39-dependent-t16-load-candidate=dtcm-1;axi1-6;fallback-3-cycles;
 * v40-loop-controls=t16-cbz+cbranch+cmp-imm8+cmp-reg16+cmp-high;
 * axi1-nc-load-completion=one-final-issue-slot-overlap;
 * exception-boundary=scoreboard-flush;icache-disabled-run=exception-break;
 * taken-branch-refill=one-cycle-from-issue;
 * icache-all-maintenance-enabled=89-cycles;
 * icache=16k/32/2way-lru;miss=4;disabled-fetch=per-line-run-once-no-tag;charge=idmap-pmsa-wbwa-240c0000-240c6000;
 * policy=<name>;xip-service=off;
 * range=90000000-94000000
 */
static const uint32_t gnw_cycle_model_config_identity[][8] = {
    [GNW_M7_DCACHE_OFF] = {
        0x7097f43a, 0xf9e9377a, 0x7fb7e838, 0xe63f6ae7, 0xa60bc0e4, 0xd2717052, 0xe13cbe92, 0x45eec51d,
    },
    [GNW_M7_DCACHE_TAGS_NO_REPLACEMENT] = {
        0x5f3d52e2, 0xfca1e553, 0x07c62be4, 0x31325ace, 0x6c9b1629, 0x5aed6a5c, 0x065eca7d, 0x937ec8b9,
    },
    [GNW_M7_DCACHE_ROUND_ROBIN_HYPOTHESIS] = {
        0xab15de4b, 0x36426fa7, 0xd71c564d, 0x7e99527c, 0x056be227, 0x0186316c, 0x1f458c66, 0x0b30a42f,
    },
    [GNW_M7_DCACHE_TREE_PLRU_HYPOTHESIS] = {
        0x6b614581, 0xc10c4bbc, 0x986286af, 0x7406ca58, 0xd534b319, 0x2100f588, 0x501fdd74, 0x0893cbb0,
    },
    [GNW_M7_DCACHE_TRUE_LRU_HYPOTHESIS] = {
        0xe76662ca, 0x6736b2ac, 0x6565bc1b, 0x0c5dff74, 0x21995478, 0x0cb0b5ad, 0xab94b32d, 0x35c3a2a3,
    },
};

/*
 * SHA-256 of:
 * gnw-h7b0-experimental-c-cycle-model-v25;dcache=zero-cost;
 * geometry=16384/32/4/128;eligibility=axisram1+xip-pmsa-region-raw0b;
 * state=clean-sweep-v2;
 * nc-axi-fetch=32-byte-last-line-4-cycles-per-transition;
 * logical-t16=and-eor-nz;load-result=3-cycles;
 * v39-dependent-t16-load-candidate=dtcm-1;axi1-6;fallback-3-cycles;
 * v40-loop-controls=t16-cbz+cbranch+cmp-imm8+cmp-reg16+cmp-high;
 * axi1-nc-load-completion=one-final-issue-slot-overlap;
 * exception-boundary=scoreboard-flush;icache-disabled-run=exception-break;
 * taken-branch-refill=one-cycle-from-issue;
 * icache-all-maintenance-enabled=89-cycles;
 * icache=16k/32/2way-lru;miss=4;disabled-fetch=per-line-run-once-no-tag;charge=idmap-pmsa-wbwa-240c0000-240c6000;
 * policy=<name>;
 * xip-service=test-fixed-3-cycles-per-new-cacheable-data-line;
 * unknown-xip-service=one-per-logical-access;
 * range=90000000-94000000
 *
 * An observed segment is only a mechanism-test request.  Segment splitting
 * is not a calibrated representation of physical OCTOSPI transactions.
 */
static const uint32_t gnw_cycle_model_xip_test_config_identity[][8] = {
    [GNW_M7_DCACHE_OFF] = {
        0xbbc02793, 0x07aff2e3, 0xb52590d9, 0x471b1aeb, 0x42d2a39a, 0x74e251ee, 0x61e6b1eb, 0xff5d0257,
    },
    [GNW_M7_DCACHE_TAGS_NO_REPLACEMENT] = {
        0x4709cea8, 0x2fe84e91, 0xd5210376, 0xb803318a, 0x0ef6f6d9, 0xe5083fe3, 0xb3ad6c52, 0xd20dc20c,
    },
    [GNW_M7_DCACHE_ROUND_ROBIN_HYPOTHESIS] = {
        0x95bd88f0, 0xc7af8d84, 0x49e6a5b6, 0xce3c72a5, 0xf52e90bc, 0x2af97974, 0xe4cab9a8, 0xd685f237,
    },
    [GNW_M7_DCACHE_TREE_PLRU_HYPOTHESIS] = {
        0x34a6554b, 0xe11d6f25, 0x853270b4, 0x142a53fa, 0x6399b0b8, 0x023dba84, 0x4a7c0c54, 0x2621e4d5,
    },
    [GNW_M7_DCACHE_TRUE_LRU_HYPOTHESIS] = {
        0x6278f67c, 0xb659b559, 0xd6501f3c, 0x9ee7255e, 0x61f7b423, 0xefd0cc70, 0x3a3438e5, 0x9c24767c,
    },
};

#define GNW_M7_AXISRAM1_BASE 0x24000000ULL
#define GNW_M7_AXISRAM1_END  0x24100000ULL
#define GNW_M7_NC_AXI_FETCH_LINE_SIZE 32ULL
#define GNW_M7_NC_AXI_FETCH_TRANSITION_CYCLES 4U
#define GNW_M7_ICIALLU_CYCLES 89U
#define GNW_M7_LRU_RESET_STATE 0xe4

/* Exact FIT source:
 * cpu-runtime-fetch-fit-v1-6a32e18861a3/analysis.json
 * QEMU SHA-256 53774163e997b2c97523b70269bcdbf4f223876198757e7efd9c04d0667ae313.
 * The profile is deliberately restricted to the matched-kernel-v5 AXI
 * addresses, fetched bytes, and region-3 noncacheable PMSA geometry. */
#define GNW_FETCH_HOT_ENTRY             0x240101c0ULL
#define GNW_FETCH_PERIODIC_ENTRY        0x240101e0ULL
#define GNW_FETCH_PERIODIC_ITERATION    0x240101e8ULL
#define GNW_FETCH_PERIODIC_INVALIDATION 0x240101f4ULL
#define GNW_FETCH_SET_ENTRY             0x24010220ULL
#define GNW_FETCH_SET_ITERATION         0x2401022cULL
#define GNW_FETCH_SET_RETURN            0x24010246ULL
#define GNW_FETCH_HOT_SHARED_CYCLES     41U
#define GNW_FETCH_PERIODIC_ENTRY_CYCLES 45U
#define GNW_FETCH_PERIODIC_ITER_CYCLES  2U
#define GNW_FETCH_PERIODIC_INV_CYCLES   24U
#define GNW_FETCH_SET_ENTRY_NUMERATOR   362698445474ULL
#define GNW_FETCH_SET_ITER_NUMERATOR    448400615980ULL

/*
 * Retained-source closure:
 * docs/timing-calibration/cpu-matched-kernel-v5/evidence/
 * 2026-10-16-rpi400-hw-01/{fit,freeze,holdout,score}.json
 * kernels.bin SHA-256
 * 28ca6be07944b36bca2c1be3ea871b6b6307f5c673d75d205b6fddbda1c7659b.
 *
 * Coefficients are derived only from the eight retained FIT rows.  The four
 * independently held-out v5 rows validate the same closed forms but do not
 * contribute parameters.  This remains an opt-in exact-kernel profile.
 */
#define GNW_MEMORY_MIN_BYTES      28672U
#define GNW_MEMORY_MAX_BYTES      61440U
#define GNW_MEMORY_DTCM_BASE      0x20004000U
#define GNW_MEMORY_AXI_BASE       0x24030000U
#define GNW_MEMORY_AHB_FIT_BASE   0x30000000U
#define GNW_MEMORY_AHB_HOLD_BASE  0x30001000U

/*
 * FIT-only residual from cpu-schedule-history-v4-fit-09d22bd93cec.
 * Values use a common denominator of 24 and apply only to the exact
 * matched-v5 AXI kernels for 4096 iterations.  HOLDOUT was not consumed.
 */
#define GNW_SCHEDULE_REGION_ENTRY     0x24010260ULL
#define GNW_SCHEDULE_REGION_DECISION  0x2401027eULL
#define GNW_SCHEDULE_REGION_RETURN    0x24010294ULL
#define GNW_SCHEDULE_OPERATION_ENTRY  0x240102a0ULL
#define GNW_SCHEDULE_OPERATION_DECISION 0x240102beULL
#define GNW_SCHEDULE_OPERATION_RETURN 0x240102d0ULL
#define GNW_SCHEDULE_ITERATIONS       4096U
#define GNW_SCHEDULE_COEFFICIENT_DENOMINATOR 24
static const int32_t gnw_schedule_region_history8_numerator[256] = {
    [0x00] = -42888,
    [0x01] = -336924,
    [0x03] = -207900,
    [0x05] = -330780,
    [0x07] = -214044,
    [0x09] = -330780,
    [0x0b] = -152592,
    [0x0d] = -201756,
    [0x0f] = -207888,
    [0x11] = -165378,
    [0x13] = -201744,
    [0x15] = -324612,
    [0x17] = -158736,
    [0x19] = -201768,
    [0x1b] = -146448,
    [0x1d] = -207876,
    [0x1f] = -201732,
    [0x25] = -324636,
    [0x27] = -207888,
    [0x2b] = -97296,
    [0x2d] = -146436,
    [0x2f] = -152592,
    [0x33] = -36354,
    [0x35] = -195576,
    [0x37] = -152592,
    [0x3b] = -140328,
    [0x3d] = -201720,
    [0x3f] = -195592,
    [0x55] = -79617,
    [0x57] = -164904,
    [0x5b] = -164904,
    [0x5f] = -207888,
    [0x6f] = -207912,
    [0x77] = -103956,
    [0x7f] = -250920,
    [0xff] = -36738,
};

static const int32_t gnw_schedule_operation_history8_numerator[256] = {
    [0x00] = -30594,
    [0x01] = -189456,
    [0x03] = -109584,
    [0x05] = -134160,
    [0x07] = -91152,
    [0x09] = -134160,
    [0x0b] = -91128,
    [0x0d] = -54288,
    [0x0f] = -72696,
    [0x11] = -67086,
    [0x13] = -41976,
    [0x15] = -78876,
    [0x17] = -72696,
    [0x19] = -54252,
    [0x1b] = -72696,
    [0x1d] = -35832,
    [0x1f] = -54252,
    [0x25] = -78864,
    [0x27] = -23544,
    [0x2b] = -60420,
    [0x2d] = -23532,
    [0x2f] = -41988,
    [0x33] = 25092,
    [0x35] = 13296,
    [0x37] = -41988,
    [0x3b] = -42012,
    [0x3d] = -5148,
    [0x3f] = -23560,
    [0x55] = -5895,
    [0x57] = -152604,
    [0x5b] = -152604,
    [0x5f] = -134136,
    [0x6f] = -134124,
    [0x77] = -67086,
    [0x7f] = -115740,
    [0xff] = -12165,
};


/*
 * The first admitted CPU profile is intentionally one exact ITCM loop.  Its
 * bytes and digest are bound to the held-out v2 capture; AXI SRAM remains
 * excluded because the runtime cannot prove its required primed-cache state.
 */
static const GnwM7ExactBodyIdentity gnw_cpu_ram_itcm_v1_identity = {
    .artifact_sha256 = {
        0x1d, 0x89, 0x6d, 0x18, 0x5b, 0xd5, 0x30, 0xc3,
        0x07, 0x20, 0x5b, 0x78, 0x1b, 0xf7, 0x0a, 0x7b,
        0x53, 0x6b, 0xc1, 0xb3, 0x10, 0xff, 0x09, 0x48,
        0x01, 0x33, 0xf4, 0x91, 0x2e, 0xbd, 0x78, 0xcd,
    },
    .bytes = {
        0x03, 0x32, 0x5a, 0x40, 0x07, 0x32, 0x42, 0x40,
        0x0b, 0x32, 0x01, 0x38, 0x7f, 0xf4, 0xf8, 0xaf,
    },
    .virtual_address = 0x7c,
    .physical_address = 0x7c,
    .size = 16,
    .region = GNW_M7_EXACT_BODY_REGION_ITCM,
};

static uint64_t fp_add(uint64_t a, uint64_t b)
{
    g_assert(b <= UINT64_MAX - a);
    return a + b;
}

static uint64_t fp_ceil_slot(uint64_t value, uint64_t slot)
{
    uint64_t mask = slot - 1;

    g_assert(is_power_of_2(slot));
    g_assert(value <= UINT64_MAX - mask);
    return (value + mask) & ~mask;
}

static uint64_t rebase_value(uint64_t value)
{
    return value > GNW_CYCLE_REBASE_FP ?
           value - GNW_CYCLE_REBASE_FP : 0;
}

static void maybe_rebase(GnwCycleModelState *s)
{
    bool previous_predates_epoch;
    unsigned i;

    if (s->cycle_fp < GNW_CYCLE_REBASE_FP) {
        return;
    }
    previous_predates_epoch = s->previous_valid &&
                              s->previous_issue_fp < GNW_CYCLE_REBASE_FP;
    g_assert(s->epoch_cycles <= UINT64_MAX - GNW_CYCLE_REBASE_CYCLES);
    s->epoch_cycles += GNW_CYCLE_REBASE_CYCLES;
    s->cycle_fp -= GNW_CYCLE_REBASE_FP;
    s->previous_issue_fp = rebase_value(s->previous_issue_fp);
    /*
     * A previous issue before the new epoch cannot share an issue cycle with
     * the first instruction at cycle zero.  Clipping its timestamp to zero
     * while retaining pair eligibility would invent a second slot.
     */
    if (previous_predates_epoch) {
        s->previous_valid = false;
    }
    for (i = 0; i < ARRAY_SIZE(s->gpr_ready_fp); i++) {
        s->gpr_ready_fp[i] = rebase_value(s->gpr_ready_fp[i]);
    }
    for (i = 0; i < ARRAY_SIZE(s->flags_ready_fp); i++) {
        s->flags_ready_fp[i] = rebase_value(s->flags_ready_fp[i]);
        s->branch_flags_ready_fp[i] =
            rebase_value(s->branch_flags_ready_fp[i]);
    }
    for (i = 0; i < ARRAY_SIZE(s->store_visible); i++) {
        s->store_visible[i].ready_fp =
            rebase_value(s->store_visible[i].ready_fp);
        if (!s->store_visible[i].ready_fp) {
            s->store_visible[i].valid = false;
        }
    }
    s->xip_service_cursor_fp = rebase_value(s->xip_service_cursor_fp);
}

static void load_path_reset(GnwM7LoadPathState *load, bool candidate)
{
    memset(load, 0, sizeof(*load));
    load->profile_identity = candidate ? GNW_M7_LOAD_PATH_PROFILE_V5 : 0;
}

static void serialize_baseline(GnwCycleModelState *s)
{
    uint64_t end = fp_add(s->cycle_fp, GNW_CYCLE_FP_ONE);
    unsigned i;

    s->cycle_fp = end;
    s->previous_valid = false;
    s->previous_was_store = false;
    for (i = 0; i < ARRAY_SIZE(s->gpr_ready_fp); i++) {
        s->gpr_ready_fp[i] = MAX(s->gpr_ready_fp[i], end);
    }
    for (i = 0; i < ARRAY_SIZE(s->flags_ready_fp); i++) {
        s->flags_ready_fp[i] = MAX(s->flags_ready_fp[i], end);
        s->branch_flags_ready_fp[i] =
            MAX(s->branch_flags_ready_fp[i], end);
    }
}

static void apply_nc_axi_fetch(GnwCycleModelState *s)
{
    uint64_t cycles = s->pending_nc_axi_fetch_cycles;

    if (!cycles) {
        return;
    }
    g_assert(cycles <= (UINT64_MAX - s->cycle_fp) / GNW_CYCLE_FP_ONE);
    s->cycle_fp += cycles * GNW_CYCLE_FP_ONE;
    s->pending_nc_axi_fetch_cycles = 0;
}

static void apply_icache_maintenance(GnwCycleModelState *s)
{
    uint64_t cycles = s->pending_icache_maintenance_cycles;

    if (!cycles) {
        return;
    }
    g_assert(cycles <= (UINT64_MAX - s->cycle_fp) / GNW_CYCLE_FP_ONE);
    s->cycle_fp += cycles * GNW_CYCLE_FP_ONE;
    s->pending_icache_maintenance_cycles = 0;
}

static void apply_icache_fetch(GnwCycleModelState *s)
{
    uint64_t cycles = s->icache.pending_cycles;

    if (cycles) {
        g_assert(cycles <= (UINT64_MAX - s->cycle_fp) / GNW_CYCLE_FP_ONE);
        s->cycle_fp += cycles * GNW_CYCLE_FP_ONE;
        s->icache.pending_cycles = 0;
    }
}

static void apply_fetch_residual(GnwCycleModelState *s)
{
    uint64_t cycles = s->pending_fetch_residual_cycles;

    if (!cycles) {
        return;
    }
    g_assert(cycles <= (UINT64_MAX - s->cycle_fp) / GNW_CYCLE_FP_ONE);
    s->cycle_fp += cycles * GNW_CYCLE_FP_ONE;
    s->pending_fetch_residual_cycles = 0;
}

static void apply_memory_residual(GnwCycleModelState *s)
{
    uint64_t cycles = s->pending_memory_residual_cycles;

    if (!cycles) {
        return;
    }
    g_assert(cycles <= (UINT64_MAX - s->cycle_fp) / GNW_CYCLE_FP_ONE);
    s->cycle_fp += cycles * GNW_CYCLE_FP_ONE;
    s->pending_memory_residual_cycles = 0;
}

static void schedule_history_reject(GnwCycleModelState *s)
{
    if (!s->schedule_history_active) {
        return;
    }
    /*
     * A positive residual is held in compensation_cycles until the complete
     * 4096-outcome kernel has matched.  Discard it on rejection.  A negative
     * residual is consumed as credit while the kernel runs, so restore only
     * the credit that was actually consumed before the mismatch.
     */
    s->schedule_history_compensation_cycles =
        s->schedule_history_applied_credit_cycles;
    s->schedule_history_credit_cycles = 0;
    s->schedule_history_applied_credit_cycles = 0;
    s->schedule_history_active = false;
    if (s->schedule_history_rejected != UINT64_MAX) {
        s->schedule_history_rejected++;
    }
}

static uint32_t schedule_history_take_compensation(GnwCycleModelState *s)
{
    uint32_t cycles = s->schedule_history_compensation_cycles;

    s->schedule_history_compensation_cycles = 0;
    return cycles;
}

static int32_t schedule_history_residual(uint8_t kind, uint8_t pattern)
{
    const int32_t *coefficients = kind == 1 ?
        gnw_schedule_region_history8_numerator :
        gnw_schedule_operation_history8_numerator;
    int64_t numerator = 0;
    unsigned i;

    for (i = 0; i < 8; i++) {
        numerator += coefficients[pattern];
        pattern = (pattern >> 1) | (pattern << 7);
    }
    return numerator < 0 ?
        -((-numerator + GNW_SCHEDULE_COEFFICIENT_DENOMINATOR / 2) /
          GNW_SCHEDULE_COEFFICIENT_DENOMINATOR) :
        (numerator + GNW_SCHEDULE_COEFFICIENT_DENOMINATOR / 2) /
        GNW_SCHEDULE_COEFFICIENT_DENOMINATOR;
}

static bool schedule_history_instruction_matches(
    const GnwCycleInsnMetadata *m)
{
    switch (m->pc) {
    case 0x24010260: case 0x240102a0: return m->encoding == 0xb420;
    case 0x24010262: case 0x240102a2: return m->encoding == 0xf2420500;
    case 0x24010266: case 0x240102a6: return m->encoding == 0xf2c20500;
    case 0x2401026a: case 0x240102aa: return m->encoding == 0x2201;
    case 0x2401026c: case 0x240102ac: return m->encoding == 0xf05f0c00;
    case 0x24010270: case 0x240102b0: return m->encoding == 0xf815300c;
    case 0x24010274: case 0x240102b4: return m->encoding == 0xf11c0c01;
    case 0x24010278: case 0x240102b8: return m->encoding == 0xf00c0c07;
    case 0x2401027c: case 0x240102bc: return m->encoding == 0x2b00;
    case 0x2401027e: case 0x240102be: return m->encoding == 0xd101;
    case 0x24010280: case 0x240102c0: return m->encoding == 0x680b;
    case 0x24010282: case 0x240102c2: return m->encoding == 0xe000;
    case 0x24010284: return m->encoding == 0x6823;
    case 0x24010286: return m->encoding == 0x18d2;
    case 0x24010288: return m->encoding == 0xea4f0272;
    case 0x2401028c: case 0x240102c8: return m->encoding == 0x3801;
    case 0x2401028e: return m->encoding == 0xd1ef;
    case 0x24010290: case 0x240102cc: return m->encoding == 0x4610;
    case 0x24010292: case 0x240102ce: return m->encoding == 0xbc20;
    case 0x24010294: case 0x240102d0: return m->encoding == 0x4770;
    case 0x240102c4: return m->encoding == 0x6022;
    case 0x240102c6: return m->encoding == 0x3201;
    case 0x240102ca: return m->encoding == 0xd1f1;
    default: return false;
    }
}

static void schedule_history_observe(GnwCycleModelState *s,
                                     const GnwCycleInsnMetadata *m)
{
    uint64_t decision, end;
    bool outcome;

    if (s->schedule_history_profile_identity !=
            GNW_M7_SCHEDULE_HISTORY_PROFILE_V4) {
        return;
    }
    if (!s->schedule_history_active) {
        if (m->argument_gpr0 == GNW_SCHEDULE_ITERATIONS &&
            (m->pc == GNW_SCHEDULE_REGION_ENTRY ||
             m->pc == GNW_SCHEDULE_OPERATION_ENTRY) &&
            schedule_history_instruction_matches(m)) {
            s->schedule_history_active = true;
            s->schedule_history_kind =
                m->pc == GNW_SCHEDULE_REGION_ENTRY ? 1 : 2;
            s->schedule_history_pattern = 0;
            s->schedule_history_outcomes = 0;
        }
        return;
    }
    if (!schedule_history_instruction_matches(m)) {
        schedule_history_reject(s);
        return;
    }
    decision = s->schedule_history_kind == 1 ?
        GNW_SCHEDULE_REGION_DECISION : GNW_SCHEDULE_OPERATION_DECISION;
    end = s->schedule_history_kind == 1 ?
        GNW_SCHEDULE_REGION_RETURN : GNW_SCHEDULE_OPERATION_RETURN;
    if (m->pc == decision) {
        int32_t residual;
        unsigned index = s->schedule_history_outcomes & 7;

        outcome = m->branch_taken;
        if (s->schedule_history_outcomes < 8) {
            s->schedule_history_pattern |= (uint8_t)outcome << index;
        } else if (outcome != ((s->schedule_history_pattern >> index) & 1)) {
            schedule_history_reject(s);
            return;
        }
        s->schedule_history_outcomes++;
        if (s->schedule_history_outcomes == 8) {
            residual = schedule_history_residual(
                s->schedule_history_kind, s->schedule_history_pattern);
            if (residual < 0) {
                s->schedule_history_credit_cycles = -residual;
            } else {
                s->schedule_history_compensation_cycles = residual;
            }
        }
    } else if (m->pc == end) {
        if (s->schedule_history_outcomes != GNW_SCHEDULE_ITERATIONS) {
            schedule_history_reject(s);
            return;
        }
        s->schedule_history_active = false;
        s->schedule_history_applied_credit_cycles = 0;
        if (s->schedule_history_accepted != UINT64_MAX) {
            s->schedule_history_accepted++;
        }
    } else if (m->pc < (s->schedule_history_kind == 1 ?
                        GNW_SCHEDULE_REGION_ENTRY :
                        GNW_SCHEDULE_OPERATION_ENTRY) || m->pc > end) {
        schedule_history_reject(s);
    }
}

static void commit_delta(CPUState *cpu, uint64_t delta_fp,
                         uint64_t retired, bool account)
{
    GnwCycleModelState *s = &cpu->gnw_cycle_model;
    uint64_t total = fp_add(s->commit_fraction_fp, delta_fp);
    uint64_t whole = total >> GNW_CYCLE_FP_SHIFT;

    /* Do not charge a positive residual until the whole kernel has matched. */
    if (s->schedule_history_compensation_cycles &&
        !s->schedule_history_active) {
        uint32_t compensation = schedule_history_take_compensation(s);

        g_assert(whole <= UINT64_MAX -
                 compensation);
        whole += compensation;
    }
    if (s->schedule_history_credit_cycles) {
        uint64_t credit = MIN(whole,
                              s->schedule_history_credit_cycles);

        whole -= credit;
        s->schedule_history_credit_cycles -= credit;
        g_assert(credit <= UINT32_MAX -
                 s->schedule_history_applied_credit_cycles);
        s->schedule_history_applied_credit_cycles += credit;
    }

    s->commit_fraction_fp = total & (GNW_CYCLE_FP_ONE - 1);
    g_assert(whole <= UINT64_MAX - s->modeled_whole_cycles);
    s->modeled_whole_cycles += whole;
    if (account) {
        icount_account_cycles(cpu, retired, whole);
    }
}

bool gnw_cycle_model_enabled(const CPUState *cpu)
{
    return cpu->gnw_cycle_model.enabled;
}

bool gnw_cycle_model_identity_valid(const CPUState *cpu)
{
    const GnwCycleModelState *s = &cpu->gnw_cycle_model;
    unsigned policy = s->dcache.policy;
    const uint32_t (*identities)[8];

    identities = s->xip_service_profile_identity ==
        GNW_M7_XIP_SERVICE_PROFILE_TEST_FIXED ?
        gnw_cycle_model_xip_test_config_identity :
        gnw_cycle_model_config_identity;

    return s->implementation_version ==
               GNW_CYCLE_MODEL_IMPLEMENTATION_VERSION &&
           policy < GNW_M7_DCACHE_POLICY_COUNT &&
           !memcmp(s->config_identity,
                   identities[policy],
                   sizeof(s->config_identity));
}

bool gnw_cycle_model_cpu_ram_itcm_profile_valid(const CPUState *cpu,
                                                 bool enabled)
{
    const GnwCycleModelState *s = &cpu->gnw_cycle_model;

    if (!enabled) {
        return s->cpu_ram_itcm_profile_identity == 0 &&
               !s->cpu_ram_itcm_profile_accepted &&
               !s->cpu_ram_itcm_profile_rejected;
    }
    return s->cpu_ram_itcm_profile_identity ==
               GNW_M7_CPU_RAM_ITCM_PROFILE_V1 &&
           s->exact_body_identity_configured &&
           !memcmp(&s->exact_body_identity, &gnw_cpu_ram_itcm_v1_identity,
                   sizeof(s->exact_body_identity));
}

bool gnw_cycle_model_exception_transition_profile_valid(const CPUState *cpu,
                                                         bool enabled)
{
    const GnwCycleModelState *s = &cpu->gnw_cycle_model;
    unsigned i;

    if (s->exception_transition_profile_identity !=
            (enabled ? GNW_M7_EXCEPTION_TRANSITION_PROFILE_V6 : 0)) {
        return false;
    }
    if (!enabled) {
        for (i = 0; i < GNW_CYCLE_EXCEPTION_TRANSITION_COUNT; i++) {
            if (s->exception_transition_count[i]) {
                return false;
            }
        }
    }
    return true;
}

bool gnw_cycle_model_fetch_residual_profile_valid(const CPUState *cpu,
                                                   bool enabled)
{
    const GnwCycleModelState *s = &cpu->gnw_cycle_model;

    if (s->fetch_residual_profile_identity !=
            (enabled ? GNW_M7_FETCH_RESIDUAL_PROFILE_V1 :
                       GNW_M7_FETCH_RESIDUAL_PROFILE_OFF) ||
        s->fetch_residual_fraction >= GNW_M7_FETCH_RESIDUAL_DENOMINATOR ||
        s->pending_fetch_residual_cycles) {
        return false;
    }
    return enabled || (!s->fetch_residual_fraction &&
                       !s->fetch_residual_charges &&
                       !s->fetch_residual_cycles &&
                       !s->fetch_residual_set_conflict_active);
}

bool gnw_cycle_model_memory_residual_profile_valid(const CPUState *cpu,
                                                    bool enabled)
{
    const GnwCycleModelState *s = &cpu->gnw_cycle_model;

    if (s->memory_residual_profile_identity !=
            (enabled ? GNW_M7_MEMORY_RESIDUAL_PROFILE_V1 :
                       GNW_M7_MEMORY_RESIDUAL_PROFILE_OFF) ||
        s->pending_memory_residual_cycles) {
        return false;
    }
    if (!enabled) {
        return !s->memory_residual_active && !s->memory_residual_charges &&
               !s->memory_residual_cycles && !s->memory_residual_accepted &&
               !s->memory_residual_rejected;
    }
    if (!s->memory_residual_active) {
        return !s->memory_residual_remaining &&
               !s->memory_residual_entry_pc;
    }
    return s->memory_residual_bytes >= GNW_MEMORY_MIN_BYTES &&
           s->memory_residual_bytes <= GNW_MEMORY_MAX_BYTES &&
           s->memory_residual_remaining <= s->memory_residual_bytes &&
           (((s->memory_residual_base == GNW_MEMORY_DTCM_BASE ||
              s->memory_residual_base == GNW_MEMORY_AXI_BASE) &&
             s->memory_residual_stride == 1) ||
            ((s->memory_residual_base == GNW_MEMORY_AHB_FIT_BASE ||
              s->memory_residual_base == GNW_MEMORY_AHB_HOLD_BASE) &&
             (s->memory_residual_stride == 1 ||
              s->memory_residual_stride == 33)));
}

bool gnw_cycle_model_schedule_history_profile_valid(const CPUState *cpu,
                                                     bool enabled)
{
    const GnwCycleModelState *s = &cpu->gnw_cycle_model;

    if (s->schedule_history_profile_identity !=
            (enabled ? GNW_M7_SCHEDULE_HISTORY_PROFILE_V4 :
                       GNW_M7_SCHEDULE_HISTORY_PROFILE_OFF) ||
        s->schedule_history_outcomes > GNW_SCHEDULE_ITERATIONS ||
        s->schedule_history_credit_cycles > 14296 ||
        s->schedule_history_applied_credit_cycles > 14296 ||
        s->schedule_history_credit_cycles +
            s->schedule_history_applied_credit_cycles > 14296 ||
        s->schedule_history_compensation_cycles > 2091) {
        return false;
    }
    if (!enabled) {
        return !s->schedule_history_outcomes &&
               !s->schedule_history_credit_cycles &&
               !s->schedule_history_applied_credit_cycles &&
               !s->schedule_history_compensation_cycles &&
               !s->schedule_history_accepted &&
               !s->schedule_history_rejected &&
               !s->schedule_history_pattern &&
               !s->schedule_history_kind &&
               !s->schedule_history_active;
    }
    if (s->schedule_history_active) {
        return (s->schedule_history_kind == 1 ||
                s->schedule_history_kind == 2) &&
               (s->schedule_history_outcomes >= 8 ||
                (!s->schedule_history_credit_cycles &&
                 !s->schedule_history_applied_credit_cycles &&
                 !s->schedule_history_compensation_cycles)) &&
               !(s->schedule_history_compensation_cycles &&
                 (s->schedule_history_credit_cycles ||
                  s->schedule_history_applied_credit_cycles));
    }
    return !s->schedule_history_credit_cycles &&
           !s->schedule_history_applied_credit_cycles &&
           !s->schedule_history_compensation_cycles;
}

/*
 * Exception entry and return are full pipeline boundaries.  The calibrated
 * transition cost is committed directly to guest time, outside cycle_fp, so
 * pre-transition issue and dependency state must not leak into the first
 * instruction on the other side.  In particular, retaining previous_valid
 * would let that instruction dual-issue with an instruction from the
 * interrupted context.
 *
 * All modeled producer latencies are at most three cycles, while every
 * admitted exception transition is at least 11 cycles.  Consequently every
 * pre-transition register, flag, and store dependency has completed by the
 * end of the transition and can be cleared without adding an inferred cost.
 */
static void exception_transition_scoreboard_flush(GnwCycleModelState *s)
{
    s->previous_valid = false;
    s->previous_was_store = false;
    memset(s->gpr_ready_fp, 0, sizeof(s->gpr_ready_fp));
    memset(s->flags_ready_fp, 0, sizeof(s->flags_ready_fp));
    memset(s->branch_flags_ready_fp, 0,
           sizeof(s->branch_flags_ready_fp));
    memset(s->store_visible, 0, sizeof(s->store_visible));
    s->exception_scoreboard_flush_pending = false;
}

bool gnw_cycle_model_exception_transition(
    CPUState *cpu, GnwCycleExceptionTransition transition)
{
    GnwCycleModelState *s = &cpu->gnw_cycle_model;
    static const uint8_t cycles[GNW_CYCLE_EXCEPTION_TRANSITION_COUNT] = {
        [GNW_CYCLE_EXCEPTION_ENTRY] = GNW_M7_EXCEPTION_ENTRY_CYCLES,
        [GNW_CYCLE_EXCEPTION_RETURN] = GNW_M7_EXCEPTION_RETURN_CYCLES,
        [GNW_CYCLE_EXCEPTION_TAIL_CHAIN] = GNW_M7_EXCEPTION_TAIL_CHAIN_CYCLES,
    };
    uint64_t cost;
    uint32_t compensation;

    if (s->enabled) {
        gnw_m7_icache_end_disabled_fetch_run(&s->icache);
    }
    if (s->enabled && s->xip_service_profile_identity ==
        GNW_M7_XIP_SERVICE_PROFILE_UNSEEN_V1) {
        xip_unseen_end_stream(s);
    }
    if (!s->enabled || s->exception_transition_profile_identity !=
                       GNW_M7_EXCEPTION_TRANSITION_PROFILE_V6) {
        return false;
    }
    g_assert(transition < GNW_CYCLE_EXCEPTION_TRANSITION_COUNT);
    cost = cycles[transition];
    /* An interrupt leaves the exact schedule kernel just as any other PC
     * departure does.  Settle its credit at the architectural boundary.
     *
     * A successful exception return is observed from inside the retiring
     * branch helper.  Flushing here would let finish() immediately repopulate
     * the scoreboard with that branch and incorrectly pair the first resumed
     * instruction across the exception boundary.  Defer only the scoreboard
     * clear until the in-flight instruction has committed. */
    schedule_history_reject(s);
    if (s->pending_valid) {
        s->exception_scoreboard_flush_pending = true;
    } else {
        exception_transition_scoreboard_flush(s);
    }
    /*
     * Rejecting a partially matched schedule kernel can refund residual
     * credit already consumed by earlier instructions.  An exception is a
     * guest-visible timing boundary, so include that refund now rather than
     * leaving DWT_CYCCNT low until the first handler instruction commits.
     * This also settles a positive residual from a just-completed exact
     * kernel before entering the handler.
     */
    compensation = schedule_history_take_compensation(s);
    g_assert(cost <= UINT64_MAX - compensation);
    cost += compensation;
    g_assert(cost <= UINT64_MAX - s->modeled_whole_cycles);
    s->modeled_whole_cycles += cost;
    if (s->exception_transition_count[transition] != UINT64_MAX) {
        s->exception_transition_count[transition]++;
    }
    return icount_commit_stall_cycles(cpu, cost);
}

bool gnw_cycle_model_xip_service_profile_valid(const CPUState *cpu)
{
    const GnwCycleModelState *s = &cpu->gnw_cycle_model;

    return s->xip_service_profile_identity ==
               GNW_M7_XIP_SERVICE_PROFILE_OFF ||
           s->xip_service_profile_identity ==
               GNW_M7_XIP_SERVICE_PROFILE_TEST_FIXED ||
           s->xip_service_profile_identity ==
               GNW_M7_XIP_SERVICE_PROFILE_UNSEEN_V1;
}

void gnw_cycle_model_set_xip_unseen_scope_probe(
    CPUState *cpu, bool (*probe)(void *opaque), void *opaque)
{
    cpu->gnw_cycle_model.xip_unseen_scope_probe = probe;
    cpu->gnw_cycle_model.xip_unseen_scope_opaque = opaque;
}

void gnw_cycle_model_set_bus_contention_probe(
    CPUState *cpu,
    uint32_t (*probe)(void *opaque, uint64_t address, unsigned size,
                      int64_t guest_time_ns),
    void *opaque)
{
    cpu->gnw_cycle_model.bus_contention_probe = probe;
    cpu->gnw_cycle_model.bus_contention_probe_opaque = opaque;
}

bool gnw_cycle_model_set_exact_body_diagnostic(
    CPUState *cpu, const GnwM7ExactBodyIdentity *identity)
{
    GnwCycleModelState *s = &cpu->gnw_cycle_model;

    memset(&s->exact_body_identity, 0, sizeof(s->exact_body_identity));
    memset(&s->exact_body_match, 0, sizeof(s->exact_body_match));
    s->exact_body_identity_configured = false;
    s->exact_body_accepted = 0;
    s->exact_body_rejected = 0;
    if (!identity) {
        return true;
    }
    if (!gnw_m7_exact_body_identity_valid(identity)) {
        return false;
    }
    s->exact_body_identity = *identity;
    s->exact_body_identity_configured = true;
    return true;
}

bool gnw_cycle_model_set_flash_schedule_diagnostic(
    CPUState *cpu, const GnwM7FlashScheduleIdentity
        identities[GNW_M7_FLASH_SCHEDULE_IDENTITIES])
{
    GnwCycleModelState *s = &cpu->gnw_cycle_model;

    memset(s->flash_schedule_identity, 0,
           sizeof(s->flash_schedule_identity));
    memset(&s->flash_schedule, 0, sizeof(s->flash_schedule));
    memset(&s->flash_schedule_pending, 0,
           sizeof(s->flash_schedule_pending));
    s->flash_schedule_configured = false;
    s->flash_schedule_attempt_pending = false;
    if (!identities) {
        return true;
    }
    if (!gnw_m7_flash_schedule_identities_valid(identities)) {
        return false;
    }
    memcpy(s->flash_schedule_identity, identities,
           sizeof(s->flash_schedule_identity));
    s->flash_schedule_configured = true;
    return true;
}

/*
 * Exact, source-bound diagnostic identities from
 * direct-flash-phase-v2/schedule-kind-5-6-loop-identities.json.  The string
 * gate is that artifact's self SHA-256.  These identities classify fetches
 * only: they carry no timing cost or coefficient authority.
 */
#define GNW_FLASH_SCHEDULE_DIAGNOSTIC_SHA256 \
    "7df95817cd7cab10b760b8221d7b9d07e996f51508b3c705db942bd01a0f7941"
#define GNW_FLASH_SCHEDULE_ARTIFACT_DIGEST { \
    0x19, 0x9a, 0x5a, 0x08, 0x58, 0x72, 0x68, 0x29, \
    0xf0, 0x05, 0x28, 0x1c, 0x99, 0x96, 0xb8, 0x0c, \
    0x84, 0x9b, 0x4b, 0xa7, 0xe0, 0x83, 0x31, 0xb5, \
    0x4f, 0x3e, 0xb9, 0xc8, 0x9c, 0x2c, 0x9f, 0x25 }
#define GNW_FLASH_SCHEDULE_SEQ_BYTES { \
    0x80, 0xea, 0x01, 0x03, 0x00, 0xeb, 0x01, 0x0b, \
    0x40, 0xea, 0x01, 0x0c, 0x85, 0xea, 0x06, 0x08, \
    0x05, 0xeb, 0x06, 0x09, 0x1b, 0x46, 0xdb, 0x46, \
    0xe4, 0x46, 0x01, 0x32, 0xa2, 0x42, 0xef, 0xd1 }
#define GNW_FLASH_SCHEDULE_FORWARD_BYTES { \
    0x80, 0xea, 0x01, 0x03, 0x00, 0xeb, 0x01, 0x0b, \
    0x40, 0xea, 0x01, 0x0c, 0x00, 0xf0, 0x01, 0xb8, \
    0x1b, 0x46, 0x85, 0xea, 0x06, 0x08, 0x05, 0xeb, \
    0x06, 0x09, 0x01, 0x32, 0xa2, 0x42, 0xef, 0xd1 }
#define GNW_FLASH_SCHEDULE_IDENTITY(_bytes, _address, _kind, _role) { \
    .artifact_sha256 = GNW_FLASH_SCHEDULE_ARTIFACT_DIGEST, \
    .bytes = _bytes, .virtual_address = _address, \
    .physical_address = _address, .classifier_identity = 0x53434831, \
    .schedule_kind = _kind, .loop_start_phase = (_address) & 7, \
    .role = _role }

static const GnwM7FlashScheduleIdentity gnw_flash_schedule_diagnostic[] = {
    GNW_FLASH_SCHEDULE_IDENTITY(GNW_FLASH_SCHEDULE_SEQ_BYTES,
                                0x0800d002, 5, 0),
    GNW_FLASH_SCHEDULE_IDENTITY(GNW_FLASH_SCHEDULE_SEQ_BYTES,
                                0x0800d0a6, 5, 0),
    GNW_FLASH_SCHEDULE_IDENTITY(GNW_FLASH_SCHEDULE_FORWARD_BYTES,
                                0x0800d142, 6, 0),
    GNW_FLASH_SCHEDULE_IDENTITY(GNW_FLASH_SCHEDULE_FORWARD_BYTES,
                                0x0800d1e6, 6, 0),
    GNW_FLASH_SCHEDULE_IDENTITY(GNW_FLASH_SCHEDULE_SEQ_BYTES,
                                0x0800d282, 5, 1),
    GNW_FLASH_SCHEDULE_IDENTITY(GNW_FLASH_SCHEDULE_SEQ_BYTES,
                                0x0800d326, 5, 1),
    GNW_FLASH_SCHEDULE_IDENTITY(GNW_FLASH_SCHEDULE_FORWARD_BYTES,
                                0x0800d3c2, 6, 1),
    GNW_FLASH_SCHEDULE_IDENTITY(GNW_FLASH_SCHEDULE_FORWARD_BYTES,
                                0x0800d466, 6, 1),
};

bool gnw_cycle_model_set_flash_schedule_diagnostic_artifact(
    CPUState *cpu, const char *identity_sha256)
{
    if (!identity_sha256) {
        return gnw_cycle_model_set_flash_schedule_diagnostic(cpu, NULL);
    }
    if (strcmp(identity_sha256, GNW_FLASH_SCHEDULE_DIAGNOSTIC_SHA256)) {
        return false;
    }
    return gnw_cycle_model_set_flash_schedule_diagnostic(
        cpu, gnw_flash_schedule_diagnostic);
}

static void xip_service_request(CPUState *cpu, uint64_t address,
                                unsigned size)
{
    GnwCycleModelState *s = &cpu->gnw_cycle_model;
    uint64_t start, end, delta;

    if (s->xip_service_profile_identity !=
            GNW_M7_XIP_SERVICE_PROFILE_TEST_FIXED ||
        !size || address < GNW_M7_XIP_BASE || address >= GNW_M7_XIP_END ||
        size > GNW_M7_XIP_END - address) {
        return;
    }

    maybe_rebase(s);
    start = MAX(s->cycle_fp, s->xip_service_cursor_fp);
    end = fp_add(start, GNW_M7_XIP_TEST_SERVICE_CYCLES * GNW_CYCLE_FP_ONE);
    delta = end - s->cycle_fp;
    s->xip_service_cursor_fp = end;
    s->cycle_fp = end;
    s->xip_service_requests++;
    g_assert((delta >> GNW_CYCLE_FP_SHIFT) <=
             UINT64_MAX - s->xip_service_cycles);
    s->xip_service_cycles += delta >> GNW_CYCLE_FP_SHIFT;
    g_assert(!(delta & (GNW_CYCLE_FP_ONE - 1)));
    delta >>= GNW_CYCLE_FP_SHIFT;
    g_assert(delta <= UINT64_MAX - s->xip_service_remaining_cycles);
    s->xip_service_remaining_cycles += delta;
}

static void xip_unseen_end_stream(GnwCycleModelState *s)
{
    s->xip_unseen_stream_active = false;
    s->xip_unseen_next_address = 0;
}

/* Apply the retained, hardware-backed v1 fit only to the exact observed
 * sequential byte-load shape. Fractional cycles carry across stream starts;
 * the measured stream intercept and first-window terms are charged once. */
static void xip_unseen_service_observe(
    CPUState *cpu, const GnwCycleMemoryObservation *o)
{
    GnwCycleModelState *s = &cpu->gnw_cycle_model;
    const uint64_t base = GNW_M7_XIP_BASE;
    const uint64_t end = GNW_M7_XIP_UNSEEN_END;
    uint64_t address = o->physical_address;
    uint64_t numerator = 0;
    bool eligible = o->kind == GNW_CYCLE_MEMORY_LOAD &&
        o->size == 1 && o->access_count == 1 && !o->access_index &&
        !o->access_flags && !(o->memop & MO_SIGN) && !o->user && !o->secure &&
        o->translation_valid && o->pmsa_valid && o->pmsa_region == 7 &&
        o->pmsa_source == GNW_CYCLE_PMSA_REGION && o->pmsa_attrs == 0x08 &&
        !o->dcache_enabled && address >= base && address < end &&
        s->xip_unseen_scope_probe &&
        s->xip_unseen_scope_probe(s->xip_unseen_scope_opaque) &&
        s->bus_contention_probe &&
        !s->bus_contention_probe(s->bus_contention_probe_opaque,
                                 address, 1,
                                 icount_get_committed_time_ns());

    /* The measured copy loop interleaves each XIP byte read with its DTCM
     * destination store. That store is part of the byte-stream body, not a
     * break in source-side OSPI continuity. */
    if (s->xip_unseen_stream_active &&
        o->kind == GNW_CYCLE_MEMORY_STORE && o->translation_valid &&
        o->store_succeeded && o->access_count == 1 && !o->access_flags &&
        o->physical_address >= 0x20000000ULL &&
        o->physical_address < 0x20020000ULL) {
        return;
    }
    if (!eligible) {
        xip_unseen_end_stream(s);
        return;
    }
    if (s->xip_service_requests != UINT64_MAX) {
        s->xip_service_requests++;
    }
    if (!s->xip_unseen_stream_active ||
        address != s->xip_unseen_next_address) {
        numerator += GNW_M7_XIP_UNSEEN_INTERCEPT_NUMERATOR;
        numerator += (int64_t)(address & 7) *
                     GNW_M7_XIP_UNSEEN_OFFSET_NUMERATOR;
        if (s->xip_unseen_streams != UINT64_MAX) {
            s->xip_unseen_streams++;
        }
    }
    numerator += GNW_M7_XIP_UNSEEN_BYTE_NUMERATOR;
    if ((address & 7) == 0 ||
        !s->xip_unseen_stream_active ||
        address != s->xip_unseen_next_address) {
        numerator += GNW_M7_XIP_UNSEEN_WINDOW_NUMERATOR;
    }
    g_assert(s->xip_unseen_fraction < GNW_M7_XIP_UNSEEN_DENOMINATOR);
    {
        __uint128_t total = (__uint128_t)s->xip_unseen_fraction + numerator;
        uint64_t cycles = total / GNW_M7_XIP_UNSEEN_DENOMINATOR;
        s->xip_unseen_fraction = total % GNW_M7_XIP_UNSEEN_DENOMINATOR;
        if (s->xip_unseen_qualified_bytes != UINT64_MAX) {
            s->xip_unseen_qualified_bytes++;
        }
        if (cycles) {
            uint64_t start, delta;
            maybe_rebase(s);
            start = MAX(s->cycle_fp, s->xip_service_cursor_fp);
            g_assert(cycles <= (UINT64_MAX >> GNW_CYCLE_FP_SHIFT));
            delta = cycles << GNW_CYCLE_FP_SHIFT;
            s->xip_service_cursor_fp = fp_add(start, delta);
            delta = s->xip_service_cursor_fp - s->cycle_fp;
            s->cycle_fp = s->xip_service_cursor_fp;
            g_assert((delta >> GNW_CYCLE_FP_SHIFT) <=
                     UINT64_MAX - s->xip_service_cycles);
            g_assert((delta >> GNW_CYCLE_FP_SHIFT) <=
                     UINT64_MAX - s->xip_service_remaining_cycles);
            s->xip_service_cycles += delta >> GNW_CYCLE_FP_SHIFT;
            s->xip_service_remaining_cycles +=
                delta >> GNW_CYCLE_FP_SHIFT;
        }
    }
    if (address == end - 1) {
        xip_unseen_end_stream(s);
    } else {
        s->xip_unseen_stream_active = true;
        s->xip_unseen_next_address = address + 1;
    }
}

bool gnw_cycle_model_xip_service_pending(const CPUState *cpu)
{
    const GnwCycleModelState *s = &cpu->gnw_cycle_model;

    return s->xip_service_remaining_cycles || s->xip_service_deferred_finish;
}

bool gnw_cycle_model_xip_service_needs_defer(const CPUState *cpu)
{
    const GnwCycleModelState *s = &cpu->gnw_cycle_model;

    return s->xip_service_remaining_cycles &&
           !s->xip_service_deferred_finish;
}

static void xip_service_cancel_pending(GnwCycleModelState *s)
{
    uint64_t cycles = s->xip_service_remaining_cycles;
    uint64_t delta_fp;

    if (!cycles) {
        return;
    }
    g_assert(!s->xip_service_deferred_finish);
    if (s->xip_service_profile_identity ==
        GNW_M7_XIP_SERVICE_PROFILE_UNSEEN_V1) {
        g_assert(cycles <= s->xip_service_cycles);
        g_assert(cycles <= s->cycle_fp >> GNW_CYCLE_FP_SHIFT);
        delta_fp = cycles << GNW_CYCLE_FP_SHIFT;
        s->cycle_fp -= delta_fp;
        s->xip_service_cursor_fp = s->cycle_fp;
        s->xip_service_cycles -= cycles;
        s->xip_service_remaining_cycles = 0;
        return;
    }
    g_assert(!(cycles % GNW_M7_XIP_TEST_SERVICE_CYCLES));
    g_assert(cycles <= s->xip_service_cycles);
    g_assert(cycles <= s->cycle_fp >> GNW_CYCLE_FP_SHIFT);
    g_assert(cycles / GNW_M7_XIP_TEST_SERVICE_CYCLES <=
             s->xip_service_requests);
    delta_fp = cycles << GNW_CYCLE_FP_SHIFT;
    s->cycle_fp -= delta_fp;
    s->xip_service_cursor_fp = s->cycle_fp;
    s->xip_service_requests -=
        cycles / GNW_M7_XIP_TEST_SERVICE_CYCLES;
    s->xip_service_cycles -= cycles;
    s->xip_service_remaining_cycles = 0;
}

void gnw_cycle_model_defer_finish(CPUState *cpu)
{
    GnwCycleModelState *s = &cpu->gnw_cycle_model;

    g_assert(s->xip_service_remaining_cycles);
    g_assert(!s->xip_service_deferred_finish);
    s->xip_service_deferred_finish = true;
    /* Consume the executed TB slot without making its base cycle visible. */
    icount_account_cycles(cpu, 1, 0);
    /*
     * The enclosing execution paths return before another instruction can
     * run.  If this commit reaches a deadline, its notification makes the
     * next cpu_handle_interrupt() pass service it before the deferred finish.
     */
    icount_commit_model_cycles(cpu);
}

bool gnw_cycle_model_drain_xip_service(CPUState *cpu)
{
    GnwCycleModelState *s = &cpu->gnw_cycle_model;
    int64_t deadline_ns, deadline_cycles;
    uint64_t slice, modeled_before;
    bool expired;

    g_assert(s->enabled && s->xip_service_deferred_finish);
    if (!s->xip_service_remaining_cycles) {
        gnw_cycle_model_finish(cpu);
        s->xip_service_deferred_finish = false;
        return icount_commit_model_cycles(cpu);
    }
    /* Match normal icount budgeting: external timers can feed guest input. */
    deadline_ns = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL,
                                             QEMU_TIMER_ATTR_ALL);
    if (deadline_ns == 0) {
        qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
        return true;
    }
    slice = s->xip_service_remaining_cycles;
    if (deadline_ns > 0) {
        deadline_cycles = icount_round(deadline_ns);
        slice = MIN(slice, (uint64_t)MAX(deadline_cycles, 1));
    }
    s->xip_service_remaining_cycles -= slice;
    /* Publish service cycles only as their guest-time slices are committed.
     * A deadline can return to the main loop with service still pending; the
     * observable model counter must not run ahead of DWT and peripherals. */
    modeled_before = s->modeled_whole_cycles;
    g_assert(slice <= UINT64_MAX - modeled_before);
    s->modeled_whole_cycles += slice;
    trace_gnw_cycle_xip_service_commit(cpu->cpu_index, slice,
        s->xip_service_remaining_cycles, modeled_before,
        s->modeled_whole_cycles);
    expired = icount_commit_stall_cycles(cpu, slice);
    if (expired || s->xip_service_remaining_cycles) {
        return true;
    }
    gnw_cycle_model_finish(cpu);
    s->xip_service_deferred_finish = false;
    return icount_commit_model_cycles(cpu);
}

static void dcache_reset(GnwM7DCacheState *d, GnwM7DCachePolicy policy)
{
    unsigned set;

    memset(d, 0, sizeof(*d));
    d->policy = policy;
    if (policy == GNW_M7_DCACHE_TRUE_LRU_HYPOTHESIS) {
        for (set = 0; set < GNW_M7_DCACHE_SETS; set++) {
            d->sets[set].replacement_state = GNW_M7_LRU_RESET_STATE;
        }
    }
}

static void gnw_cycle_model_seed_near_rebase(GnwCycleModelState *s)
{
    uint64_t half = GNW_CYCLE_FP_ONE / 2;

    s->cycle_fp = GNW_CYCLE_REBASE_FP;
    s->previous_issue_fp = GNW_CYCLE_REBASE_FP - half;
    s->previous_valid = true;
    s->previous.insn_class = GNW_CYCLE_INSN_ALU;
    s->previous.flags = GNW_CYCLE_META_SUPPORTED;
    s->previous.gpr_writes = 1U << 10;
    s->gpr_ready_fp[10] = GNW_CYCLE_REBASE_FP + half;
    /* Represent the completed first slot immediately before this state. */
    s->commit_fraction_fp = half;
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
                            bool load_path_candidate)
{
    GnwCycleModelState *s = &cpu->gnw_cycle_model;

    g_assert(!s->enabled);
    g_assert(dcache_policy < GNW_M7_DCACHE_POLICY_COUNT);
    memset(s, 0, sizeof(*s));
    s->enabled = true;
    s->test_near_rebase = test_near_rebase;
    dcache_reset(&s->dcache, dcache_policy);
    memcpy(s->config_identity,
           (xip_service_test_profile ?
            gnw_cycle_model_xip_test_config_identity :
            gnw_cycle_model_config_identity)[dcache_policy],
           sizeof(s->config_identity));
    s->implementation_version = GNW_CYCLE_MODEL_IMPLEMENTATION_VERSION;
    s->xip_service_profile_identity = xip_service_test_profile ?
        GNW_M7_XIP_SERVICE_PROFILE_TEST_FIXED :
        (xip_service_unseen_v1_profile ?
         GNW_M7_XIP_SERVICE_PROFILE_UNSEEN_V1 :
         GNW_M7_XIP_SERVICE_PROFILE_OFF);
    s->bus_contention_profile_identity = bus_contention_test_profile ?
        GNW_M7_BUS_CONTENTION_PROFILE_TEST_FIXED :
        (bus_contention_mdma_axi1_profile ?
         GNW_M7_BUS_CONTENTION_PROFILE_MDMA_AXI1_V1 :
         (bus_contention_v37_bwm_comparison ?
          GNW_M7_BUS_CONTENTION_PROFILE_V37_BWM_COMPARE :
          (bus_contention_v46_bwm_17_4 ?
           GNW_M7_BUS_CONTENTION_PROFILE_V46_BWM_17_4 :
           GNW_M7_BUS_CONTENTION_PROFILE_OFF)));
    if (cpu_ram_itcm_profile) {
        s->cpu_ram_itcm_profile_identity = GNW_M7_CPU_RAM_ITCM_PROFILE_V1;
        g_assert(gnw_cycle_model_set_exact_body_diagnostic(
            cpu, &gnw_cpu_ram_itcm_v1_identity));
    }
    s->exception_transition_profile_identity =
        exception_transition_profile ?
        GNW_M7_EXCEPTION_TRANSITION_PROFILE_V6 : 0;
    s->fetch_residual_profile_identity = fetch_residual_profile ?
        GNW_M7_FETCH_RESIDUAL_PROFILE_V1 :
        GNW_M7_FETCH_RESIDUAL_PROFILE_OFF;
    s->memory_residual_profile_identity = memory_residual_profile ?
        GNW_M7_MEMORY_RESIDUAL_PROFILE_V1 :
        GNW_M7_MEMORY_RESIDUAL_PROFILE_OFF;
    s->schedule_history_profile_identity = schedule_history_profile ?
        GNW_M7_SCHEDULE_HISTORY_PROFILE_V4 :
        GNW_M7_SCHEDULE_HISTORY_PROFILE_OFF;
    load_path_reset(&s->load_path, load_path_candidate);
    s->icache.enabled = true;
    if (test_near_rebase) {
        gnw_cycle_model_seed_near_rebase(s);
    }
}

void gnw_cycle_model_reset(CPUState *cpu)
{
    GnwCycleModelState *s = &cpu->gnw_cycle_model;
    bool enabled = s->enabled;
    bool test_near_rebase = s->test_near_rebase;
    GnwM7DCachePolicy dcache_policy = s->dcache.policy;
    uint32_t identity[ARRAY_SIZE(s->config_identity)];
    uint32_t version = s->implementation_version;
    uint32_t xip_profile = s->xip_service_profile_identity;
    bool (*xip_scope_probe)(void *) = s->xip_unseen_scope_probe;
    void *xip_scope_opaque = s->xip_unseen_scope_opaque;
    uint32_t bus_profile = s->bus_contention_profile_identity;
    uint32_t cpu_ram_profile = s->cpu_ram_itcm_profile_identity;
    uint32_t exception_profile = s->exception_transition_profile_identity;
    uint32_t fetch_residual_profile = s->fetch_residual_profile_identity;
    uint32_t memory_residual_profile = s->memory_residual_profile_identity;
    uint32_t schedule_history_profile =
        s->schedule_history_profile_identity;
    bool load_path_candidate =
        s->load_path.profile_identity == GNW_M7_LOAD_PATH_PROFILE_V5;
    uint32_t (*bus_probe)(void *, uint64_t, unsigned, int64_t) =
        s->bus_contention_probe;
    void *bus_probe_opaque = s->bus_contention_probe_opaque;
    GnwM7ExactBodyIdentity exact_body_identity = s->exact_body_identity;
    bool exact_body_configured = s->exact_body_identity_configured;
    GnwM7FlashScheduleIdentity
        flash_schedule_identity[GNW_M7_FLASH_SCHEDULE_IDENTITIES];
    bool flash_schedule_configured = s->flash_schedule_configured;

    memcpy(flash_schedule_identity, s->flash_schedule_identity,
           sizeof(flash_schedule_identity));

    if (s->xip_service_deferred_finish) {
        /* The executed slot was credited while its finish was deferred. */
        icount_retract_cycle_credit(1);
    }

    memcpy(identity, s->config_identity, sizeof(identity));
    memset(s, 0, sizeof(*s));
    s->enabled = enabled;
    if (enabled) {
        s->test_near_rebase = test_near_rebase;
        dcache_reset(&s->dcache, dcache_policy);
        memcpy(s->config_identity, identity, sizeof(identity));
        s->implementation_version = version;
        s->xip_service_profile_identity = xip_profile;
        s->xip_unseen_scope_probe = xip_scope_probe;
        s->xip_unseen_scope_opaque = xip_scope_opaque;
        s->bus_contention_profile_identity = bus_profile;
        s->cpu_ram_itcm_profile_identity = cpu_ram_profile;
        s->exception_transition_profile_identity = exception_profile;
        s->fetch_residual_profile_identity = fetch_residual_profile;
        s->memory_residual_profile_identity = memory_residual_profile;
        s->schedule_history_profile_identity = schedule_history_profile;
        s->icache.enabled = true;
        s->bus_contention_probe = bus_probe;
        s->bus_contention_probe_opaque = bus_probe_opaque;
        if (exact_body_configured) {
            s->exact_body_identity = exact_body_identity;
            s->exact_body_identity_configured = true;
        }
        if (flash_schedule_configured) {
            memcpy(s->flash_schedule_identity, flash_schedule_identity,
                   sizeof(s->flash_schedule_identity));
            s->flash_schedule_configured = true;
        }
        load_path_reset(&s->load_path, load_path_candidate);
        if (test_near_rebase) {
            gnw_cycle_model_seed_near_rebase(s);
        }
    }
}

void gnw_cycle_model_begin(CPUState *cpu,
                           const GnwCycleInsnMetadata *metadata,
                           bool disfold_set)
{
    GnwCycleModelState *s = &cpu->gnw_cycle_model;

    if (!s->enabled) {
        return;
    }
    g_assert(!s->pending_valid);
    s->pending = *metadata;
    s->pending_valid = true;
    s->disfold_set = disfold_set;
}

void gnw_cycle_model_refine(CPUState *cpu,
                            const GnwCycleInsnMetadata *metadata)
{
    GnwCycleModelState *s = &cpu->gnw_cycle_model;
    GnwCycleInsnMetadata *pending;

    if (!s->enabled) {
        return;
    }
    g_assert(s->pending_valid);
    pending = &s->pending;
    pending->gpr_reads = metadata->gpr_reads;
    pending->gpr_writes = metadata->gpr_writes;
    pending->store_data_reads = metadata->store_data_reads;
    pending->address_reads = metadata->address_reads;
    pending->insn_class = metadata->insn_class;
    pending->flags = metadata->flags;
    pending->fallback_reason = metadata->fallback_reason;
    pending->apsr_reads = metadata->apsr_reads;
    pending->apsr_writes = metadata->apsr_writes;
    pending->memory_kind = metadata->memory_kind;
    pending->memory_size = metadata->memory_size;
}

static GnwM7DCacheLine *dcache_line(GnwM7DCacheState *d,
                                    unsigned set, unsigned way)
{
    return &d->lines[set * GNW_M7_DCACHE_WAYS + way];
}

static bool dcache_set_nonempty(GnwM7DCacheState *d, unsigned set)
{
    unsigned way;

    for (way = 0; way < GNW_M7_DCACHE_WAYS; way++) {
        if (dcache_line(d, set, way)->valid) {
            return true;
        }
    }
    return false;
}

static void dcache_set_unknown(GnwM7DCacheState *d, unsigned set,
                               bool membership_unknown)
{
    if (!d->sets[set].association_unknown) {
        d->sets[set].association_unknown = true;
        d->uncertain_sets++;
    }
    d->sets[set].membership_unknown |= membership_unknown;
}

static void dcache_clear_uncertainty(GnwM7DCacheState *d, unsigned set)
{
    if (d->sets[set].association_unknown) {
        g_assert(d->uncertain_sets);
        d->uncertain_sets--;
    }
    d->sets[set].association_unknown = false;
    d->sets[set].membership_unknown = false;
}

static bool dcache_set_dirty(GnwM7DCacheState *d, unsigned set)
{
    unsigned way;

    for (way = 0; way < GNW_M7_DCACHE_WAYS; way++) {
        GnwM7DCacheLine *line = dcache_line(d, set, way);

        if (line->valid && line->dirty) {
            return true;
        }
    }
    return false;
}

static void dcache_abandon_setway_sweep(GnwM7DCacheState *d, unsigned set)
{
    GnwM7DCacheSet *set_state = &d->sets[set];

    if (set_state->unclean_invalidated_way_mask &&
        dcache_set_dirty(d, set)) {
        d->ambiguous_accesses++;
    }
    set_state->invalidated_way_mask = 0;
    set_state->cleaned_way_mask = 0;
    set_state->unclean_invalidated_way_mask = 0;
}

static void dcache_clean_set(GnwM7DCacheState *d, unsigned set)
{
    unsigned way;

    for (way = 0; way < GNW_M7_DCACHE_WAYS; way++) {
        GnwM7DCacheLine *line = dcache_line(d, set, way);

        if (line->valid && line->dirty) {
            line->dirty = false;
            g_assert(d->dirty_lines);
            d->dirty_lines--;
        }
    }
}

static void dcache_clear_line(GnwM7DCacheState *d, unsigned set,
                              unsigned way, bool discard_dirty)
{
    GnwM7DCacheLine *line = dcache_line(d, set, way);

    if (!line->valid) {
        return;
    }
    if (line->dirty) {
        g_assert(d->dirty_lines);
        d->dirty_lines--;
        if (discard_dirty) {
            d->dirty_discards++;
        }
    }
    g_assert(d->valid_lines);
    d->valid_lines--;
    memset(line, 0, sizeof(*line));
}

static void dcache_clear_set(GnwM7DCacheState *d, unsigned set,
                             bool discard_dirty)
{
    unsigned way;

    for (way = 0; way < GNW_M7_DCACHE_WAYS; way++) {
        dcache_clear_line(d, set, way, discard_dirty);
    }
    dcache_clear_uncertainty(d, set);
    memset(&d->sets[set], 0, sizeof(d->sets[set]));
    if (d->policy == GNW_M7_DCACHE_TRUE_LRU_HYPOTHESIS) {
        d->sets[set].replacement_state = GNW_M7_LRU_RESET_STATE;
    }
}

static int dcache_find_way(GnwM7DCacheState *d, unsigned set,
                           uint64_t line_base)
{
    unsigned way;

    for (way = 0; way < GNW_M7_DCACHE_WAYS; way++) {
        GnwM7DCacheLine *line = dcache_line(d, set, way);

        if (line->valid && line->line_base == line_base) {
            return way;
        }
    }
    return -1;
}

static int dcache_find_invalid(GnwM7DCacheState *d, unsigned set)
{
    unsigned way;

    for (way = 0; way < GNW_M7_DCACHE_WAYS; way++) {
        if (!dcache_line(d, set, way)->valid) {
            return way;
        }
    }
    return -1;
}

static unsigned dcache_tree_plru_victim(uint8_t state)
{
    if (!(state & 1)) {
        return state & 2 ? 1 : 0;
    }
    return state & 4 ? 3 : 2;
}

static uint8_t dcache_tree_plru_touch(uint8_t state, unsigned way)
{
    switch (way) {
    case 0:
        return (state | 3) & 7;
    case 1:
        return (state | 1) & ~2;
    case 2:
        return (state & ~1) | 4;
    default:
        return state & ~5;
    }
}

static unsigned dcache_lru_victim(uint8_t state)
{
    return (state >> 6) & 3;
}

static uint8_t dcache_lru_touch(uint8_t state, unsigned way)
{
    unsigned order[GNW_M7_DCACHE_WAYS];
    unsigned i, out = 1;
    uint8_t result = way;

    for (i = 0; i < GNW_M7_DCACHE_WAYS; i++) {
        order[i] = (state >> (i * 2)) & 3;
        if (order[i] != way) {
            result |= order[i] << (out++ * 2);
        }
    }
    g_assert(out == GNW_M7_DCACHE_WAYS);
    return result;
}

static void dcache_replacement_touch(GnwM7DCacheState *d, unsigned set,
                                     unsigned way, bool allocation)
{
    GnwM7DCacheSet *set_state = &d->sets[set];

    switch (d->policy) {
    case GNW_M7_DCACHE_ROUND_ROBIN_HYPOTHESIS:
        if (allocation) {
            set_state->replacement_state = (way + 1) & 3;
        }
        break;
    case GNW_M7_DCACHE_TREE_PLRU_HYPOTHESIS:
        set_state->replacement_state =
            dcache_tree_plru_touch(set_state->replacement_state, way);
        break;
    case GNW_M7_DCACHE_TRUE_LRU_HYPOTHESIS:
        set_state->replacement_state =
            dcache_lru_touch(set_state->replacement_state, way);
        break;
    default:
        break;
    }
}

static int dcache_replacement_victim(GnwM7DCacheState *d, unsigned set)
{
    switch (d->policy) {
    case GNW_M7_DCACHE_ROUND_ROBIN_HYPOTHESIS:
        return d->sets[set].replacement_state & 3;
    case GNW_M7_DCACHE_TREE_PLRU_HYPOTHESIS:
        return dcache_tree_plru_victim(d->sets[set].replacement_state);
    case GNW_M7_DCACHE_TRUE_LRU_HYPOTHESIS:
        return dcache_lru_victim(d->sets[set].replacement_state);
    default:
        return -1;
    }
}

typedef enum GnwM7DCacheLineAccess {
    GNW_M7_DCACHE_LINE_UNKNOWN,
    GNW_M7_DCACHE_LINE_HIT,
    GNW_M7_DCACHE_LINE_MISS,
} GnwM7DCacheLineAccess;

typedef struct GnwM7DCacheMemoryResult {
    bool classified;
    uint32_t miss_lines;
} GnwM7DCacheMemoryResult;

static GnwM7DCacheLineAccess
dcache_access_line(GnwM7DCacheState *d, uint64_t line_base, bool store)
{
    unsigned set = (line_base / GNW_M7_DCACHE_LINE_SIZE) %
                   GNW_M7_DCACHE_SETS;
    GnwM7DCacheLine *line;
    int way;

    d->accesses++;
    if (d->sets[set].association_unknown) {
        dcache_abandon_setway_sweep(d, set);
        d->ambiguous_accesses++;
        return GNW_M7_DCACHE_LINE_UNKNOWN;
    }
    way = dcache_find_way(d, set, line_base);
    if (way >= 0) {
        d->hits++;
        line = dcache_line(d, set, way);
        if (store && !line->dirty) {
            line->dirty = true;
            d->dirty_lines++;
        }
        dcache_replacement_touch(d, set, way, false);
        return GNW_M7_DCACHE_LINE_HIT;
    }

    d->misses++;
    way = dcache_find_invalid(d, set);
    if (way < 0) {
        way = dcache_replacement_victim(d, set);
        if (way < 0) {
            dcache_set_unknown(d, set, true);
            d->ambiguous_accesses++;
            return GNW_M7_DCACHE_LINE_UNKNOWN;
        }
        line = dcache_line(d, set, way);
        if (line->dirty) {
            d->dirty_evictions++;
            g_assert(d->dirty_lines);
            d->dirty_lines--;
        }
    } else {
        line = dcache_line(d, set, way);
        d->valid_lines++;
    }
    line->valid = true;
    line->dirty = store;
    line->line_base = line_base;
    if (store) {
        d->dirty_lines++;
    }
    d->allocations++;
    dcache_replacement_touch(d, set, way, true);
    return GNW_M7_DCACHE_LINE_MISS;
}

static GnwM7DCacheMemoryResult
dcache_apply_memory(GnwM7DCacheState *d,
                    const GnwCycleMemoryObservation *o)
{
    uint64_t first, last, line;
    GnwM7DCacheMemoryResult result = { 0 };
    bool in_axi, in_xip;

    if (d->policy == GNW_M7_DCACHE_OFF) {
        return result;
    }
    if (!d->data_cache_enabled) {
        d->rejected_disabled++;
        return result;
    }
    if ((o->kind != GNW_CYCLE_MEMORY_LOAD &&
         o->kind != GNW_CYCLE_MEMORY_STORE) || !o->size) {
        d->rejected_path++;
        return result;
    }
    in_axi = o->physical_address >= GNW_M7_AXISRAM1_BASE &&
             o->physical_address < GNW_M7_AXISRAM1_END &&
             o->size <= GNW_M7_AXISRAM1_END - o->physical_address;
    in_xip = o->physical_address >= GNW_M7_XIP_BASE &&
             o->physical_address < GNW_M7_XIP_END &&
             o->size <= GNW_M7_XIP_END - o->physical_address;
    /* XIP is backed by writable RAM in the mechanism tests, but actual NOR
     * programming uses the indirect controller path.  Do not let an
     * artificial guest store seed the diagnostic cache shadow. */
    if (!in_axi && (!in_xip || o->kind != GNW_CYCLE_MEMORY_LOAD)) {
        d->rejected_path++;
        return result;
    }
    if (!o->pmsa_valid || o->pmsa_source != GNW_CYCLE_PMSA_REGION ||
        o->pmsa_attrs != GNW_CYCLE_PMSA_ATTR_NORMAL_WBWA_NSH) {
        d->rejected_attributes++;
        return result;
    }
    result.classified = true;
    first = o->physical_address & ~(uint64_t)(GNW_M7_DCACHE_LINE_SIZE - 1);
    last = (o->physical_address + o->size - 1) &
           ~(uint64_t)(GNW_M7_DCACHE_LINE_SIZE - 1);
    for (line = first; ; line += GNW_M7_DCACHE_LINE_SIZE) {
        GnwM7DCacheLineAccess access =
            dcache_access_line(d, line, o->kind == GNW_CYCLE_MEMORY_STORE);

        if (access == GNW_M7_DCACHE_LINE_UNKNOWN) {
            result.classified = false;
        } else if (access == GNW_M7_DCACHE_LINE_MISS) {
            result.miss_lines++;
        }
        if (line == last) {
            break;
        }
    }
    return result;
}

static void dcache_apply_mva(GnwM7DCacheState *d,
                             GnwCycleCacheMaintenanceOp op,
                             uint32_t operand)
{
    uint64_t line_base = operand & ~(uint64_t)(GNW_M7_DCACHE_LINE_SIZE - 1);
    unsigned set = (line_base / GNW_M7_DCACHE_LINE_SIZE) %
                   GNW_M7_DCACHE_SETS;
    GnwM7DCacheLine *line;
    int way;

    if (d->sets[set].association_unknown) {
        dcache_abandon_setway_sweep(d, set);
        d->ambiguous_accesses++;
        return;
    }
    way = dcache_find_way(d, set, line_base);
    if (way < 0) {
        return;
    }
    line = dcache_line(d, set, way);
    if (op == GNW_CYCLE_CACHE_DCCMVAU ||
        op == GNW_CYCLE_CACHE_DCCMVAC ||
        op == GNW_CYCLE_CACHE_DCCIMVAC) {
        if (line->dirty) {
            line->dirty = false;
            g_assert(d->dirty_lines);
            d->dirty_lines--;
        }
    }
    if (op == GNW_CYCLE_CACHE_DCIMVAC ||
        op == GNW_CYCLE_CACHE_DCCIMVAC) {
        dcache_clear_line(d, set, way, op == GNW_CYCLE_CACHE_DCIMVAC);
    }
}

static void dcache_apply_uncertain_setway(GnwM7DCacheState *d, unsigned set,
                                          unsigned way, bool clean,
                                          bool invalidate)
{
    GnwM7DCacheSet *set_state = &d->sets[set];
    uint8_t bit = 1U << way;

    if (clean) {
        set_state->cleaned_way_mask |= bit;
    }
    if (invalidate) {
        if (!clean && !(set_state->cleaned_way_mask & bit)) {
            set_state->unclean_invalidated_way_mask |= bit;
        }
        set_state->invalidated_way_mask |= bit;
        set_state->membership_unknown = true;
    }
    if (set_state->cleaned_way_mask == 0xf) {
        if (set_state->unclean_invalidated_way_mask &&
            dcache_set_dirty(d, set)) {
            d->ambiguous_accesses++;
        }
        dcache_clean_set(d, set);
        set_state->cleaned_way_mask = 0;
        if (!set_state->membership_unknown &&
            !set_state->invalidated_way_mask &&
            !set_state->unclean_invalidated_way_mask) {
            dcache_clear_uncertainty(d, set);
        }
    }
    if (set_state->invalidated_way_mask == 0xf) {
        uint8_t unclean = set_state->unclean_invalidated_way_mask;

        if (unclean != 0 && unclean != 0xf &&
            dcache_set_dirty(d, set)) {
            d->ambiguous_accesses++;
        }
        dcache_clear_set(d, set, unclean == 0xf);
    }
}

static void dcache_apply_setway(GnwM7DCacheState *d,
                                GnwCycleCacheMaintenanceOp op,
                                uint32_t operand)
{
    unsigned set = (operand >> 5) & (GNW_M7_DCACHE_SETS - 1);
    unsigned way = (operand >> 30) & (GNW_M7_DCACHE_WAYS - 1);
    GnwM7DCacheLine *line;
    bool invalidate = op == GNW_CYCLE_CACHE_DCISW ||
                      op == GNW_CYCLE_CACHE_DCCISW;
    bool clean = op == GNW_CYCLE_CACHE_DCCSW ||
                 op == GNW_CYCLE_CACHE_DCCISW;

    if (d->policy == GNW_M7_DCACHE_TAGS_NO_REPLACEMENT) {
        if (!dcache_set_nonempty(d, set)) {
            return;
        }
        dcache_set_unknown(d, set, false);
        dcache_apply_uncertain_setway(d, set, way, clean, invalidate);
        return;
    }
    if (d->sets[set].association_unknown) {
        dcache_apply_uncertain_setway(d, set, way, clean, invalidate);
        return;
    }
    line = dcache_line(d, set, way);
    if (clean && line->valid && line->dirty) {
        line->dirty = false;
        g_assert(d->dirty_lines);
        d->dirty_lines--;
    }
    if (invalidate) {
        dcache_clear_line(d, set, way, !clean);
    }
}

static void dcache_apply_maintenance(
    GnwM7DCacheState *d,
    const GnwCycleCacheMaintenanceObservation *o)
{
    if (d->policy == GNW_M7_DCACHE_OFF) {
        return;
    }
    if (o->op <= GNW_CYCLE_CACHE_BPIALL) {
        d->maintenance[o->op]++;
    }
    switch (o->op) {
    case GNW_CYCLE_CACHE_DCIMVAC:
    case GNW_CYCLE_CACHE_DCCMVAU:
    case GNW_CYCLE_CACHE_DCCMVAC:
    case GNW_CYCLE_CACHE_DCCIMVAC:
        dcache_apply_mva(d, o->op, o->operand);
        break;
    case GNW_CYCLE_CACHE_DCISW:
    case GNW_CYCLE_CACHE_DCCSW:
    case GNW_CYCLE_CACHE_DCCISW:
        dcache_apply_setway(d, o->op, o->operand);
        break;
    default:
        /* I-cache and branch-predictor state have no retired-fetch backend. */
        break;
    }
}

static void dcache_apply_control(
    GnwM7DCacheState *d, const GnwCycleCacheControlObservation *o)
{
    unsigned set;

    if (d->policy == GNW_M7_DCACHE_OFF) {
        return;
    }
    d->instruction_cache_enabled = o->new_instruction_cache_enabled;
    if (d->data_cache_enabled != o->new_data_cache_enabled) {
        for (set = 0; set < GNW_M7_DCACHE_SETS; set++) {
            if (dcache_set_nonempty(d, set)) {
                dcache_abandon_setway_sweep(d, set);
                dcache_set_unknown(d, set, false);
            }
        }
    }
    d->data_cache_enabled = o->new_data_cache_enabled;
}

static void dcache_queue(GnwM7DCacheState *d,
                         const GnwM7DCachePendingEvent *event)
{
    g_assert(d->pending_count < GNW_M7_DCACHE_PENDING_EVENTS);
    d->pending[d->pending_count++] = *event;
}

static void dcache_commit_pending(GnwM7DCacheState *d)
{
    unsigned i;

    /* Diagnostic state only: applying these events never alters cycle cost. */
    for (i = 0; i < d->pending_count; i++) {
        GnwM7DCachePendingEvent *event = &d->pending[i];

        switch (event->type) {
        case GNW_M7_DCACHE_PENDING_MEMORY:
            dcache_apply_memory(d, &event->u.memory);
            break;
        case GNW_M7_DCACHE_PENDING_MAINTENANCE:
            dcache_apply_maintenance(d, &event->u.maintenance);
            break;
        case GNW_M7_DCACHE_PENDING_CONTROL:
            dcache_apply_control(d, &event->u.control);
            break;
        default:
            g_assert_not_reached();
        }
    }
    d->pending_count = 0;
}

void gnw_cycle_model_memory(
    CPUState *cpu, const GnwCycleMemoryObservation *observation)
{
    gnw_cycle_model_memory_completed(cpu, observation);
}

void gnw_cycle_model_memory_completed(
    CPUState *cpu, const GnwCycleMemoryObservation *observation)
{
    GnwCycleModelState *s = &cpu->gnw_cycle_model;
    uint32_t attributes;
    GnwM7DCacheMemoryResult cache_result;
    unsigned load_region = 0;
    unsigned load_classification = 0;
    bool load_accepted = false;

    if (!s->enabled) {
        return;
    }
    g_assert(s->pending_valid);
    g_assert(observation->translation_valid);
    g_assert(observation->access_count > 0);
    g_assert(observation->access_index < observation->access_count);
    attributes =
        (observation->pmsa_attrs & GNW_CYCLE_MEMATTR_PMSA_MASK) |
        (observation->secure ? GNW_CYCLE_MEMATTR_SECURE : 0) |
        (observation->user ? GNW_CYCLE_MEMATTR_USER : 0) |
        ((uint32_t)observation->pmsa_region <<
         GNW_CYCLE_MEMATTR_REGION_SHIFT) |
        ((observation->mmu_idx & 0xff) << GNW_CYCLE_MEMATTR_MMU_IDX_SHIFT) |
        ((uint32_t)observation->pmsa_source <<
         GNW_CYCLE_MEMATTR_SOURCE_SHIFT) |
        (observation->pmsa_valid ? GNW_CYCLE_MEMATTR_PMSA_VALID : 0);

    /*
     * This is a successful QEMU logical data operation.  Commit its
     * diagnostic cache effect now so a later transfer fault in an LDM/STM
     * cannot erase effects of earlier completed transfers.  Instruction
     * issue/cost state remains pending and is still handled by finish/abort.
     */
    s->pending.memory_address = observation->physical_address;
    s->pending.memory_attributes = attributes;
    s->pending.region_load_latency_class =
        gnw_m7_region_load_latency_class(&s->pending, observation);
    /* Memory instructions normally take the conservative serialized fallback
     * because their latency is not modeled.  The calibrated scalar-load
     * candidate has a resolved result latency and register dependencies, so
     * admit only those completed region-classified loads to the scoreboard.
     * All other memory shapes and regions retain the legacy fallback. */
    if (s->pending.region_load_latency_class) {
        s->pending.flags |= GNW_CYCLE_META_SUPPORTED;
        s->pending.fallback_reason = GNW_CYCLE_FALLBACK_NONE;
    }
    cache_result = dcache_apply_memory(&s->dcache, observation);

    /*
     * Replay only the v5 profile's exact successful scalar register-offset
     * word-load shape.  This diagnostic timeline is deliberately disjoint
     * from cycle_fp and icount: its fitted scale includes omitted loop work.
     */
    if (gnw_m7_load_path_observation_eligible(&s->pending, observation)) {
        bool dtcm = observation->physical_address >= 0x20000000ULL &&
                    observation->physical_address < 0x20020000ULL;
        bool axi_nc = observation->physical_address >= 0x24000000ULL &&
                      observation->physical_address < 0x24080000ULL &&
                      observation->pmsa_valid &&
                      observation->pmsa_source == GNW_CYCLE_PMSA_REGION &&
                      observation->pmsa_attrs == 0x08;
        bool axi_wbwa = observation->physical_address >=
                            GNW_M7_AXISRAM1_BASE &&
                        observation->physical_address <
                            GNW_M7_AXISRAM1_END &&
                        cache_result.classified;

        if (dtcm || axi_nc || axi_wbwa) {
            load_region = dtcm ? 1 : axi_nc ? 2 : 3;
            load_classification = axi_wbwa ?
                (cache_result.miss_lines ? 2 : 1) : 3;
            load_accepted = gnw_m7_load_path_observe(
                &s->load_path, s->pending.address_reads,
                s->pending.gpr_writes, axi_wbwa,
                axi_wbwa && cache_result.miss_lines);
        } else {
            s->load_path.rejected_region++;
        }
    } else if (observation->kind == GNW_CYCLE_MEMORY_LOAD) {
        s->load_path.rejected_shape++;
    }
    if (observation->kind == GNW_CYCLE_MEMORY_LOAD) {
        trace_gnw_cycle_load_path(
            cpu->cpu_index, s->pending.pc, load_region, load_classification,
            load_accepted, load_accepted ? s->load_path.last_issue_fp : 0,
            load_accepted ? s->load_path.last_ready_fp : 0,
            s->load_path.epoch_cycles, s->load_path.critical_path_fp);
    }
    if ((observation->kind == GNW_CYCLE_MEMORY_LOAD ||
         observation->kind == GNW_CYCLE_MEMORY_STORE) &&
        (s->bus_contention_profile_identity ==
             GNW_M7_BUS_CONTENTION_PROFILE_TEST_FIXED ||
         s->bus_contention_profile_identity ==
             GNW_M7_BUS_CONTENTION_PROFILE_V37_BWM_COMPARE ||
         s->bus_contention_profile_identity ==
             GNW_M7_BUS_CONTENTION_PROFILE_V46_BWM_17_4 ||
         (observation->kind == GNW_CYCLE_MEMORY_LOAD &&
          observation->size == 4)) &&
        observation->access_count == 1 && !observation->access_flags &&
        observation->physical_address >= GNW_M7_AXISRAM1_BASE &&
        observation->physical_address < GNW_M7_AXISRAM1_END &&
        observation->size <= GNW_M7_AXISRAM1_END -
                             observation->physical_address &&
        s->bus_contention_profile_identity != GNW_M7_BUS_CONTENTION_PROFILE_OFF) {
        bool v37_bwm_shape =
            s->bus_contention_profile_identity ==
                GNW_M7_BUS_CONTENTION_PROFILE_V37_BWM_COMPARE &&
            gnw_m7_bus_contention_v37_bwm_load_eligible(&s->pending,
                                                        observation) &&
            observation->pmsa_valid &&
            observation->pmsa_source == GNW_CYCLE_PMSA_REGION &&
            observation->pmsa_attrs ==
                GNW_M7_BUS_CONTENTION_MDMA_AXI1_PMSA_ATTRS;
        bool v46_bwm_shape =
            s->bus_contention_profile_identity ==
                GNW_M7_BUS_CONTENTION_PROFILE_V46_BWM_17_4 &&
            gnw_m7_bus_contention_v46_bwm_load_eligible(&s->pending,
                                                        observation);
        bool mdma_v1_shape =
            s->bus_contention_profile_identity ==
                GNW_M7_BUS_CONTENTION_PROFILE_MDMA_AXI1_V1 &&
            gnw_m7_bus_contention_load_eligible(&s->pending, observation) &&
            observation->physical_address >=
                GNW_M7_BUS_CONTENTION_MDMA_AXI1_VICTIM_BASE &&
            observation->physical_address <
                GNW_M7_BUS_CONTENTION_MDMA_AXI1_VICTIM_END &&
            observation->size <=
                GNW_M7_BUS_CONTENTION_MDMA_AXI1_VICTIM_END -
                observation->physical_address &&
            observation->translation_valid && observation->pmsa_valid &&
            observation->pmsa_source == GNW_CYCLE_PMSA_REGION &&
            observation->pmsa_attrs ==
                GNW_M7_BUS_CONTENTION_MDMA_AXI1_PMSA_ATTRS;

        /* The calibrated profile is deliberately narrower than the
         * mechanism-test profile: hardware only measured dependent 32-bit
         * Normal-NC loads in this frozen victim window. */
        if (s->bus_contention_profile_identity ==
                GNW_M7_BUS_CONTENTION_PROFILE_MDMA_AXI1_V1 &&
            !mdma_v1_shape) {
            goto skip_bus_contention;
        }
        if (s->bus_contention_profile_identity ==
                GNW_M7_BUS_CONTENTION_PROFILE_V37_BWM_COMPARE &&
            !v37_bwm_shape) {
            goto skip_bus_contention;
        }
        if (s->bus_contention_profile_identity ==
                GNW_M7_BUS_CONTENTION_PROFILE_V46_BWM_17_4 &&
            !v46_bwm_shape) {
            goto skip_bus_contention;
        }
        s->bus_contention_observations++;
        int64_t guest_time_ns = 0;
        if (s->bus_contention_profile_identity ==
                GNW_M7_BUS_CONTENTION_PROFILE_V37_BWM_COMPARE ||
            s->bus_contention_profile_identity ==
                GNW_M7_BUS_CONTENTION_PROFILE_V46_BWM_17_4) {
            guest_time_ns = icount_get_committed_time_ns();
        }
        uint32_t masters = s->bus_contention_probe ?
            s->bus_contention_probe(s->bus_contention_probe_opaque,
                                    observation->physical_address,
                                    observation->size, guest_time_ns) : 0;
        uint32_t admitted = s->bus_contention_profile_identity ==
            GNW_M7_BUS_CONTENTION_PROFILE_MDMA_AXI1_V1 ?
            GNW_M7_BUS_MASTER_MDMA_LARGE :
            (s->bus_contention_profile_identity ==
                 GNW_M7_BUS_CONTENTION_PROFILE_V37_BWM_COMPARE ?
             GNW_M7_BUS_MASTER_MDMA_BWM_COMPARE :
             (s->bus_contention_profile_identity ==
                  GNW_M7_BUS_CONTENTION_PROFILE_V46_BWM_17_4 ?
             GNW_M7_BUS_MASTER_MDMA_BWM_COMPARE :
             (GNW_M7_BUS_MASTER_MDMA | GNW_M7_BUS_MASTER_DMA2D)));

        if (masters & admitted) {
            s->bus_contention_matches++;
            uint64_t cycles=GNW_M7_BUS_CONTENTION_TEST_CYCLES;
            if (s->bus_contention_profile_identity == GNW_M7_BUS_CONTENTION_PROFILE_MDMA_AXI1_V1) {
                uint64_t total=s->bus_contention_fraction + GNW_M7_BUS_CONTENTION_MDMA_AXI1_NUMERATOR;
                cycles=total / GNW_M7_BUS_CONTENTION_MDMA_AXI1_DENOMINATOR;
                s->bus_contention_fraction=total % GNW_M7_BUS_CONTENTION_MDMA_AXI1_DENOMINATOR;
            } else if (s->bus_contention_profile_identity ==
                       GNW_M7_BUS_CONTENTION_PROFILE_V37_BWM_COMPARE) {
                uint64_t total = s->bus_contention_fraction +
                    GNW_M7_BUS_CONTENTION_V37_BWM_COMPARE_NUMERATOR;
                cycles = total /
                    GNW_M7_BUS_CONTENTION_V37_BWM_COMPARE_DENOMINATOR;
                s->bus_contention_fraction = total %
                    GNW_M7_BUS_CONTENTION_V37_BWM_COMPARE_DENOMINATOR;
            } else if (s->bus_contention_profile_identity ==
                       GNW_M7_BUS_CONTENTION_PROFILE_V46_BWM_17_4) {
                uint64_t total = s->bus_contention_fraction +
                    GNW_M7_BUS_CONTENTION_V46_BWM_NUMERATOR;
                cycles = total /
                    GNW_M7_BUS_CONTENTION_V46_BWM_DENOMINATOR;
                s->bus_contention_fraction = total %
                    GNW_M7_BUS_CONTENTION_V46_BWM_DENOMINATOR;
            }
            s->bus_contention_cycles += cycles;
            s->pending_bus_contention_cycles += cycles;
        }
    }
skip_bus_contention:
    if (s->xip_service_profile_identity ==
        GNW_M7_XIP_SERVICE_PROFILE_UNSEEN_V1) {
        xip_unseen_service_observe(cpu, observation);
    }
    if (observation->kind == GNW_CYCLE_MEMORY_LOAD &&
        observation->access_count == 1 && !observation->access_flags) {
        if (observation->physical_address >= GNW_M7_XIP_BASE &&
            observation->physical_address < GNW_M7_XIP_END &&
            cache_result.classified) {
            uint32_t i;

            for (i = 0; i < cache_result.miss_lines; i++) {
                xip_service_request(cpu, observation->physical_address,
                                    observation->size);
            }
        } else {
            /* Preserve the conservative legacy request if the cache shadow
             * cannot classify this XIP access with certainty. */
            xip_service_request(cpu, observation->physical_address,
                                observation->size);
        }
    }

    trace_gnw_cycle_memory(cpu->cpu_index, s->pending.pc,
                           observation->virtual_address,
                           observation->physical_address,
                           observation->kind, observation->size,
                           observation->address_reads,
                           observation->store_data_reads);
    trace_gnw_cycle_memory_attrs(cpu->cpu_index, s->pending.pc,
                           observation->memop, observation->mmu_idx,
                           observation->pmsa_valid,
                           observation->pmsa_region,
                           observation->pmsa_attrs,
                           observation->pmsa_source,
                           observation->secure, observation->user);
    trace_gnw_cycle_memory_completion(cpu->cpu_index, s->pending.pc,
                           observation->access_index,
                           observation->access_count,
                           observation->access_flags,
                           observation->store_succeeded);
}

static bool fetch_residual_matches(const GnwCycleFetchObservation *observation,
                                   uint64_t address, const uint8_t *bytes,
                                   size_t size)
{
    return observation->virtual_address == address &&
           observation->physical_address == address &&
           observation->size == size &&
           observation->pmsa_valid &&
           observation->pmsa_source == GNW_CYCLE_PMSA_REGION &&
           observation->pmsa_region == 3 && observation->pmsa_attrs == 0 &&
           !observation->known_cacheable &&
           !memcmp(observation->bytes, bytes, size);
}

static void fetch_residual_charge(GnwCycleModelState *s, uint64_t cycles)
{
    g_assert(cycles <= UINT32_MAX - s->pending_fetch_residual_cycles);
    s->pending_fetch_residual_cycles += cycles;
    if (s->fetch_residual_charges != UINT64_MAX) {
        s->fetch_residual_charges++;
    }
    if (cycles <= UINT64_MAX - s->fetch_residual_cycles) {
        s->fetch_residual_cycles += cycles;
    } else {
        s->fetch_residual_cycles = UINT64_MAX;
    }
}

static void fetch_residual_charge_rational(GnwCycleModelState *s,
                                           uint64_t numerator)
{
    uint64_t total = s->fetch_residual_fraction + numerator;

    fetch_residual_charge(s, total / GNW_M7_FETCH_RESIDUAL_DENOMINATOR);
    s->fetch_residual_fraction =
        total % GNW_M7_FETCH_RESIDUAL_DENOMINATOR;
}

static void fetch_residual_observe(
    GnwCycleModelState *s, const GnwCycleFetchObservation *observation)
{
    static const uint8_t hot_entry[] = { 0x0b, 0x21 };
    static const uint8_t periodic_entry[] = { 0x70, 0xb5 };
    static const uint8_t periodic_iteration[] = { 0x05, 0xeb, 0x45, 0x15 };
    static const uint8_t periodic_invalidation[] = { 0x00, 0x20 };
    static const uint8_t set_entry[] = { 0xf0, 0xb5 };
    static const uint8_t set_iteration[] = { 0xa8, 0x47 };
    static const uint8_t set_return[] = { 0xf0, 0xbd };

    if (s->fetch_residual_profile_identity !=
            GNW_M7_FETCH_RESIDUAL_PROFILE_V1) {
        return;
    }
    if (fetch_residual_matches(observation, GNW_FETCH_HOT_ENTRY,
                               hot_entry, sizeof(hot_entry))) {
        fetch_residual_charge(s, GNW_FETCH_HOT_SHARED_CYCLES);
    } else if (fetch_residual_matches(observation, GNW_FETCH_PERIODIC_ENTRY,
                                      periodic_entry,
                                      sizeof(periodic_entry))) {
        fetch_residual_charge(s, GNW_FETCH_PERIODIC_ENTRY_CYCLES);
    } else if (fetch_residual_matches(
                   observation, GNW_FETCH_PERIODIC_ITERATION,
                   periodic_iteration, sizeof(periodic_iteration))) {
        fetch_residual_charge(s, GNW_FETCH_PERIODIC_ITER_CYCLES);
    } else if (fetch_residual_matches(
                   observation, GNW_FETCH_PERIODIC_INVALIDATION,
                   periodic_invalidation, sizeof(periodic_invalidation))) {
        fetch_residual_charge(s, GNW_FETCH_PERIODIC_INV_CYCLES);
    } else if (fetch_residual_matches(observation, GNW_FETCH_SET_ENTRY,
                                      set_entry, sizeof(set_entry))) {
        s->fetch_residual_fraction = 0;
        s->fetch_residual_set_conflict_active = true;
        fetch_residual_charge_rational(s, GNW_FETCH_SET_ENTRY_NUMERATOR);
    } else if (s->fetch_residual_set_conflict_active &&
               fetch_residual_matches(observation, GNW_FETCH_SET_ITERATION,
                                      set_iteration,
                                      sizeof(set_iteration))) {
        fetch_residual_charge_rational(s, GNW_FETCH_SET_ITER_NUMERATOR);
    } else if (s->fetch_residual_set_conflict_active &&
               fetch_residual_matches(observation, GNW_FETCH_SET_RETURN,
                                      set_return, sizeof(set_return))) {
        s->fetch_residual_set_conflict_active = false;
    }
}

static void memory_residual_charge(GnwCycleModelState *s, uint32_t cycles)
{
    g_assert(cycles <= UINT32_MAX - s->pending_memory_residual_cycles);
    s->pending_memory_residual_cycles += cycles;
    if (s->memory_residual_charges != UINT64_MAX) {
        s->memory_residual_charges++;
    }
    if (cycles <= UINT64_MAX - s->memory_residual_cycles) {
        s->memory_residual_cycles += cycles;
    } else {
        s->memory_residual_cycles = UINT64_MAX;
    }
}

static bool memory_residual_arguments(
    const GnwCycleFetchObservation *observation, uint32_t *intercept)
{
    uint32_t base = observation->gpr0;
    uint32_t bytes = observation->gpr1;
    uint32_t stride = observation->gpr2;

    if (bytes < GNW_MEMORY_MIN_BYTES || bytes > GNW_MEMORY_MAX_BYTES) {
        return false;
    }
    if ((base == GNW_MEMORY_DTCM_BASE || base == GNW_MEMORY_AXI_BASE) &&
        stride == 1) {
        *intercept = base == GNW_MEMORY_DTCM_BASE ? 56 : 58;
        return true;
    }
    if ((base == GNW_MEMORY_AHB_FIT_BASE ||
         base == GNW_MEMORY_AHB_HOLD_BASE) &&
        (stride == 1 || stride == 33)) {
        *intercept = stride == 1 ? 58 : 490;
        return true;
    }
    return false;
}

static bool memory_residual_body_matches(
    CPUState *cpu, const GnwCycleFetchObservation *observation)
{
    uint8_t body[GNW_M7_MEMORY_RESIDUAL_BODY_SIZE];

    /* The entry provenance already proves identity-mapped noncacheable AXI
     * SRAM.  Read the complete linked body before charging any retained-fit
     * coefficient so a lookalike prologue cannot enter the exact profile. */
    return cpu_memory_rw_debug(cpu, observation->virtual_address, body,
                               sizeof(body), false) == 0 &&
           gnw_m7_memory_residual_body_matches(body, sizeof(body));
}

static void memory_residual_observe(
    CPUState *cpu, GnwCycleModelState *s,
    const GnwCycleFetchObservation *observation)
{
    static const uint8_t iteration[] = { 0x9b, 0x19 };
    static const uint8_t function_return[] = { 0xf0, 0xbd };
    uint32_t intercept;

    if (s->memory_residual_profile_identity !=
            GNW_M7_MEMORY_RESIDUAL_PROFILE_V1) {
        return;
    }
    if (gnw_m7_memory_residual_entry_matches(observation) &&
        memory_residual_body_matches(cpu, observation) &&
        memory_residual_arguments(observation, &intercept)) {
        if (s->memory_residual_active) {
            if (s->memory_residual_rejected != UINT64_MAX) {
                s->memory_residual_rejected++;
            }
            s->memory_residual_active = false;
            s->memory_residual_remaining = 0;
        }
        s->memory_residual_entry_pc = observation->virtual_address;
        s->memory_residual_base = observation->gpr0;
        s->memory_residual_bytes = observation->gpr1;
        s->memory_residual_stride = observation->gpr2;
        s->memory_residual_remaining = observation->gpr1;
        s->memory_residual_active = true;
        if (s->memory_residual_accepted != UINT64_MAX) {
            s->memory_residual_accepted++;
        }
        memory_residual_charge(s, intercept);
    } else if (s->memory_residual_active &&
               gnw_m7_memory_residual_instruction_matches(
                   observation, s->memory_residual_entry_pc,
                   GNW_M7_MEMORY_RESIDUAL_ITERATION_OFFSET,
                   iteration[0], iteration[1])) {
        if (!s->memory_residual_remaining) {
            if (s->memory_residual_rejected != UINT64_MAX) {
                s->memory_residual_rejected++;
            }
            s->memory_residual_active = false;
            s->memory_residual_entry_pc = 0;
            return;
        }
        /* One cycle on iterations 0, 2, ... gives ceil(bytes / 2). */
        if (((s->memory_residual_bytes - s->memory_residual_remaining) & 1)
                == 0) {
            memory_residual_charge(s, 1);
        }
        s->memory_residual_remaining--;
    } else if (s->memory_residual_active &&
               gnw_m7_memory_residual_instruction_matches(
                   observation, s->memory_residual_entry_pc,
                   GNW_M7_MEMORY_RESIDUAL_RETURN_OFFSET,
                   function_return[0], function_return[1])) {
        if (s->memory_residual_remaining) {
            if (s->memory_residual_rejected != UINT64_MAX) {
                s->memory_residual_rejected++;
            }
        }
        s->memory_residual_active = false;
        s->memory_residual_entry_pc = 0;
        s->memory_residual_remaining = 0;
    } else if (s->memory_residual_active &&
               (observation->virtual_address == s->memory_residual_entry_pc +
                    GNW_M7_MEMORY_RESIDUAL_ITERATION_OFFSET ||
                observation->virtual_address == s->memory_residual_entry_pc +
                    GNW_M7_MEMORY_RESIDUAL_RETURN_OFFSET)) {
        if (s->memory_residual_rejected != UINT64_MAX) {
            s->memory_residual_rejected++;
        }
        s->memory_residual_active = false;
        s->memory_residual_entry_pc = 0;
        s->memory_residual_remaining = 0;
    }
}

void gnw_cycle_model_instruction_fetch(
    CPUState *cpu, const GnwCycleFetchObservation *observation)
{
    GnwCycleModelState *s = &cpu->gnw_cycle_model;

    if (!s->enabled || !observation->size) {
        return;
    }
    if (s->icache.attempt_active) {
        gnw_m7_icache_stage_fetch(&s->icache, observation);
    }
    s->instruction_fetch_segments++;
    s->instruction_fetch_bytes += observation->size;
    fetch_residual_observe(s, observation);
    memory_residual_observe(cpu, s, observation);
    if (observation->physical_address >= GNW_M7_AXISRAM1_BASE &&
        observation->physical_address < GNW_M7_AXISRAM1_END &&
        observation->pmsa_valid &&
        observation->pmsa_source == GNW_CYCLE_PMSA_REGION &&
        observation->pmsa_attrs == 0 && !observation->known_cacheable) {
        uint64_t line = observation->physical_address &
            ~(GNW_M7_NC_AXI_FETCH_LINE_SIZE - 1);

        if (s->nc_axi_fetch_valid && s->nc_axi_fetch_line != line) {
            g_assert(s->pending_nc_axi_fetch_cycles <=
                     UINT32_MAX - GNW_M7_NC_AXI_FETCH_TRANSITION_CYCLES);
            s->pending_nc_axi_fetch_cycles +=
                GNW_M7_NC_AXI_FETCH_TRANSITION_CYCLES;
            if (s->nc_axi_fetch_transitions != UINT64_MAX) {
                s->nc_axi_fetch_transitions++;
            }
        }
        s->nc_axi_fetch_line = line;
        s->nc_axi_fetch_valid = true;
    } else {
        s->nc_axi_fetch_line = 0;
        s->nc_axi_fetch_valid = false;
    }
    if (s->flash_schedule_configured &&
        s->flash_schedule_attempt_pending) {
        gnw_flash_schedule_observe(&s->flash_schedule_pending,
                                   s->flash_schedule_identity,
                                   observation);
    }
    if (s->exact_body_identity_configured) {
        bool candidate_start = observation->virtual_address ==
            s->exact_body_identity.virtual_address &&
            observation->physical_address ==
            s->exact_body_identity.physical_address;
        bool was_rejected;

        if (!s->exact_body_match.active && candidate_start) {
            gnw_m7_exact_body_match_begin(
                &s->exact_body_match, &s->exact_body_identity,
                observation->exact_body_region_state_verified);
        }
        if (s->exact_body_match.active) {
            was_rejected = s->exact_body_match.rejected;
            if (gnw_cycle_exact_body_match_fetch(
                    &s->exact_body_match, &s->exact_body_identity,
                    observation)) {
                if (s->exact_body_accepted != UINT64_MAX) {
                    s->exact_body_accepted++;
                }
                if (s->cpu_ram_itcm_profile_identity ==
                        GNW_M7_CPU_RAM_ITCM_PROFILE_V1 &&
                    s->cpu_ram_itcm_profile_accepted != UINT64_MAX) {
                    s->cpu_ram_itcm_profile_accepted++;
                }
                s->exact_body_match.active = false;
            } else if (!was_rejected && s->exact_body_match.rejected) {
                if (s->exact_body_rejected != UINT64_MAX) {
                    s->exact_body_rejected++;
                }
                if (s->cpu_ram_itcm_profile_identity ==
                        GNW_M7_CPU_RAM_ITCM_PROFILE_V1 &&
                    s->cpu_ram_itcm_profile_rejected != UINT64_MAX) {
                    s->cpu_ram_itcm_profile_rejected++;
                }
            }
        } else if (candidate_start && s->exact_body_match.rejected) {
            if (s->exact_body_rejected != UINT64_MAX) {
                s->exact_body_rejected++;
            }
        }
    }
    gnw_flash_fetch_observe(&s->flash_fetch,
                            observation->physical_address,
                            observation->size);
    xip_service_request(cpu, observation->physical_address,
                        observation->size);
    trace_gnw_cycle_instruction_fetch(
        cpu->cpu_index, observation->virtual_address,
        observation->physical_address, observation->size,
        observation->pmsa_valid, observation->pmsa_region,
        observation->pmsa_attrs, observation->pmsa_source,
        observation->known_cacheable,
        (observation->secure ? 1 : 0) | (observation->user ? 2 : 0));
}

void gnw_cycle_model_instruction_fetch_complete(CPUState *cpu)
{
    if (!cpu->gnw_cycle_model.enabled) {
        return;
    }
    if (cpu->gnw_cycle_model.flash_schedule_attempt_pending) {
        cpu->gnw_cycle_model.flash_schedule =
            cpu->gnw_cycle_model.flash_schedule_pending;
        cpu->gnw_cycle_model.flash_schedule_attempt_pending = false;
    }
    gnw_m7_icache_finish_attempt(&cpu->gnw_cycle_model.icache, true);
    gnw_flash_fetch_commit(&cpu->gnw_cycle_model.flash_fetch);
    cpu->gnw_cycle_model.nc_axi_fetch_attempt_active = false;
    cpu->gnw_cycle_model.fetch_residual_attempt_active = false;
}

void gnw_cycle_model_instruction_fetch_attempt(CPUState *cpu)
{
    GnwCycleModelState *s = &cpu->gnw_cycle_model;

    if (s->enabled) {
        s->instruction_fetch_attempts++;
        g_assert(!s->nc_axi_fetch_attempt_active);
        s->nc_axi_fetch_saved_line = s->nc_axi_fetch_line;
        s->nc_axi_fetch_saved_transitions = s->nc_axi_fetch_transitions;
        s->nc_axi_fetch_saved_cycles = s->pending_nc_axi_fetch_cycles;
        s->nc_axi_fetch_saved_valid = s->nc_axi_fetch_valid;
        s->nc_axi_fetch_attempt_active = true;
        g_assert(!s->fetch_residual_attempt_active);
        s->fetch_residual_saved_fraction = s->fetch_residual_fraction;
        s->fetch_residual_saved_charges = s->fetch_residual_charges;
        s->fetch_residual_saved_cycles = s->fetch_residual_cycles;
        s->fetch_residual_saved_pending_cycles =
            s->pending_fetch_residual_cycles;
        s->fetch_residual_saved_set_conflict_active =
            s->fetch_residual_set_conflict_active;
        s->fetch_residual_attempt_active = true;
        s->memory_residual_saved_entry_pc = s->memory_residual_entry_pc;
        s->memory_residual_saved_base = s->memory_residual_base;
        s->memory_residual_saved_bytes = s->memory_residual_bytes;
        s->memory_residual_saved_stride = s->memory_residual_stride;
        s->memory_residual_saved_remaining = s->memory_residual_remaining;
        s->memory_residual_saved_pending_cycles =
            s->pending_memory_residual_cycles;
        s->memory_residual_saved_charges = s->memory_residual_charges;
        s->memory_residual_saved_cycles = s->memory_residual_cycles;
        s->memory_residual_saved_accepted = s->memory_residual_accepted;
        s->memory_residual_saved_rejected = s->memory_residual_rejected;
        s->memory_residual_saved_active = s->memory_residual_active;
        g_assert(!s->icache.attempt_active && !s->icache.pending_count);
        s->icache.pending_overflow = false;
        s->icache.attempt_active = true;
        if (s->flash_schedule_configured) {
            s->flash_schedule_pending = s->flash_schedule;
            s->flash_schedule_attempt_pending = true;
        }
        gnw_flash_fetch_cancel(&s->flash_fetch);
    }
}

void gnw_cycle_model_instruction_fetch_incomplete(CPUState *cpu)
{
    GnwCycleModelState *s = &cpu->gnw_cycle_model;

    if (s->enabled) {
        s->instruction_fetch_incomplete++;
        gnw_m7_icache_finish_attempt(&s->icache, false);
        s->flash_schedule_attempt_pending = false;
        gnw_flash_fetch_cancel(&s->flash_fetch);
        g_assert(s->nc_axi_fetch_attempt_active);
        s->nc_axi_fetch_line = s->nc_axi_fetch_saved_line;
        s->nc_axi_fetch_transitions = s->nc_axi_fetch_saved_transitions;
        s->pending_nc_axi_fetch_cycles = s->nc_axi_fetch_saved_cycles;
        s->nc_axi_fetch_valid = s->nc_axi_fetch_saved_valid;
        s->nc_axi_fetch_attempt_active = false;
        g_assert(s->fetch_residual_attempt_active);
        s->fetch_residual_fraction = s->fetch_residual_saved_fraction;
        s->fetch_residual_charges = s->fetch_residual_saved_charges;
        s->fetch_residual_cycles = s->fetch_residual_saved_cycles;
        s->pending_fetch_residual_cycles =
            s->fetch_residual_saved_pending_cycles;
        s->fetch_residual_set_conflict_active =
            s->fetch_residual_saved_set_conflict_active;
        s->fetch_residual_attempt_active = false;
        s->memory_residual_entry_pc = s->memory_residual_saved_entry_pc;
        s->memory_residual_base = s->memory_residual_saved_base;
        s->memory_residual_bytes = s->memory_residual_saved_bytes;
        s->memory_residual_stride = s->memory_residual_saved_stride;
        s->memory_residual_remaining = s->memory_residual_saved_remaining;
        s->pending_memory_residual_cycles =
            s->memory_residual_saved_pending_cycles;
        s->memory_residual_charges = s->memory_residual_saved_charges;
        s->memory_residual_cycles = s->memory_residual_saved_cycles;
        s->memory_residual_accepted = s->memory_residual_saved_accepted;
        s->memory_residual_rejected = s->memory_residual_saved_rejected;
        s->memory_residual_active = s->memory_residual_saved_active;
    }
}

void gnw_cycle_model_cache_maintenance(
    CPUState *cpu, const GnwCycleCacheMaintenanceObservation *observation)
{
    GnwCycleModelState *s = &cpu->gnw_cycle_model;
    GnwM7DCachePendingEvent event = {
        .type = GNW_M7_DCACHE_PENDING_MAINTENANCE,
        .u.maintenance = *observation,
    };

    trace_gnw_cycle_cache_maintenance(
        cpu->cpu_index, observation->op, observation->operand,
        observation->instruction_cache_enabled,
        observation->data_cache_enabled);
    if (s->enabled && observation->op == GNW_CYCLE_CACHE_ICIALLU) {
        gnw_flash_fetch_invalidate(&s->flash_fetch, true, 0);
        gnw_m7_icache_invalidate_all(&s->icache);
        /* The 89-cycle service was identified only with I-cache enabled. */
        if (observation->instruction_cache_enabled) {
            g_assert(s->pending_icache_maintenance_cycles <=
                     UINT32_MAX - GNW_M7_ICIALLU_CYCLES);
            s->pending_icache_maintenance_cycles += GNW_M7_ICIALLU_CYCLES;
        }
    } else if (s->enabled && observation->op == GNW_CYCLE_CACHE_ICIMVAU) {
        gnw_flash_fetch_invalidate(&s->flash_fetch, false,
                                   observation->operand);
        gnw_m7_icache_invalidate_line(&s->icache, observation->operand);
    }
    if (s->enabled && (observation->op == GNW_CYCLE_CACHE_ICIALLU ||
                       observation->op == GNW_CYCLE_CACHE_ICIMVAU)) {
        s->nc_axi_fetch_line = 0;
        s->nc_axi_fetch_valid = false;
    }
    if (!s->enabled || s->dcache.policy == GNW_M7_DCACHE_OFF) {
        return;
    }
    if (s->pending_valid) {
        dcache_queue(&s->dcache, &event);
    } else {
        dcache_apply_maintenance(&s->dcache, observation);
        s->dcache.external_events++;
    }
}

void gnw_cycle_model_cache_control(
    CPUState *cpu, const GnwCycleCacheControlObservation *observation)
{
    GnwCycleModelState *s = &cpu->gnw_cycle_model;
    GnwM7DCachePendingEvent event = {
        .type = GNW_M7_DCACHE_PENDING_CONTROL,
        .u.control = *observation,
    };

    trace_gnw_cycle_cache_control(
        cpu->cpu_index, observation->old_instruction_cache_enabled,
        observation->new_instruction_cache_enabled,
        observation->old_data_cache_enabled,
        observation->new_data_cache_enabled);
    if (s->enabled && observation->old_instruction_cache_enabled !=
                      observation->new_instruction_cache_enabled) {
        /* Retention on disable is unmeasured; flush the experimental shadow. */
        gnw_m7_icache_clear(&s->icache);
        s->nc_axi_fetch_line = 0;
        s->nc_axi_fetch_valid = false;
    }
    if (!s->enabled || s->dcache.policy == GNW_M7_DCACHE_OFF) {
        return;
    }
    if (s->pending_valid) {
        dcache_queue(&s->dcache, &event);
    } else {
        dcache_apply_control(&s->dcache, observation);
        s->dcache.external_events++;
    }
}

static uint64_t metadata_latency(const GnwCycleInsnMetadata *m)
{
    if (m->memory_kind == GNW_CYCLE_MEMORY_LOAD) {
        return GNW_CYCLE_FP_ONE *
               gnw_m7_region_load_latency_cycles(
                   m->region_load_latency_class);
    }
    if (m->insn_class == GNW_CYCLE_INSN_EARLY_ADD_SUB) {
        return GNW_CYCLE_FP_ONE / 2;
    }
    if (m->insn_class == GNW_CYCLE_INSN_MAC ||
        (m->apsr_writes && m->gpr_writes &&
         m->insn_class != GNW_CYCLE_INSN_ALU)) {
        return GNW_CYCLE_FP_ONE * 2;
    }
    return GNW_CYCLE_FP_ONE;
}

static bool same_issue_cycle(uint64_t a, uint64_t b)
{
    return (a >> GNW_CYCLE_FP_SHIFT) == (b >> GNW_CYCLE_FP_SHIFT);
}

static void apply_bus_contention(GnwCycleModelState *s)
{
    uint64_t cycles = s->pending_bus_contention_cycles;

    if (!cycles) {
        return;
    }
    g_assert(cycles <= (UINT64_MAX - s->cycle_fp) / GNW_CYCLE_FP_ONE);
    s->cycle_fp += cycles * GNW_CYCLE_FP_ONE;
    s->pending_bus_contention_cycles = 0;
}

void gnw_cycle_model_finish(CPUState *cpu)
{
    GnwCycleModelState *s = &cpu->gnw_cycle_model;
    GnwCycleInsnMetadata *m;
    uint64_t start, issue, slot, latency, ready;
    unsigned reg;
    uint32_t mask;
    const uint32_t flag_mask = (1U << ARRAY_SIZE(s->flags_ready_fp)) - 1;
    bool pair_rejected = false;
    uint64_t retired;

    if (!s->enabled) {
        return;
    }
    maybe_rebase(s);
    start = s->cycle_fp;
    apply_nc_axi_fetch(s);
    apply_icache_fetch(s);
    apply_fetch_residual(s);
    apply_memory_residual(s);
    retired = s->xip_service_deferred_finish ? 0 : 1;

    if (!s->pending_valid) {
        g_assert(!s->dcache.pending_count);
        s->fallback[GNW_CYCLE_FALLBACK_PREDICATE_SKIP]++;
        serialize_baseline(s);
        apply_icache_maintenance(s);
        apply_bus_contention(s);
        commit_delta(cpu, s->cycle_fp - start, retired, true);
        s->completed_instructions++;
        if (s->exception_scoreboard_flush_pending) {
            exception_transition_scoreboard_flush(s);
        }
        return;
    }

    m = &s->pending;
    if (m->flags & GNW_CYCLE_META_BRANCH) {
        m->branch_target = cpu->cc->get_pc(cpu);
        m->branch_taken = m->branch_target != m->pc + m->size;
    }
    schedule_history_observe(s, m);
    s->pending_valid = false;
    dcache_commit_pending(&s->dcache);
    if (!(m->flags & GNW_CYCLE_META_SUPPORTED)) {
        unsigned reason = m->fallback_reason;

        if (reason <= GNW_CYCLE_FALLBACK_NONE ||
            reason >= GNW_CYCLE_FALLBACK_COUNT) {
            reason = GNW_CYCLE_FALLBACK_UNSUPPORTED_DECODE;
        }
        s->fallback[reason]++;
        serialize_baseline(s);
        apply_icache_maintenance(s);

        apply_bus_contention(s);
        commit_delta(cpu, s->cycle_fp - start, retired, true);
        s->completed_instructions++;
        if (s->exception_scoreboard_flush_pending) {
            exception_transition_scoreboard_flush(s);
        }
        return;
    }

    if (m->insn_class == GNW_CYCLE_INSN_NOP && !s->disfold_set) {
        s->folded_nops++;
        commit_delta(cpu, 0, retired, true);
        s->completed_instructions++;
        if (s->exception_scoreboard_flush_pending) {
            exception_transition_scoreboard_flush(s);
        }
        return;
    }

    slot = s->disfold_set ? GNW_CYCLE_FP_ONE : GNW_CYCLE_FP_ONE / 2;
    issue = fp_ceil_slot(s->cycle_fp, slot);
    if (!s->disfold_set && s->previous_valid &&
        same_issue_cycle(s->previous_issue_fp, issue)) {
        bool apsr_raw = (s->previous.apsr_writes & m->apsr_reads) &&
                        !(m->flags & GNW_CYCLE_META_DIRECT_BRANCH);

        pair_rejected = s->previous.insn_class == GNW_CYCLE_INSN_NOP ||
                        m->insn_class == GNW_CYCLE_INSN_NOP ||
                        (s->previous.flags & GNW_CYCLE_META_SYSTEM) ||
                        (m->flags & GNW_CYCLE_META_SYSTEM) ||
                        ((s->previous.gpr_writes & m->gpr_reads) &&
                         s->previous.insn_class !=
                         GNW_CYCLE_INSN_EARLY_ADD_SUB) || apsr_raw;
        if (pair_rejected) {
            issue = fp_ceil_slot(fp_add(issue, 1), GNW_CYCLE_FP_ONE);
        }
    }

    ready = issue;
    for (mask = m->gpr_reads; mask; mask &= mask - 1) {
        reg = ctz32(mask);
        ready = MAX(ready, s->gpr_ready_fp[reg]);
    }
    for (mask = m->apsr_reads & flag_mask; mask; mask &= mask - 1) {
        reg = ctz32(mask);
        ready = MAX(ready,
                    m->flags & GNW_CYCLE_META_DIRECT_BRANCH ?
                    s->branch_flags_ready_fp[reg] :
                    s->flags_ready_fp[reg]);
    }
    issue = fp_ceil_slot(ready, slot);
    latency = metadata_latency(m);
    ready = fp_add(issue, latency);
    if (m->region_load_latency_class == GNW_M7_REGION_LOAD_LATENCY_AXI1) {
        uint64_t contention = s->pending_bus_contention_cycles;

        /* AXI1 normal-noncacheable loads occupy the measured Cortex-M7 load
         * path through result completion.  One final issue slot can overlap
         * the response; dependent reads still wait for the full result-ready
         * time.  This models the V40 idle loop's measured five-cycle AXI1 vs
         * DTCM delta without changing the dependent-chain latency. */
        g_assert(contention <=
                 (UINT64_MAX - ready) / GNW_CYCLE_FP_ONE);
        ready += contention * GNW_CYCLE_FP_ONE;
        s->pending_bus_contention_cycles = 0;
        s->cycle_fp = MAX(fp_add(issue, slot),
                          ready >= slot ? ready - slot : 0);
    } else {
        s->cycle_fp = fp_add(issue, slot);
    }
    if ((m->flags & GNW_CYCLE_META_BRANCH) && m->branch_taken) {
        /* A taken branch makes its target ready no earlier than one full
         * cycle after branch issue.  With DISFOLD clear this supplies the
         * missing half-cycle after the branch's half-cycle issue slot; with
         * DISFOLD set the existing full slot already meets the floor. */
        s->cycle_fp = MAX(s->cycle_fp,
                          fp_add(issue, GNW_CYCLE_FP_ONE));
    }
    for (mask = m->gpr_writes; mask; mask &= mask - 1) {
        reg = ctz32(mask);
        s->gpr_ready_fp[reg] = ready;
    }
    for (mask = m->apsr_writes & flag_mask; mask; mask &= mask - 1) {
        reg = ctz32(mask);
        s->flags_ready_fp[reg] = fp_add(issue, latency);
        s->branch_flags_ready_fp[reg] = fp_add(issue, slot);
    }

    s->previous = *m;
    s->previous_valid = true;
    s->previous_was_store = false;
    s->previous_issue_fp = issue;
    apply_icache_maintenance(s);
    apply_bus_contention(s);
    if (!s->disfold_set && !pair_rejected &&
        same_issue_cycle(start, issue) && (issue & (GNW_CYCLE_FP_ONE / 2))) {
        s->issued_pairs++;
    }
    commit_delta(cpu, s->cycle_fp - start, retired, true);
    s->completed_instructions++;
    if (s->exception_scoreboard_flush_pending) {
        exception_transition_scoreboard_flush(s);
    }
}

void gnw_cycle_model_abort(CPUState *cpu, bool attempt_counted)
{
    GnwCycleModelState *s = &cpu->gnw_cycle_model;
    uint64_t start;

    if (!s->enabled) {
        return;
    }
    xip_unseen_end_stream(s);
    /* A faulted attempt never reaches the deadline-aware service drain. */
    xip_service_cancel_pending(s);
    if (!s->pending_valid) {
        return;
    }
    start = s->cycle_fp;
    apply_nc_axi_fetch(s);
    s->pending_fetch_residual_cycles = 0;
    s->pending_memory_residual_cycles = 0;
    s->pending_icache_maintenance_cycles = 0;
    gnw_m7_icache_finish_attempt(&s->icache, false);
    s->pending_valid = false;
    s->pending_bus_contention_cycles = 0;
    s->dcache.pending_count = 0;
    if (s->exception_scoreboard_flush_pending) {
        exception_transition_scoreboard_flush(s);
    }
    if (!attempt_counted) {
        return;
    }
    s->aborted_attempts++;
    s->fallback[GNW_CYCLE_FALLBACK_EXCEPTION_ATTEMPT]++;
    serialize_baseline(s);
    /* QEMU's exact attempted-instruction count supplies this baseline unit. */
    commit_delta(cpu, s->cycle_fp - start, 0, false);
}
