/*
 * Stub for the TIM2-TIM14 block, extended with a real per-instance
 * counter for CR1.CEN -- see gnw_h7b0_tim2.c for rationale (this used
 * to be a plain shadow plus an instant EGR.UG->SR.UIF hack; CEN-driven
 * counting now actually paces against elapsed virtual-clock time
 * instead of never completing/being meaningless).
 */
#ifndef HW_MISC_GNW_H7B0_TIM2_H
#define HW_MISC_GNW_H7B0_TIM2_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/timer.h"
#include "hw/misc/gnw_h7b0_rcc.h"

#define TYPE_GNW_H7B0_TIM2 "gnw-h7b0-tim2"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0Tim2State, GNW_H7B0_TIM2)

/* TIM2-7 (6 x 0x400 = 0x1800) + TIM12/13/14 (3 x 0x400 = 0xC00) = 0x2400,
 * ending exactly at LPTIM1's own base (0x40002400) -- NOT 0x2800, which
 * was off by one 0x400 block and silently overlapped/shadowed the real
 * LPTIM1 device (gnw_h7b0_lptim1.c) with this plain-shadow stub at the
 * same priority, since this region is mapped after LPTIM1 in
 * gnw_h7b0_soc_realize(). Confirmed live via `info mtree` showing both
 * regions claiming 0x40002400-0x400027ff, and gnw_h7b0_lptim1_write()/
 * _read() never being reached at all for any access in that range, while
 * root-causing stm32h7b0-diag's case_lptim1_correct.c genuinely failing
 * (ISR.ARROK never observed set, CNT never observed advancing) even
 * after the real LPTIM1 device was added this session. */
#define GNW_H7B0_TIM2_SIZE 0x2400

/* TIM2/3/4/5/6/7 sit on a uniform 0x400 stride within this block (see
 * gnw_h7b0_tim2.c's EGR/CR1 handling); TIM12/13/14 don't and aren't
 * covered by the counter model below. */
#define GNW_H7B0_TIM2_BLOCK_INSTANCE_COUNT 6

struct GnwH7B0Tim2State;

typedef struct GnwH7B0Tim2TimerCtx {
    struct GnwH7B0Tim2State *s;
    int idx;
} GnwH7B0Tim2TimerCtx;

struct GnwH7B0Tim2State {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_TIM2_SIZE / 4];
    QEMUTimer *count_timer[GNW_H7B0_TIM2_BLOCK_INSTANCE_COUNT];
    GnwH7B0Tim2TimerCtx timer_ctx[GNW_H7B0_TIM2_BLOCK_INSTANCE_COUNT];

    /*
     * Reference point for deriving a LIVE CNT on read, rather than
     * returning whatever the guest last wrote.
     *
     * cnt_base is CNT's value at cnt_ref_ns (virtual-clock ns); a read
     * extrapolates from there using the instance's current PSC and
     * kernel clock, wrapping at ARR+1. Re-latched whenever anything
     * that invalidates the extrapolation changes (CNT/PSC/ARR written,
     * CEN toggled, EGR.UG issued, reset).
     *
     * Stock Mario polls TIM5's CNT 540 times a second (measured) as a
     * free-running microsecond time base -- ARR=0xffffffff, PSC=0x15,
     * never reading any other timer register. With a shadow-only CNT it
     * asked "what time is it?" 540 times a second and always got the
     * same answer, so every timing decision it made was wrong; audible
     * as crunchy/echoing audio, because its mixer emits chunks whose
     * phase no longer lines up (measured: 1.70x larger sample-to-sample
     * discontinuity exactly at DMA half-buffer seams vs mid-chunk).
     */
    uint64_t cnt_ref_ns[GNW_H7B0_TIM2_BLOCK_INSTANCE_COUNT];
    uint32_t cnt_base[GNW_H7B0_TIM2_BLOCK_INSTANCE_COUNT];

    /* Not owned; set by the board via gnw_h7b0_tim2_set_rcc() once both
     * TIM2 and RCC are realized, same pattern as gnw_h7b0_sai1_set_rcc()
     * -- lets period computation track the real, currently-configured
     * CPU/HCLK rate (which changes at runtime per retro-go's overclock
     * feature) instead of a fixed guess. May be NULL (e.g. unit tests
     * instantiating TIM2 standalone) -- callers must check before use. */
    GnwH7B0RccState *rcc;
};

void gnw_h7b0_tim2_set_rcc(GnwH7B0Tim2State *s, GnwH7B0RccState *rcc);

#endif
