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
 * Fork-local: 12 -> 16 (4096 -> 65536 entries).
 *
 * QEMU's default of 12 assumes 4KB pages. This target is ARM M-profile, which
 * forces TARGET_PAGE_BITS = 10 (1KB pages) to model sub-4K MPU regions --
 * target/arm/cpu.c. The softmmu TB hash (accel/tcg/tb-hash.h) splits its bits
 * evenly between page number and intra-page offset, so with 1KB pages a 12-bit
 * cache indexes on only 6 page bits: a working set spanning more than 64 pages
 * aliases hard in a direct-mapped cache, and every miss falls through to
 * tb_htable_lookup().
 *
 * Measured on the 8086tiny DOS core (backup/dos-perf, fixed 30s timeline):
 * host CPU 8.73s -> 6.97s median, a 20% reduction, against a ~+-2% noise floor.
 * helper_lookup_tb_ptr was ~32-35% of on-CPU samples before the change.
 *
 * Cost is 16 bytes/entry = 1MB per vCPU (was 64KB); this machine is single-core.
 */
#define TB_JMP_CACHE_BITS 16
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
