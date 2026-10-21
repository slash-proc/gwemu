/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/host-utils.h"
#include "system/cpus.h"
#include "system/qtest.h"
#include "qemu/main-loop.h"
#include "qemu/option.h"
#include "qemu/seqlock.h"
#include "system/replay.h"
#include "system/runstate.h"
#include "hw/core/cpu.h"
#include "exec/icount.h"
#include "system/cpu-timers-internal.h"

/*
 * ICOUNT: Instruction Counter
 *
 * this module is split off from cpu-timers because the icount part
 * is TCG-specific, and does not need to be built for other accels.
 */
static bool icount_sleep = true;
/* Arbitrarily pick 1MIPS as the minimum allowable speed.  */
#define MAX_ICOUNT_SHIFT 10

bool icount_align_option;

/* Do not count executed instructions */
ICountMode use_icount = ICOUNT_DISABLED;

bool icount_sleep_enabled(void)
{
    return icount_sleep;
}

bool icount_alignment_enabled(void)
{
    return icount_align_option;
}

enum {
    ICOUNT_CYCLE_MODEL_NONE,
    ICOUNT_CYCLE_MODEL_DYNAMIC,
    ICOUNT_CYCLE_MODEL_TEST_HALF_RATE,
    ICOUNT_CYCLE_MODEL_EXPERIMENTAL_M7,
};

static void icount_enable_precise(void)
{
    /* Fixed conversion of insn to ns via "shift" option */
    use_icount = ICOUNT_PRECISE;
}

static void icount_enable_adaptive(void)
{
    /* Runtime adaptive algorithm to compute shift */
    use_icount = ICOUNT_ADAPTATIVE;
}

/*
 * The current number of executed instructions is based on what we
 * originally budgeted minus the current state of the decrementing
 * icount counters in extra/u16.low.
 */
static int64_t icount_get_executed(CPUState *cpu)
{
    int64_t executed = (cpu->icount_budget -
                        (cpu->neg.icount_decr.u16.low + cpu->icount_extra));

    /*
     * A modeled CF_NOIRQ TB cannot take the normal budget-exhaustion exit.
     * Its per-instruction decrement can therefore roll the 16-bit low half
     * from zero to 0xffff before the TB reaches its commit boundary.  The
     * linear subtraction above reads that single-instruction rollover as
     * -65535.  Recover the modular distance; a TB is capped at 65535
     * instructions, so at most one wrap can occur between commits.
     */
    if (icount_cycle_model_enabled() && executed < 0 &&
        executed >= -UINT16_MAX) {
        executed += 1ULL << 16;
    }
    return executed;
}

uint64_t icount_get_executed_in_slice(CPUState *cpu)
{
    int64_t executed = icount_get_executed(cpu);

    g_assert(executed >= 0);
    return executed;
}

/*
 * Update the global shared timer_state.qemu_icount to take into
 * account executed instructions. This is done by the TCG vCPU
 * thread so the main-loop can see time has moved forward.
 */
static void icount_update_locked(CPUState *cpu)
{
    int64_t executed = icount_get_executed(cpu);
    uint64_t modeled_retired = cpu->icount_model_pending_retired;
    uint64_t modeled_cycles = cpu->icount_model_pending_cycles;

    g_assert(executed >= 0);
    if (timers_state.icount_test_cycle_scale) {
        g_assert(modeled_retired == 0 && modeled_cycles == 0);
        modeled_retired = executed;
        g_assert((uint64_t)executed <=
                 UINT64_MAX / timers_state.icount_test_cycle_scale);
        modeled_cycles = (uint64_t)executed *
                         timers_state.icount_test_cycle_scale;
    } else if (timers_state.icount_cycle_model_config ==
               ICOUNT_CYCLE_MODEL_TEST_HALF_RATE &&
               modeled_retired == 0 && modeled_cycles == 0) {
        uint64_t start = qatomic_read(&timers_state.qemu_icount);

        modeled_retired = executed;
        /* Deterministic 0,1,0,1... cost, phase derived from migrated icount. */
        modeled_cycles = ((start & 1) + (uint64_t)executed) / 2;
    }
    /* A producer may only describe instructions QEMU actually retired. */
    g_assert(modeled_retired <= executed);

    if (modeled_cycles >= modeled_retired) {
        uint64_t penalty = modeled_cycles - modeled_retired;
        int64_t old = qatomic_read(&timers_state.icount_cycle_penalties);

        g_assert(penalty <= INT64_MAX - old);
        qatomic_set(&timers_state.icount_cycle_penalties, old + penalty);
    } else {
        uint64_t credit = modeled_retired - modeled_cycles;
        int64_t old = qatomic_read(&timers_state.icount_cycle_credits);

        g_assert(credit <= INT64_MAX - old);
        qatomic_set(&timers_state.icount_cycle_credits, old + credit);
    }
    cpu->icount_model_pending_retired = 0;
    cpu->icount_model_pending_cycles = 0;
    cpu->icount_budget -= executed;
    /*
     * Model checkpoints may commit exactly as the 16-bit decrementer rolls
     * over.  Rebase both parts onto the reduced budget here; otherwise the
     * refill can restore low while leaving extra relative to the old budget,
     * making the next executed count negative by 0xffff instructions.
     */
    cpu->neg.icount_decr.u16.low = MIN(0xffff, cpu->icount_budget);
    cpu->icount_extra = cpu->icount_budget -
                       cpu->neg.icount_decr.u16.low;

    qatomic_set(&timers_state.qemu_icount,
                timers_state.qemu_icount + executed);
}

bool icount_cycle_model_enabled(void)
{
    return qatomic_read(&timers_state.icount_cycle_model_config) != 0;
}

void icount_enable_cycle_model(void)
{
    g_assert(icount_enabled() == ICOUNT_PRECISE);
    if (qatomic_read(&timers_state.icount_cycle_model_config) ==
        ICOUNT_CYCLE_MODEL_NONE) {
        qatomic_set(&timers_state.icount_cycle_model_config,
                    ICOUNT_CYCLE_MODEL_DYNAMIC);
    }
}

bool icount_enable_experimental_m7_cycle_model(void)
{
    uint32_t config;

    g_assert(icount_enabled() == ICOUNT_PRECISE);
    if (qatomic_read(&timers_state.icount_test_cycle_scale)) {
        return false;
    }
    config = qatomic_read(&timers_state.icount_cycle_model_config);
    if (config == ICOUNT_CYCLE_MODEL_NONE ||
        config == ICOUNT_CYCLE_MODEL_DYNAMIC) {
        qatomic_set(&timers_state.icount_cycle_model_config,
                    ICOUNT_CYCLE_MODEL_EXPERIMENTAL_M7);
        return true;
    }
    return config == ICOUNT_CYCLE_MODEL_EXPERIMENTAL_M7;
}

void icount_set_test_cycle_scale(unsigned cycles)
{
    g_assert(cycles > 0);
    g_assert(!qatomic_read(&timers_state.icount_test_cycle_scale));
    qatomic_set(&timers_state.icount_test_cycle_scale, cycles);
    icount_enable_cycle_model();
}

void icount_set_test_half_rate(void)
{
    g_assert(icount_enabled() == ICOUNT_PRECISE);
    g_assert(!qatomic_read(&timers_state.icount_cycle_model_config));
    g_assert(!qatomic_read(&timers_state.icount_test_cycle_scale));
    qatomic_set(&timers_state.icount_cycle_model_config,
                ICOUNT_CYCLE_MODEL_TEST_HALF_RATE);
}

void icount_test_account_mmio_read(void)
{
    uint64_t start;

    if (qatomic_read(&timers_state.icount_cycle_model_config) !=
        ICOUNT_CYCLE_MODEL_TEST_HALF_RATE || !current_cpu ||
        !current_cpu->running) {
        return;
    }
    g_assert(current_cpu->icount_model_pending_retired == 0);
    g_assert(current_cpu->icount_model_pending_cycles == 0);
    start = qatomic_read(&timers_state.qemu_icount);
    icount_account_cycles(current_cpu, 1, start & 1);
}

void icount_account_cycles(CPUState *cpu, uint64_t retired, uint64_t cycles)
{
    g_assert(cpu == current_cpu);
    g_assert(icount_cycle_model_enabled());
    g_assert(retired <= UINT64_MAX - cpu->icount_model_pending_retired);
    g_assert(cycles <= UINT64_MAX - cpu->icount_model_pending_cycles);

    cpu->icount_model_pending_retired += retired;
    cpu->icount_model_pending_cycles += cycles;
}

bool icount_commit_model_cycles(CPUState *cpu)
{
    bool expired;

    g_assert(icount_cycle_model_enabled());
    icount_update(cpu);
    expired = qemu_clock_expired(QEMU_CLOCK_VIRTUAL);
    if (expired) {
        qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
    }
    return expired;
}

bool icount_commit_stall_cycles(CPUState *cpu, uint64_t cycles)
{
    int64_t old;
    bool expired;

    g_assert(cpu == current_cpu);
    g_assert(icount_cycle_model_enabled());
    g_assert(cpu->icount_model_pending_retired == 0);
    g_assert(cpu->icount_model_pending_cycles == 0);
    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    old = qatomic_read(&timers_state.icount_cycle_penalties);
    g_assert(cycles <= INT64_MAX - old);
    qatomic_set(&timers_state.icount_cycle_penalties, old + cycles);
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                         &timers_state.vm_clock_lock);
    expired = qemu_clock_expired(QEMU_CLOCK_VIRTUAL);
    if (expired) {
        qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
    }
    return expired;
}

void icount_retract_cycle_credit(uint64_t cycles)
{
    int64_t old;

    g_assert(icount_cycle_model_enabled());
    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    old = qatomic_read(&timers_state.icount_cycle_credits);
    g_assert(cycles <= old);
    qatomic_set(&timers_state.icount_cycle_credits, old - cycles);
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                         &timers_state.vm_clock_lock);
}

/*
 * Update the global shared timer_state.qemu_icount to take into
 * account executed instructions. This is done by the TCG vCPU
 * thread so the main-loop can see time has moved forward.
 */
void icount_update(CPUState *cpu)
{
    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    icount_update_locked(cpu);
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                         &timers_state.vm_clock_lock);
}

static int64_t icount_get_raw_locked(void)
{
    CPUState *cpu = current_cpu;

    if (cpu && cpu->running) {
        if (!cpu->neg.can_do_io) {
            error_report("Bad icount read");
            exit(1);
        }
        /*
         * Ordinary icount exposes the current TB's pre-decremented count.
         * A dynamic cycle model cannot publish that in-flight instruction:
         * its completed cost is not known until the TB/exception boundary,
         * and a later credit would move an already observed clock backward.
         */
        if (!icount_cycle_model_enabled()) {
            icount_update_locked(cpu);
        }
    }
    /* The read is protected by the seqlock, but needs atomic to avoid UB */
    return qatomic_read(&timers_state.qemu_icount);
}

static int64_t icount_ns_from_count(int64_t count)
{
    int64_t frequency = qatomic_read(&timers_state.icount_cycle_clock_hz);

    if (frequency) {
        if (count < 0) {
            return -muldiv64(-count, NANOSECONDS_PER_SECOND, frequency);
        }
        return muldiv64(count, NANOSECONDS_PER_SECOND, frequency);
    }
    return count << qatomic_read(&timers_state.icount_time_shift);
}

static int64_t icount_time_at(int64_t icount)
{
    int64_t frequency = qatomic_read(&timers_state.icount_cycle_clock_hz);

    if (frequency) {
        int64_t anchor = qatomic_read(&timers_state.icount_cycle_anchor);
        int64_t anchor_ns = qatomic_read(&timers_state.icount_cycle_anchor_ns);

        return anchor_ns + icount_ns_from_count(icount - anchor);
    }
    return icount_ns_from_count(icount);
}

/*
 * Convert the retired-instruction timeline into modeled CPU cycles.  Stalls
 * add cycles, while issue overlap can retire instructions without advancing
 * the modeled cycle count by the same amount.  A correction that would move
 * time behind the high-water mark is a producer accounting bug: completed
 * costs must be committed before a clock-visible helper runs.
 */
static int64_t icount_get_modeled_cycles(int64_t instructions)
{
    int64_t penalties = qatomic_read(&timers_state.icount_cycle_penalties);
    int64_t credits = qatomic_read(&timers_state.icount_cycle_credits);
    int64_t cycles;
    int64_t highwater;

    g_assert(instructions >= 0);
    g_assert(penalties <= INT64_MAX - instructions);
    cycles = instructions + penalties;
    /* Credits are valid only when committed with their retired slice. */
    g_assert(credits <= cycles);
    cycles -= credits;

    highwater = qatomic_read(&timers_state.icount_cycle_highwater);
    /* A retrospective credit is a producer bug; never hide it by freezing. */
    g_assert(cycles >= highwater);
    while (cycles > highwater) {
        int64_t old = qatomic_cmpxchg(&timers_state.icount_cycle_highwater,
                                     highwater, cycles);
        if (old == highwater) {
            return cycles;
        }
        highwater = old;
    }
    return highwater;
}

static int64_t icount_get_locked(void)
{
    int64_t icount = icount_get_raw_locked();

    if (!icount_cycle_model_enabled()) {
        /*
         * Preserve ordinary icount as a read-only lockless timeline.  The
         * modeled-cycle high-water mark is validation state for signed
         * producers; touching it here would let concurrent default-mode
         * readers race before their seqlock retry.
         */
        return qatomic_read(&timers_state.qemu_icount_bias) +
               icount_time_at(icount);
    }
    return qatomic_read(&timers_state.qemu_icount_bias) +
           icount_time_at(icount_get_modeled_cycles(icount));
}

void icount_add_cycle_penalty(uint64_t cycles)
{
    if (!cycles) {
        return;
    }
    g_assert(qatomic_read(&timers_state.icount_cycle_clock_hz) > 0);
    icount_account_cycles(current_cpu, 0, cycles);
}

void icount_add_cycle_credit(uint64_t cycles)
{
    if (!cycles) {
        return;
    }
    g_assert(qatomic_read(&timers_state.icount_cycle_clock_hz) > 0);
    icount_account_cycles(current_cpu, cycles, 0);
}

int64_t icount_get_raw(void)
{
    int64_t icount;
    unsigned start;

    do {
        start = seqlock_read_begin(&timers_state.vm_clock_seqlock);
        icount = icount_get_raw_locked();
    } while (seqlock_read_retry(&timers_state.vm_clock_seqlock, start));

    return icount;
}

int64_t icount_get_cycle_count(int64_t *idle_ns)
{
    IcountCycleStamp stamp;

    icount_get_cycle_stamp(&stamp);
    if (idle_ns) {
        *idle_ns = stamp.idle_ns;
    }
    return stamp.modeled_cycles;
}

static void icount_get_cycle_stamp_locked(IcountCycleStamp *stamp)
{
    int64_t cycles;
    int64_t core_hz = qatomic_read(&timers_state.icount_cycle_clock_hz);

    g_assert(core_hz >= 0);

    if (icount_cycle_model_enabled()) {
        cycles = icount_get_modeled_cycles(
            qatomic_read(&timers_state.qemu_icount));
    } else {
        cycles = qatomic_read(&timers_state.qemu_icount);
        if (current_cpu && current_cpu->running) {
            int64_t executed = icount_get_executed(current_cpu);

            /*
             * An MMIO helper runs before the current instruction completes.
             * Include the earlier instructions in its TB, but keep that
             * observer instruction outside its own cycle sample.
             */
            g_assert(executed >= 0);
            if (executed > 0) {
                g_assert(executed - 1 <= INT64_MAX - cycles);
                cycles += executed - 1;
            }
        }
    }
    *stamp = (IcountCycleStamp) {
        .modeled_cycles = cycles,
        .idle_ns = qatomic_read(&timers_state.qemu_icount_bias),
        .core_hz = core_hz,
        .generation = qatomic_read(
            &timers_state.icount_cycle_clock_generation),
    };
}

void icount_get_cycle_stamp(IcountCycleStamp *stamp)
{
    unsigned start;

    g_assert(icount_enabled());
    g_assert(stamp);
    if (icount_cycle_model_enabled()) {
        /* Keep the instruction count and its signed adjustments coherent. */
        seqlock_write_lock(&timers_state.vm_clock_seqlock,
                           &timers_state.vm_clock_lock);
        icount_get_cycle_stamp_locked(stamp);
        seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                             &timers_state.vm_clock_lock);
        return;
    }

    do {
        start = seqlock_read_begin(&timers_state.vm_clock_seqlock);
        icount_get_cycle_stamp_locked(stamp);
    } while (seqlock_read_retry(&timers_state.vm_clock_seqlock, start));
}

/* Return the virtual CPU time, based on the instruction counter.  */
int64_t icount_get(void)
{
    int64_t icount;
    unsigned start;

    if (icount_cycle_model_enabled()) {
        /*
         * Modeled time combines qemu_icount with signed adjustments that
         * are published in the same seqlock write section.  A lockless
         * reader may transiently pair the old instruction count with new
         * credits before seqlock_read_retry() notices the writer.  Do not
         * run high-water validation on that torn tuple: cycle-model mode is
         * already restricted to one-instruction TBs, so serialize its clock
         * reads with the existing writer lock and validate one coherent
         * committed prefix.
         */
        seqlock_write_lock(&timers_state.vm_clock_seqlock,
                           &timers_state.vm_clock_lock);
        icount = icount_get_locked();
        seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                             &timers_state.vm_clock_lock);
        return icount;
    }

    do {
        start = seqlock_read_begin(&timers_state.vm_clock_seqlock);
        icount = icount_get_locked();
    } while (seqlock_read_retry(&timers_state.vm_clock_seqlock, start));

    return icount;
}

/*
 * Return the diagnostic virtual-time position at the last committed
 * cycle-model instruction prefix.  Unlike icount_get(), this deliberately
 * does not inspect or update the current CPU's in-flight TB state, so it is
 * suitable for read-only probes called from an in-flight memory callback
 * while can_do_io is false.  It is not a general-purpose side-effect clock
 * read; device operations must use the normal virtual clock APIs at a safe
 * I/O boundary.
 */
int64_t icount_get_committed_time_ns(void)
{
    int64_t instructions, cycles;

    g_assert(icount_cycle_model_enabled());

    /* The count and signed cycle adjustments must describe one prefix. */
    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    instructions = qatomic_read(&timers_state.qemu_icount);
    cycles = icount_get_modeled_cycles(instructions);
    cycles = qatomic_read(&timers_state.qemu_icount_bias) +
             icount_time_at(cycles);
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                         &timers_state.vm_clock_lock);
    return cycles;
}

int64_t icount_to_ns(int64_t icount)
{
    return icount_ns_from_count(icount);
}

void icount_cycle_clock_boundary(uint64_t frequency_hz,
                                 bool force_generation,
                                 IcountCycleStamp *before,
                                 IcountCycleStamp *after)
{
    IcountCycleStamp old_stamp;
    int64_t raw, now_ns;
    uint64_t generation;

    g_assert(icount_enabled());
    g_assert(frequency_hz > 0 && frequency_hz <= INT64_MAX);
    qatomic_set(&timers_state.icount_cycle_clock_config, 1);

    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    icount_get_cycle_stamp_locked(&old_stamp);
    raw = old_stamp.modeled_cycles;
    now_ns = qatomic_read(&timers_state.qemu_icount_bias) +
             icount_time_at(raw);
    qatomic_set(&timers_state.icount_cycle_anchor, raw);
    qatomic_set(&timers_state.icount_cycle_anchor_ns,
                now_ns - qatomic_read(&timers_state.qemu_icount_bias));
    qatomic_set(&timers_state.icount_cycle_clock_hz, frequency_hz);
    generation = old_stamp.generation;
    if (force_generation || old_stamp.core_hz != frequency_hz) {
        g_assert(generation != UINT64_MAX);
        generation++;
        qatomic_set(&timers_state.icount_cycle_clock_generation, generation);
    }
    if (before) {
        *before = old_stamp;
    }
    if (after) {
        *after = (IcountCycleStamp) {
            .modeled_cycles = old_stamp.modeled_cycles,
            .idle_ns = old_stamp.idle_ns,
            .core_hz = frequency_hz,
            .generation = generation,
        };
    }
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                         &timers_state.vm_clock_lock);
}

void icount_set_cycle_clock_hz(uint64_t frequency_hz)
{
    int64_t old_frequency, raw, now_ns;
    uint64_t generation;

    g_assert(icount_enabled());
    g_assert(frequency_hz > 0 && frequency_hz <= INT64_MAX);
    qatomic_set(&timers_state.icount_cycle_clock_config, 1);

    /*
     * Preserve this legacy API's treatment of an in-flight ordinary icount
     * slice.  New coordinated device boundaries use the stamp-based API
     * above, whose coordinates deliberately match icount_get_cycle_count().
     */
    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    raw = icount_get_raw_locked();
    if (icount_cycle_model_enabled()) {
        raw = icount_get_modeled_cycles(raw);
    }
    now_ns = qatomic_read(&timers_state.qemu_icount_bias) +
             icount_time_at(raw);
    old_frequency = qatomic_read(&timers_state.icount_cycle_clock_hz);
    qatomic_set(&timers_state.icount_cycle_anchor, raw);
    qatomic_set(&timers_state.icount_cycle_anchor_ns,
                now_ns - qatomic_read(&timers_state.qemu_icount_bias));
    qatomic_set(&timers_state.icount_cycle_clock_hz, frequency_hz);
    if (old_frequency != frequency_hz) {
        generation = qatomic_read(
            &timers_state.icount_cycle_clock_generation);
        g_assert(generation != UINT64_MAX);
        qatomic_set(&timers_state.icount_cycle_clock_generation,
                    generation + 1);
    }
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                         &timers_state.vm_clock_lock);
}

/*
 * Correlation between real and virtual time is always going to be
 * fairly approximate, so ignore small variation.
 * When the guest is idle real and virtual time will be aligned in
 * the IO wait loop.
 */
#define ICOUNT_WOBBLE (NANOSECONDS_PER_SECOND / 10)

static void icount_adjust(void)
{
    int64_t cur_time;
    int64_t cur_icount;
    int64_t delta;

    /* If the VM is not running, then do nothing.  */
    if (!runstate_is_running()) {
        return;
    }

    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    cur_time = REPLAY_CLOCK_LOCKED(REPLAY_CLOCK_VIRTUAL_RT,
                                   cpu_get_clock_locked());
    cur_icount = icount_get_locked();

    delta = cur_icount - cur_time;
    /* FIXME: This is a very crude algorithm, somewhat prone to oscillation.  */
    if (delta > 0
        && timers_state.last_delta + ICOUNT_WOBBLE < delta * 2
        && timers_state.icount_time_shift > 0) {
        /* The guest is getting too far ahead.  Slow time down.  */
        qatomic_set(&timers_state.icount_time_shift,
                    timers_state.icount_time_shift - 1);
    }
    if (delta < 0
        && timers_state.last_delta - ICOUNT_WOBBLE > delta * 2
        && timers_state.icount_time_shift < MAX_ICOUNT_SHIFT) {
        /* The guest is getting too far behind.  Speed time up.  */
        qatomic_set(&timers_state.icount_time_shift,
                    timers_state.icount_time_shift + 1);
    }
    timers_state.last_delta = delta;
    qatomic_set(&timers_state.qemu_icount_bias,
                cur_icount - (timers_state.qemu_icount
                              << timers_state.icount_time_shift));
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                         &timers_state.vm_clock_lock);
}

static void icount_adjust_rt(void *opaque)
{
    timer_mod(timers_state.icount_rt_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL_RT) + 1000);
    icount_adjust();
}

static void icount_adjust_vm(void *opaque)
{
    timer_mod(timers_state.icount_vm_timer,
                   qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                   NANOSECONDS_PER_SECOND / 10);
    icount_adjust();
}

int64_t icount_round(int64_t count)
{
    int64_t frequency = qatomic_read(&timers_state.icount_cycle_clock_hz);
    int shift = qatomic_read(&timers_state.icount_time_shift);

    if (frequency) {
        int64_t instructions = muldiv64(count, frequency,
                                        NANOSECONDS_PER_SECOND);

        /* Round a deadline up to the first instruction at or after it. */
        if (muldiv64(instructions, NANOSECONDS_PER_SECOND, frequency) <
            count) {
            instructions++;
        }
        return instructions;
    }
    return (count + (1 << shift) - 1) >> shift;
}

static void icount_warp_rt(void)
{
    unsigned seq;
    int64_t warp_start;

    /*
     * The icount_warp_timer is rescheduled soon after vm_clock_warp_start
     * changes from -1 to another value, so the race here is okay.
     */
    do {
        seq = seqlock_read_begin(&timers_state.vm_clock_seqlock);
        warp_start = timers_state.vm_clock_warp_start;
    } while (seqlock_read_retry(&timers_state.vm_clock_seqlock, seq));

    if (warp_start == -1) {
        return;
    }

    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    if (runstate_is_running()) {
        int64_t clock = REPLAY_CLOCK_LOCKED(REPLAY_CLOCK_VIRTUAL_RT,
                                            cpu_get_clock_locked());
        int64_t warp_delta;

        warp_delta = clock - timers_state.vm_clock_warp_start;
        if (icount_enabled() == ICOUNT_ADAPTATIVE) {
            /*
             * In adaptive mode, do not let QEMU_CLOCK_VIRTUAL run too far
             * ahead of real time (it might already be ahead so careful not
             * to go backwards).
             */
            int64_t cur_icount = icount_get_locked();
            int64_t delta = clock - cur_icount;

            if (delta < 0) {
                delta = 0;
            }
            warp_delta = MIN(warp_delta, delta);
        }
        qatomic_set(&timers_state.qemu_icount_bias,
                    timers_state.qemu_icount_bias + warp_delta);
    }
    timers_state.vm_clock_warp_start = -1;
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);

    if (qemu_clock_expired(QEMU_CLOCK_VIRTUAL)) {
        qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
    }
}

static void icount_timer_cb(void *opaque)
{
    /*
     * No need for a checkpoint because the timer already synchronizes
     * with CHECKPOINT_CLOCK_VIRTUAL_RT.
     */
    icount_warp_rt();
}

void icount_start_warp_timer(void)
{
    int64_t clock;
    int64_t deadline;

    assert(icount_enabled());

    /*
     * Nothing to do if the VM is stopped: QEMU_CLOCK_VIRTUAL timers
     * do not fire, so computing the deadline does not make sense.
     */
    if (!runstate_is_running()) {
        return;
    }

    if (replay_mode != REPLAY_MODE_PLAY) {
        if (!all_cpu_threads_idle()) {
            return;
        }

        if (qtest_enabled()) {
            /* When testing, qtest commands advance icount.  */
            return;
        }

        replay_checkpoint(CHECKPOINT_CLOCK_WARP_START);
    } else {
        /* warp clock deterministically in record/replay mode */
        if (!replay_checkpoint(CHECKPOINT_CLOCK_WARP_START)) {
            /*
             * vCPU is sleeping and warp can't be started.
             * It is probably a race condition: notification sent
             * to vCPU was processed in advance and vCPU went to sleep.
             * Therefore we have to wake it up for doing something.
             */
            if (replay_has_event()) {
                qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
            }
            return;
        }
    }

    /* We want to use the earliest deadline from ALL vm_clocks */
    clock = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT);
    deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL,
                                          ~QEMU_TIMER_ATTR_EXTERNAL);
    if (deadline < 0) {
        if (!icount_sleep) {
            warn_report_once("icount sleep disabled and no active timers");
        }
        return;
    }

    if (deadline > 0) {
        /*
         * Ensure QEMU_CLOCK_VIRTUAL proceeds even when the virtual CPU goes to
         * sleep.  Otherwise, the CPU might be waiting for a future timer
         * interrupt to wake it up, but the interrupt never comes because
         * the vCPU isn't running any insns and thus doesn't advance the
         * QEMU_CLOCK_VIRTUAL.
         */
        if (!icount_sleep) {
            /*
             * We never let VCPUs sleep in no sleep icount mode.
             * If there is a pending QEMU_CLOCK_VIRTUAL timer we just advance
             * to the next QEMU_CLOCK_VIRTUAL event and notify it.
             * It is useful when we want a deterministic execution time,
             * isolated from host latencies.
             */
            seqlock_write_lock(&timers_state.vm_clock_seqlock,
                               &timers_state.vm_clock_lock);
            qatomic_set(&timers_state.qemu_icount_bias,
                        timers_state.qemu_icount_bias + deadline);
            seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                                 &timers_state.vm_clock_lock);
            qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
        } else {
            /*
             * We do stop VCPUs and only advance QEMU_CLOCK_VIRTUAL after some
             * "real" time, (related to the time left until the next event) has
             * passed. The QEMU_CLOCK_VIRTUAL_RT clock will do this.
             * This avoids that the warps are visible externally; for example,
             * you will not be sending network packets continuously instead of
             * every 100ms.
             */
            seqlock_write_lock(&timers_state.vm_clock_seqlock,
                               &timers_state.vm_clock_lock);
            if (timers_state.vm_clock_warp_start == -1
                || timers_state.vm_clock_warp_start > clock) {
                timers_state.vm_clock_warp_start = clock;
            }
            seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                                 &timers_state.vm_clock_lock);
            timer_mod_anticipate(timers_state.icount_warp_timer,
                                 clock + deadline);
        }
    } else if (deadline == 0) {
        qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
    }
}

void icount_account_warp_timer(void)
{
    if (!icount_sleep) {
        return;
    }

    /*
     * Nothing to do if the VM is stopped: QEMU_CLOCK_VIRTUAL timers
     * do not fire, so computing the deadline does not make sense.
     */
    if (!runstate_is_running()) {
        return;
    }

    replay_async_events();

    /* warp clock deterministically in record/replay mode */
    if (!replay_checkpoint(CHECKPOINT_CLOCK_WARP_ACCOUNT)) {
        return;
    }

    timer_del(timers_state.icount_warp_timer);
    icount_warp_rt();
}

bool icount_configure(QemuOpts *opts, Error **errp)
{
    const char *option = qemu_opt_get(opts, "shift");
    bool sleep = qemu_opt_get_bool(opts, "sleep", true);
    bool align = qemu_opt_get_bool(opts, "align", false);
    long time_shift = -1;

    if (!option) {
        if (qemu_opt_get(opts, "align") != NULL) {
            error_setg(errp, "Please specify shift option when using align");
            return false;
        }
        return true;
    }

    if (align && !sleep) {
        error_setg(errp, "align=on and sleep=off are incompatible");
        return false;
    }

    if (strcmp(option, "auto") != 0) {
        if (qemu_strtol(option, NULL, 0, &time_shift) < 0
            || time_shift < 0 || time_shift > MAX_ICOUNT_SHIFT) {
            error_setg(errp, "icount: Invalid shift value");
            return false;
        }
    } else if (icount_align_option) {
        error_setg(errp, "shift=auto and align=on are incompatible");
        return false;
    } else if (!icount_sleep) {
        error_setg(errp, "shift=auto and sleep=off are incompatible");
        return false;
    }

    icount_sleep = sleep;
    if (icount_sleep) {
        timers_state.icount_warp_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL_RT,
                                         icount_timer_cb, NULL);
    }

    icount_align_option = align;

    if (time_shift >= 0) {
        timers_state.icount_time_shift = time_shift;
        icount_enable_precise();
        return true;
    }

    icount_enable_adaptive();

    /*
     * 125MIPS seems a reasonable initial guess at the guest speed.
     * It will be corrected fairly quickly anyway.
     */
    timers_state.icount_time_shift = 3;

    /*
     * Have both realtime and virtual time triggers for speed adjustment.
     * The realtime trigger catches emulated time passing too slowly,
     * the virtual time trigger catches emulated time passing too fast.
     * Realtime triggers occur even when idle, so use them less frequently
     * than VM triggers.
     */
    timers_state.vm_clock_warp_start = -1;
    timers_state.icount_rt_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL_RT,
                                   icount_adjust_rt, NULL);
    timer_mod(timers_state.icount_rt_timer,
                   qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL_RT) + 1000);
    timers_state.icount_vm_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        icount_adjust_vm, NULL);
    timer_mod(timers_state.icount_vm_timer,
                   qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                   NANOSECONDS_PER_SECOND / 10);
    return true;
}

void icount_notify_exit(void)
{
    assert(icount_enabled());

    if (current_cpu) {
        qemu_cpu_kick(current_cpu);
        qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
    }
}
