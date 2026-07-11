/*
 * STM32H7B0 PWR minimal stub (Nintendo Game & Watch)
 *
 * Not cycle-accurate, not a full PWR model. Purpose: real firmware's
 * supply-config code (HAL_PWREx_ConfigSupply()-equivalent) writes
 * PWR_CR3 then polls PWR_CSR1's ACTVOSRDY bit, and separately writes
 * PWR_SRDCR's VOS field then polls PWR_SRDCR's own VOSRDY bit -- both
 * must not hang forever waiting for a status bit nothing ever sets. See
 * docs/roadmap.md Phase 1 and STATUS.md's PWR entry (found via a
 * gnw-chainloader boot hang in SystemClock_Config once RCC/flash/DBGMCU
 * gaps were closed). Every other register is a plain read-what-was-
 * written shadow with no side effects.
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

#ifndef HW_MISC_GNW_H7B0_PWR_H
#define HW_MISC_GNW_H7B0_PWR_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_PWR "gnw-h7b0-pwr"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0PwrState, GNW_H7B0_PWR)

/*
 * Register offsets/bits below are from STM32H7B0.svd's PWR peripheral
 * (baseAddress 0x58024800, right after RCC), cross-checked against
 * sdk/cmsis-device-h7/Include/stm32h7b0xx.h's PWR_TypeDef.
 */
#define GNW_H7B0_PWR_SIZE   0x400

#define GNW_H7B0_PWR_CSR1     0x04
#define PWR_CSR1_ACTVOSRDY    (1U << 13)

#define GNW_H7B0_PWR_CR3      0x0C

#define GNW_H7B0_PWR_SRDCR    0x18
#define PWR_SRDCR_VOSRDY      (1U << 13)

struct GnwH7B0PwrState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_PWR_SIZE / 4];
};

#endif
