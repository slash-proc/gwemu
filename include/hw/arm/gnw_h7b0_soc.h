/*
 * STM32H7B0 SoC (Nintendo Game & Watch), Phase 0 skeleton
 *
 * Bare Cortex-M7 + RAM only, no peripherals yet. Modeled on
 * hw/arm/stm32f405_soc.c's SoC-container pattern. Addresses taken from
 * ../../../minicraft-gnw/tools/retro-go-porting-toolkit/linker.ld (real
 * hardware memory map, cross-checked against STM32H7B0.svd at the repo
 * root) rather than the reference manual directly.
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

#ifndef HW_ARM_GNW_H7B0_SOC_H
#define HW_ARM_GNW_H7B0_SOC_H

#include "hw/arm/armv7m.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_SOC "gnw-h7b0-soc"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0State, GNW_H7B0_SOC)

/*
 * Real STM32H7B0 memory map (per
 * minicraft-gnw/tools/retro-go-porting-toolkit/linker.ld). Only DTCM and
 * AXI SRAM are modeled in this Phase 0 skeleton; ITCM, AHB SRAM, internal
 * flash, and QSPI/OSPI external flash are added in Phase 1 once boot-path
 * work starts.
 */
#define DTCM_BASE_ADDRESS  0x20000000
#define DTCM_SIZE          (128 * 1024)
#define AXISRAM_BASE_ADDRESS 0x24000000
#define AXISRAM_SIZE       (1024 * 1024)

struct GnwH7B0State {
    SysBusDevice parent_obj;

    ARMv7MState armv7m;

    MemoryRegion dtcm;
    MemoryRegion axisram;
    MemoryRegion axisram_alias;

    Clock *sysclk;
};

#endif
