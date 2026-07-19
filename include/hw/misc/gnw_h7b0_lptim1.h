/*
 * STM32H7B0 LPTIM1 free-running-counter minimal stub (Nintendo Game & Watch)
 *
 * Was entirely unmapped (LPTIM1_BASE == 0x40002400 had no device/RAM/
 * unimplemented-stub backing at all, unlike LPTIM2/LPTIM3 which at
 * least get create_unimplemented_device()) -- found via stm32h7b0-diag's
 * case_lptim1_correct.c (LPTIM1's first-ever use in this firmware)
 * hanging the whole diag sweep: any register access here faulted, and
 * the resulting unrecovered HardFault spun forever in the default fault
 * handler, timing out the host harness rather than failing cleanly.
 *
 * Not cycle-accurate, no real kernel-clock-mux modeling (LPTIM1SEL is
 * left at its SVD reset default -- case_lptim1_correct.c's own header
 * comment already disclaims any timing-accuracy claim, only that the
 * counter visibly advances). CNT free-runs off QEMU_CLOCK_VIRTUAL once
 * CR.ENABLE + (CNTSTRT|SNGSTRT) are both set, wrapping at ARR+1 -- same
 * on-read-computed-from-elapsed-time approach as gnw_h7b0_dwt.c's
 * CYCCNT, just at an arbitrary assumed rate since no known consumer
 * needs a real one yet. Every other register is a plain read/write
 * shadow with no side effects.
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

#ifndef HW_MISC_GNW_H7B0_LPTIM1_H
#define HW_MISC_GNW_H7B0_LPTIM1_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_LPTIM1 "gnw-h7b0-lptim1"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0Lptim1State, GNW_H7B0_LPTIM1)

#define GNW_H7B0_LPTIM1_SIZE   0x400

#define GNW_H7B0_LPTIM1_ISR    0x00
#define LPTIM_ISR_CMPOK        (1U << 3)
#define LPTIM_ISR_ARROK        (1U << 4)
#define GNW_H7B0_LPTIM1_ICR    0x04
#define LPTIM_ICR_CMPOKCF      (1U << 3)
#define LPTIM_ICR_ARROKCF      (1U << 4)
#define GNW_H7B0_LPTIM1_IER    0x08
#define GNW_H7B0_LPTIM1_CFGR   0x0C
#define GNW_H7B0_LPTIM1_CR     0x10
#define LPTIM_CR_ENABLE        (1U << 0)
#define LPTIM_CR_SNGSTRT       (1U << 1)
#define LPTIM_CR_CNTSTRT       (1U << 2)
#define GNW_H7B0_LPTIM1_CMP    0x14
#define GNW_H7B0_LPTIM1_ARR    0x18
#define GNW_H7B0_LPTIM1_CNT    0x1C

/* Arbitrary free-run rate -- fast enough that CNT visibly advances
 * within any realistic host-side or guest-busy-wait poll window, no
 * real-clock-tree derivation intended (see file header). */
#define GNW_H7B0_LPTIM1_ASSUMED_HZ 1000000ULL

struct GnwH7B0Lptim1State {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;
    uint32_t regs[GNW_H7B0_LPTIM1_SIZE / 4];

    bool running;
    uint32_t base_count;
    int64_t enabled_since_ns;
};

#endif
