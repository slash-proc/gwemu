/*
 * Stub for the TIM2-TIM14 block, extended with a real per-instance
 * counter for CR1.CEN -- see gnw_h7b0_tim2.c for rationale (this used
 * to be a plain shadow plus an instant EGR.UG->SR.UIF hack; CEN-driven
 * counting now actually paces against elapsed virtual-clock time
 * instead of never completing/being meaningless).
 */
#ifndef HW_MISC_GNW_H7B0_TIM2_H
#define HW_MISC_GNW_H7B0_TIM2_H

#include "hw/sysbus.h"
#include "qom/object.h"
#include "qemu/timer.h"
#include "hw/misc/gnw_h7b0_rcc.h"

#define TYPE_GNW_H7B0_TIM2 "gnw-h7b0-tim2"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0Tim2State, GNW_H7B0_TIM2)

#define GNW_H7B0_TIM2_SIZE 0x2800

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
