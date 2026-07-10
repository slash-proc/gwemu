/*
 * STM32H7B0 RCC minimal stub (Nintendo Game & Watch)
 *
 * Not cycle-accurate, not a full RCC model. Purpose: real firmware's
 * clock-init code writes an *ON bit then polls the matching *RDY bit in
 * RCC_CR (and SW then polls SWS in RCC_CFGR) and must not hang forever
 * doing so -- see docs/roadmap.md Phase 1. Every other register is a
 * plain read-what-was-written shadow with no side effects.
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

#ifndef HW_MISC_GNW_H7B0_RCC_H
#define HW_MISC_GNW_H7B0_RCC_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_RCC "gnw-h7b0-rcc"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0RccState, GNW_H7B0_RCC)

/*
 * Register offsets/bits below are from
 * sdk/cmsis-device-h7/Include/stm32h7b0xx.h's RCC_TypeDef and RCC_CR_x /
 * RCC_CFGR_x bit definitions (run scripts/fetch-sdk.sh if sdk/ is
 * missing), cross-checked against RM0455's register boundary table
 * (RCC at 0x58024400-0x580247FF) and STM32H7B0.svd.
 */
#define GNW_H7B0_RCC_SIZE   0x400

#define GNW_H7B0_RCC_CR     0x00
#define RCC_CR_HSION        (1U << 0)
#define RCC_CR_HSIRDY       (1U << 2)
#define RCC_CR_HSEON        (1U << 16)
#define RCC_CR_HSERDY       (1U << 17)
#define RCC_CR_PLL1ON       (1U << 24)
#define RCC_CR_PLL1RDY      (1U << 25)
#define RCC_CR_PLL2ON       (1U << 26)
#define RCC_CR_PLL2RDY      (1U << 27)
#define RCC_CR_PLL3ON       (1U << 28)
#define RCC_CR_PLL3RDY      (1U << 29)

#define GNW_H7B0_RCC_CFGR   0x10
#define RCC_CFGR_SW_SHIFT   0
#define RCC_CFGR_SW_MASK    (0x7U << RCC_CFGR_SW_SHIFT)
#define RCC_CFGR_SWS_SHIFT  3
#define RCC_CFGR_SWS_MASK   (0x7U << RCC_CFGR_SWS_SHIFT)

struct GnwH7B0RccState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_RCC_SIZE / 4];
};

#endif
