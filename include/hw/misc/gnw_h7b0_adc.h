/*
 * STM32H7B0 ADC1/ADC2 minimal stub (Nintendo Game & Watch)
 *
 * Not cycle-accurate, not a real conversion model -- ADC reads always
 * return 0 (see gnw_h7b0_adc_read()), so anything depending on a real
 * battery-voltage/analog reading (board_adc_init()'s actual purpose)
 * will read a fixed value, not real hardware behavior. Purpose: real
 * firmware's ADC init sets CR's ADEN bit then polls the matching
 * ISR's ADRDY bit and must not hang forever doing so (found via a
 * gnw-chainloader boot hang in board_adc_init()) -- see
 * docs/roadmap.md Phase 1. Every other register is a plain
 * read-what-was-written shadow with no side effects.
 *
 * Covers both ADC1 (this device's own base) and ADC2 (mapped 0x100
 * higher, per sdk/cmsis-device-h7/Include/stm32h7b0xx.h's ADC2_BASE =
 * ADC1_BASE + 0x100) plus the shared ADC12_COMMON registers at +0x300,
 * as one 0x400 register file -- not two separate device instances,
 * since firmware addresses them as a single contiguous block via
 * ADC1/ADC2 pointers into the same peripheral window.
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

#ifndef HW_MISC_GNW_H7B0_ADC_H
#define HW_MISC_GNW_H7B0_ADC_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_ADC "gnw-h7b0-adc"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0AdcState, GNW_H7B0_ADC)

#define GNW_H7B0_ADC_SIZE   0x400

/* Per-instance offsets within each ADC1/ADC2 0x100 sub-block. */
#define GNW_H7B0_ADC_ISR    0x00
#define ADC_ISR_ADRDY       (1U << 0)
#define GNW_H7B0_ADC_CR     0x08
#define ADC_CR_ADEN         (1U << 0)

/* ADC1/ADC2 sub-block stride and count within the combined register file. */
#define GNW_H7B0_ADC_INSTANCE_STRIDE 0x100
#define GNW_H7B0_ADC_INSTANCE_COUNT  2

struct GnwH7B0AdcState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_ADC_SIZE / 4];
};

#endif
