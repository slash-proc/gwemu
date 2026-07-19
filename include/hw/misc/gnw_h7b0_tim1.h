/*
 * Stub for TIM1 (advanced-control timer), extended with a real counter
 * for CR1.CEN -- see gnw_h7b0_tim1.c for rationale. Mirrors the same
 * approach as gnw_h7b0_tim2.c's TIM2-TIM14 block model (CEN-driven
 * counting paced against elapsed virtual-clock time, SR.UIF set on
 * overflow via a QEMUTimer), but for a single instance and with a
 * live-computed CNT so a short busy-wait between two CNT reads also
 * observes real advancement, not just the eventual UIF.
 */
#ifndef HW_MISC_GNW_H7B0_TIM1_H
#define HW_MISC_GNW_H7B0_TIM1_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/timer.h"
#include "hw/misc/gnw_h7b0_rcc.h"

#define TYPE_GNW_H7B0_TIM1 "gnw-h7b0-tim1"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0Tim1State, GNW_H7B0_TIM1)

#define GNW_H7B0_TIM1_SIZE 0x400

struct GnwH7B0Tim1State {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_TIM1_SIZE / 4];
    QEMUTimer *count_timer;

    /* Virtual-clock timestamp of the last counter (re)start -- CR1.CEN
     * 0->1 transition or an EGR.UG forced reload while counting. CNT is
     * computed lazily on read from elapsed time since this point, same
     * idea as gnw_h7b0_ltdc.c's vblank-phase computation elsewhere in
     * this project: cheap, and correct regardless of how often (or
     * rarely) the guest actually polls CNT. */
    int64_t count_start_ns;

    /* Not owned; set by the board via gnw_h7b0_tim1_set_rcc() once both
     * TIM1 and RCC are realized, same pattern as gnw_h7b0_tim2_set_rcc()
     * -- lets period computation track the real, currently-configured
     * CPU/HCLK rate rather than a fixed guess. May be NULL. */
    GnwH7B0RccState *rcc;
};

void gnw_h7b0_tim1_set_rcc(GnwH7B0Tim1State *s, GnwH7B0RccState *rcc);

#endif
