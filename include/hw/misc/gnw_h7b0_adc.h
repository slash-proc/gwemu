/*
 * STM32H7B0 ADC1/ADC2 minimal stub (Nintendo Game & Watch)
 *
 * Not cycle-accurate, not a real conversion model. Purpose: real
 * firmware's ADC init sets CR's ADEN bit then polls the matching
 * ISR's ADRDY bit and must not hang forever doing so (found via a
 * gnw-chainloader boot hang in board_adc_init()) -- see
 * docs/roadmap.md Phase 1. CR's ADCAL (bit 31, self-clearing
 * "calibration in progress" flag on real hardware) is cleared
 * instantly for the same reason: patched-zelda-bank1.bin's ADC init
 * sets it and spins on it clearing, which never happened before this
 * fix.
 *
 * Regular conversions (CR's ADSTART) are also instant: ISR's EOC is set
 * and DR is loaded with GNW_H7B0_ADC_FULL_BATTERY_RAW as soon as ADSTART
 * is written, and the matching NVIC line (ADC_IRQn, shared by ADC1/ADC2)
 * is pulsed if IER's EOCIE is set. This covers both firmware's battery
 * read paths: gnw-chainloader's board_get_battery_raw() busy-polls ISR's
 * EOC after setting ADSTART, while retro-go-sd's bq24072.c calls
 * HAL_ADC_Start_IT() and reads the result from HAL_ADC_ConvCpltCallback(),
 * which only fires on a real ADC interrupt. Only ADC1's channel-4 (PC4)
 * battery-sense reading is modeled; every other register is a plain
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
#define ADC_ISR_EOC         (1U << 2)
#define GNW_H7B0_ADC_IER    0x04
#define ADC_IER_EOCIE       (1U << 2)
#define GNW_H7B0_ADC_CR     0x08
#define ADC_CR_ADEN         (1U << 0)
#define ADC_CR_ADSTART      (1U << 2)
#define ADC_CR_ADCAL        (1U << 31)
#define GNW_H7B0_ADC_DR     0x40

/*
 * Fixed regular-conversion result returned on every ADSTART, standing in for
 * board_get_battery_raw()'s PC4/channel-4 battery-voltage read (see
 * gnw-chainloader's board.c and retro-go-sd's bq24072.c). Both firmwares
 * treat raw >= 13000 as BQ24072_BATTERY_FULL (100%); 13500 sits safely above
 * that threshold.
 */
#define GNW_H7B0_ADC_FULL_BATTERY_RAW 13500

/* ADC1/ADC2 sub-block stride and count within the combined register file. */
#define GNW_H7B0_ADC_INSTANCE_STRIDE 0x100
#define GNW_H7B0_ADC_INSTANCE_COUNT  2

struct GnwH7B0AdcState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;
    uint32_t regs[GNW_H7B0_ADC_SIZE / 4];
};

#endif
