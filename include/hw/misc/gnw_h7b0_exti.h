/*
 * EXTI (extended interrupt/event controller), Nintendo Game & Watch
 * STM32H7B0.
 *
 * Real edge-triggered interrupt delivery for lines 0-21 (group 1, the only
 * group any board line we care about maps into), wired to NVIC. Real
 * register semantics for RTSR1/FTSR1 (rising/falling trigger select) and
 * CPUIMR1/CPUPR1 (interrupt mask / pending, CPU domain -- D3PMR1 and the
 * D1/D2-domain variants aren't modeled, this SoC has no dual-core split
 * that would need them). SWIER1 (software interrupt) isn't modeled.
 *
 * Added specifically because a stock firmware image's EXTI0_IRQHandler
 * (real disassembly, tail-jumps to a real, non-trivial handler body) turned
 * out to be load-bearing for boot to proceed past a SysTick-driven
 * watchdog that only starts counting once EXTI0 has fired at least once --
 * see gnw_h7b0_gpio.c's PA0 (WKUP1, best guess) release logic, and
 * ~/Nerd/git/gnw-mario-decomp's README for the full investigation.
 */
#ifndef HW_MISC_GNW_H7B0_EXTI_H
#define HW_MISC_GNW_H7B0_EXTI_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_EXTI "gnw-h7b0-exti"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0ExtiState, GNW_H7B0_EXTI)

#define GNW_H7B0_EXTI_SIZE 0x400
#define GNW_H7B0_EXTI_NUM_IRQ_OUTPUTS 7

struct GnwH7B0ExtiState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_EXTI_SIZE / 4];
    /* EXTI0, EXTI1, EXTI2, EXTI3, EXTI4, EXTI9_5, EXTI15_10, in that order
     * (matches the real NVIC vector table's IRQ grouping for lines 0-15). */
    qemu_irq irq[GNW_H7B0_EXTI_NUM_IRQ_OUTPUTS];
    /* Last level seen per line 0-21, for edge detection. */
    bool line_level[22];
};

/*
 * Called by other device models (currently just gnw_h7b0_gpio.c) when a
 * pin wired to EXTI line `line` (0-21) changes level. Detects the edge
 * against the trigger-select config firmware has written (RTSR1/FTSR1),
 * and if unmasked (CPUIMR1), sets the pending bit and pulses the matching
 * NVIC line. A no-op if firmware hasn't configured this line yet, same as
 * real hardware.
 */
void gnw_h7b0_exti_set_line(GnwH7B0ExtiState *s, int line, bool level);

#endif
