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
#include "hw/misc/gnw_h7b0_rcc.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_SOC "gnw-h7b0-soc"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0State, GNW_H7B0_SOC)

/*
 * Real STM32H7B0 memory map, per RM0455 Table 6 (rm0455.pdf, repo root),
 * cross-checked against STM32H7B0.svd's RCC clock-enable bit names
 * (AXISRAM1/2/3LPEN, AHBSRAM1/2LPEN, DTCM1/2LPEN, BKPRAMLPEN, SRDSRAMLPEN)
 * confirming the bank split. EXCEPTION: internal flash size/layout is
 *2x256K dual-bank per the project owner (community-verified real-silicon
 * fact), NOT RM0455's stated 128K single-bank — see
 * ../../docs/h7b0-flash-discrepancy.md before changing FLASH_BANK*.
 */
#define ITCM_BASE_ADDRESS     0x00000000
#define ITCM_SIZE             (64 * 1024)

#define DTCM_BASE_ADDRESS     0x20000000
#define DTCM_SIZE             (128 * 1024)

#define AXISRAM1_BASE_ADDRESS 0x24000000
#define AXISRAM1_SIZE         (256 * 1024)
#define AXISRAM2_BASE_ADDRESS 0x24040000
#define AXISRAM2_SIZE         (384 * 1024)
#define AXISRAM3_BASE_ADDRESS 0x240A0000
#define AXISRAM3_SIZE         (384 * 1024)

#define AHBSRAM1_BASE_ADDRESS 0x30000000
#define AHBSRAM1_SIZE         (64 * 1024)
#define AHBSRAM2_BASE_ADDRESS 0x30010000
#define AHBSRAM2_SIZE         (64 * 1024)

#define SRDSRAM_BASE_ADDRESS  0x38000000
#define SRDSRAM_SIZE          (32 * 1024)
#define BKPSRAM_BASE_ADDRESS  0x38800000
#define BKPSRAM_SIZE          (4 * 1024)

/*
 * Internal flash: see ../../docs/h7b0-flash-discrepancy.md. 2x256K
 * dual-bank per community/project-owner knowledge, NOT RM0455's stated
 * 128K single-bank. Modeled as plain RAM for now (no program/erase
 * register semantics yet); content loading and bank-select-to-address-0
 * mirroring is future work, see docs/roadmap.md Phase 1.
 */
#define FLASH_BANK1_BASE_ADDRESS 0x08000000
#define FLASH_BANK2_BASE_ADDRESS 0x08100000
#define FLASH_BANK_SIZE           (256 * 1024)

/*
 * External OSPI flash (real G&W firmware lives here, XIP). Dual-quad
 * OCTOSPI1+OCTOSPI2 via the IO manager, since we're targeting the Tim
 * Scheuerwegen SD-card mod (SPI2/OSPI2 path), not the yota9 mod. Not yet
 * modeled as a real OCTOSPI device (no register-level emulation) --
 * reserved as plain RAM for now so an image can be loaded and executed
 * from the real address for boot-path testing. Size varies 1-256MB on
 * real hardware; default to minicraft-gnw's linker.ld default (64M).
 */
#define EXTFLASH_BASE_ADDRESS 0x90000000
#define EXTFLASH_SIZE         (64 * 1024 * 1024)

/*
 * RCC base address: computed as PERIPH_BASE + SRD_AHB4PERIPH_BASE offset
 * + 0x4400 from sdk/cmsis-device-h7/Include/stm32h7b0xx.h, cross-checked
 * against RM0455 Table 7's register boundary list (RCC:
 * 0x58024400-0x580247FF) -- both agree.
 */
#define RCC_BASE_ADDRESS 0x58024400

struct GnwH7B0State {
    SysBusDevice parent_obj;

    ARMv7MState armv7m;
    GnwH7B0RccState rcc;

    MemoryRegion itcm;
    MemoryRegion dtcm;
    MemoryRegion axisram1;
    MemoryRegion axisram2;
    MemoryRegion axisram3;
    MemoryRegion ahbsram1;
    MemoryRegion ahbsram2;
    MemoryRegion srdsram;
    MemoryRegion bkpsram;
    MemoryRegion flash_bank1;
    MemoryRegion flash_bank2;
    MemoryRegion extflash;

    Clock *sysclk;
};

#endif
