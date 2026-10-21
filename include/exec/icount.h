/*
 * icount - Instruction Counter API
 * CPU timers state API
 *
 * Copyright 2020 SUSE LLC
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef EXEC_ICOUNT_H
#define EXEC_ICOUNT_H

/**
 * ICountMode: icount enablement state:
 *
 * @ICOUNT_DISABLED: Disabled - Do not count executed instructions.
 * @ICOUNT_PRECISE: Enabled - Fixed conversion of insn to ns via "shift" option
 * @ICOUNT_ADAPTATIVE: Enabled - Runtime adaptive algorithm to compute shift
 */
typedef enum {
    ICOUNT_DISABLED = 0,
    ICOUNT_PRECISE,
    ICOUNT_ADAPTATIVE,
} ICountMode;

typedef struct IcountCycleStamp {
    int64_t modeled_cycles;
    int64_t idle_ns;
    uint64_t core_hz;
    uint64_t generation;
} IcountCycleStamp;

#ifdef CONFIG_TCG
extern ICountMode use_icount;
#define icount_enabled() (use_icount)
bool icount_sleep_enabled(void);
bool icount_alignment_enabled(void);
#else
#define icount_enabled() ICOUNT_DISABLED
static inline bool icount_sleep_enabled(void)
{
    return true;
}
static inline bool icount_alignment_enabled(void)
{
    return false;
}
#endif

/* Protect the CONFIG_USER_ONLY test vs poisoning. */
#if defined(COMPILING_PER_TARGET) || defined(COMPILING_SYSTEM_VS_USER)
# ifdef CONFIG_USER_ONLY
#  undef  icount_enabled
#  define icount_enabled() ICOUNT_DISABLED
# endif
#endif

/*
 * Update the icount with the executed instructions. Called by
 * cpus-tcg vCPU thread so the main-loop can see time has moved forward.
 */
void icount_update(CPUState *cpu);

/*
 * Account one completed scheduler slice.  @retired must include only
 * completed producer callbacks; @cycles is their total modeled cost and may
 * be smaller or larger than @retired.  QEMU's exact icount checkpoint is the
 * execution-budget authority after partial-TB restoration.  Some target fault
 * paths count the faulting instruction attempt there even though it did not
 * architecturally retire; until exception-attempt costs are calibrated, leave
 * that unit unmodeled so it keeps the one-cycle baseline.
 * Calls accumulate in CPU-local state and are published atomically with the
 * next exact retired-instruction checkpoint.
 *
 * Clock-visible helpers within the current instruction observe only the
 * previously committed prefix.  The producer must account each completed
 * instruction before a later guest instruction or helper begins.  A faulting
 * or otherwise non-retired instruction must not be included.  Persistent
 * scheduler state is owned and migrated by the producer, must carry a
 * configuration hash, and must match on migration; these pending fields must
 * be empty at a migration boundary.
 *
 * Retired-instruction budgets remain QEMU execution limits; modeled cycles
 * advance virtual time and never buy or retract instructions.  While costs
 * are known only after execution, translated code is restricted to one
 * instruction and cannot chain directly to another TB.  A producer that can
 * stall within an instruction must additionally expose and stop at the next
 * positive event deadline before this API can support it; charging the whole
 * stall retrospectively is unsupported.
 */
void icount_account_cycles(CPUState *cpu, uint64_t retired, uint64_t cycles);

/*
 * Publish a pending modeled slice at a TB/exception boundary.  Returns true
 * when the modeled time crossed a virtual-clock deadline.  The caller must
 * leave translated execution so timer callbacks can run before another guest
 * instruction retires.
 */
bool icount_commit_model_cycles(CPUState *cpu);
bool icount_commit_stall_cycles(CPUState *cpu, uint64_t cycles);
void icount_retract_cycle_credit(uint64_t cycles);
/* Exact instructions consumed from the current execution budget. */
uint64_t icount_get_executed_in_slice(CPUState *cpu);

/* Dynamic producers require exact per-instruction commit boundaries. */
#if defined(COMPILING_PER_TARGET) || defined(COMPILING_SYSTEM_VS_USER)
# ifdef CONFIG_USER_ONLY
static inline bool icount_cycle_model_enabled(void)
{
    return false;
}
# else
bool icount_cycle_model_enabled(void);
# endif
#else
bool icount_cycle_model_enabled(void);
#endif
void icount_enable_cycle_model(void);
/* Hidden, unvalidated Cortex-M7 producer; rejects incompatible test modes. */
bool icount_enable_experimental_m7_cycle_model(void);

/* Unstable validation-only producers with deterministic signed costs. */
void icount_set_test_cycle_scale(unsigned cycles);
void icount_set_test_half_rate(void);
/* Validation hook called after a guest DWT MMIO read observes the clock. */
void icount_test_account_mmio_read(void);

/* get raw icount value */
int64_t icount_get_raw(void);
/* Return the exact committed cycle position and optional idle-warp bias. */
int64_t icount_get_cycle_count(int64_t *idle_ns);
/* Read cycle position, idle-warp position, and clock identity coherently. */
void icount_get_cycle_stamp(IcountCycleStamp *stamp);
/* Add a completed stall to the current producer slice. */
void icount_add_cycle_penalty(uint64_t cycles);
/* Account completed retired instructions whose issue cost was fully hidden. */
void icount_add_cycle_credit(uint64_t cycles);

/* return the virtual CPU time in ns, based on the instruction counter. */
int64_t icount_get(void);
/*
 * Return the coherent virtual ns at the last committed cycle-model prefix.
 * Diagnostic/probe use only: this does not update the current TB and is safe
 * for read-only in-flight memory callbacks when can_do_io is false. It is not
 * a general side-effect clock read.
 */
int64_t icount_get_committed_time_ns(void);
/*
 * convert an instruction counter value to ns, based on the icount shift.
 * This shift is set as a fixed value with the icount "shift" option
 * (precise mode), or it is constantly approximated and corrected at
 * runtime in adaptive mode.
 */
int64_t icount_to_ns(int64_t icount);

/* Rebase precise icount virtual time so one counted guest instruction is
 * treated as one modeled CPU cycle at @frequency_hz. Clock changes preserve
 * the already elapsed virtual time. This is opt-in and does not alter the
 * normal fixed-shift icount behavior. */
void icount_set_cycle_clock_hz(uint64_t frequency_hz);

/*
 * Rebase the cycle-to-time conversion without moving either cycle-domain
 * coordinate.  A rate change advances the generation; @force_generation also
 * advances it for a same-rate peripheral-clock boundary.  At most one
 * generation is consumed per call.  @before and @after may be NULL.
 */
void icount_cycle_clock_boundary(uint64_t frequency_hz,
                                 bool force_generation,
                                 IcountCycleStamp *before,
                                 IcountCycleStamp *after);

/**
 * icount_configure: configure the icount options, including "shift"
 * @opts: Options to parse
 * @errp: pointer to a NULL-initialized error object
 *
 * Return: true on success, else false setting @errp with error
 */
bool icount_configure(QemuOpts *opts, Error **errp);

/* used by tcg vcpu thread to calc icount budget */
int64_t icount_round(int64_t count);

/* if the CPUs are idle, start accounting real time to virtual clock. */
void icount_start_warp_timer(void);
void icount_account_warp_timer(void);
void icount_notify_exit(void);

#endif /* EXEC_ICOUNT_H */
