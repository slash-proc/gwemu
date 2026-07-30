/*
 * The per-CPU TranslationBlock jump cache.
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ACCEL_TCG_TB_JMP_CACHE_H
#define ACCEL_TCG_TB_JMP_CACHE_H

#include "qemu/rcu.h"
#include "exec/cpu-common.h"

/*
 * Fork-local: 12 -> 14 (4096 -> 16384 entries).
 *
 * QEMU's default of 12 assumes 4KB pages. This target is ARM M-profile, which
 * forces TARGET_PAGE_BITS = 10 (1KB pages) to model sub-4K MPU regions --
 * target/arm/cpu.c. The softmmu TB hash (accel/tcg/tb-hash.h) splits its bits
 * evenly between page number and intra-page offset, so with 1KB pages a 12-bit
 * cache indexes on only 6 page bits: a working set spanning more than 64 pages
 * aliases hard in a direct-mapped cache, and every miss falls through to
 * tb_htable_lookup().
 *
 * Measured on the 8086tiny DOS core (backup/dos-perf, fixed 30s timeline), as
 * an interleaved 4-round A/B on one build tree, host CPU-seconds:
 *   BITS=12  8.91 8.92 8.95 9.15  median 8.935
 *   BITS=14  6.81 6.93 6.78 6.77  median 6.795   -24%
 * against a ~+-2% noise floor. helper_lookup_tb_ptr was 32-35% of on-CPU
 * samples before; after, tb_htable_lookup drops out of the profile entirely.
 *
 * 14 is the knee, not a guess: 15/16/18 were measured and are statistically
 * indistinguishable from 14, so the whole effect is the single step 12 -> 14
 * (6 -> 7 page bits) and larger caches only cost footprint.
 *
 * Cost is 16 bytes/entry = 256KB per vCPU (was 64KB); this machine is single-core.
 */
#define TB_JMP_CACHE_BITS 14
#define TB_JMP_CACHE_SIZE (1 << TB_JMP_CACHE_BITS)

/*
 * Invalidated in parallel; all accesses to 'tb' must be atomic.
 * A valid entry is read/written by a single CPU, therefore there is
 * no need for qatomic_rcu_read() and pc is always consistent with a
 * non-NULL value of 'tb'.  Strictly speaking pc is only needed for
 * CF_PCREL, but it's used always for simplicity.
 */
typedef struct CPUJumpCache {
    struct rcu_head rcu;
    struct {
        TranslationBlock *tb;
        vaddr pc;
    } array[TB_JMP_CACHE_SIZE];
} CPUJumpCache;

#endif /* ACCEL_TCG_TB_JMP_CACHE_H */
