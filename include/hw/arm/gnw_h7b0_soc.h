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
#include "hw/misc/gnw_h7b0_pwr.h"
#include "hw/misc/gnw_h7b0_ospi.h"
#include "hw/misc/gnw_h7b0_adc.h"
#include "hw/display/gnw_h7b0_ltdc.h"
#include "hw/misc/gnw_h7b0_spi.h"
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

/*
 * DBGMCU (CoreSight debug unit), per RM0455 Table 7's register boundary
 * list. Modeled as plain RAM for now -- see the comment above its
 * INIT_RAM_REGION call in gnw_h7b0_soc.c realize().
 */
#define DBGMCU_BASE_ADDRESS 0x5C001000
#define DBGMCU_SIZE          0x400

/*
 * Flash controller registers (FLASH_ACR etc. -- NOT the memory-mapped
 * flash content itself, that's FLASH_BANK1/2_BASE_ADDRESS above), per
 * RM0455/CMSIS FLASH_R_BASE = AHB3PERIPH_BASE + 0x2000. Modeled as plain
 * RAM for now -- see the DBGMCU comment in gnw_h7b0_soc.c realize() for
 * the same rationale (real firmware polls FLASH_ACR wait-state-ready
 * bits during clock init; without backing memory here that BusFaults).
 */
#define FLASH_R_BASE_ADDRESS 0x52002000
#define FLASH_R_SIZE          0x1000

/*
 * FMC (Flexible Memory Controller -- external SRAM/NAND/PSRAM/SDRAM
 * interface), per RM0455/CMSIS FMC_R_BASE = AHB3PERIPH_BASE + 0x4000.
 * Modeled as plain RAM for now -- same rationale as FLASH_R/DBGMCU
 * above.
 */
#define FMC_BASE_ADDRESS 0x52004000
#define FMC_SIZE          0x1000

/*
 * PWR (power control), per RM0455/SVD baseAddress 0x58024800, right
 * after RCC. Modeled as a real minimal device (hw/misc/gnw_h7b0_pwr.c),
 * RCC-style: found by a gnw-chainloader boot hang in SystemClock_Config
 * spinning on PWR_CSR1's ACTVOSRDY bit, which a plain-RAM stub never
 * sets (it's a hardware-set-only status bit, not something firmware
 * writes). See gnw_h7b0_pwr.h.
 */
#define PWR_BASE_ADDRESS 0x58024800

/*
 * GPIOA-K, per STM32H7B0.svd (contiguous 0x400-per-port blocks,
 * 0x58020000-0x58022FFF). Modeled as plain RAM for now -- real GPIO
 * (buttons, LCD control lines) is Phase 4 territory (see
 * docs/roadmap.md); this is only enough to let pin-init code that
 * reads/writes MODER/OTYPER/etc. during early boot avoid BusFaulting.
 * No button input or LCD control-line side effects yet -- do not treat
 * this as a real GPIO model.
 */
#define GPIO_BASE_ADDRESS 0x58020000
#define GPIO_SIZE         (11 * 0x400)

/*
 * CRS (Clock Recovery System, HSI48 auto-trim -- used by USB), per
 * STM32H7B0.svd baseAddress 0x40008400. Modeled as plain RAM for now --
 * same rationale as the other not-yet-modeled peripherals above.
 */
#define CRS_BASE_ADDRESS 0x40008400
#define CRS_SIZE          0x400

/*
 * OCTOSPI1/2 controller registers (distinct from the memory-mapped XIP
 * window at EXTFLASH_BASE_ADDRESS above), per STM32H7B0.svd. Modeled as
 * a real minimal device (hw/misc/gnw_h7b0_ospi.c), RCC/PWR-style: found
 * by a gnw-chainloader boot hitting its own "spin forever on HAL error"
 * trap in OSPI_WriteBytes, because HAL_OSPI_Command()'s post-config
 * poll of SR's TCF bit never completed against a plain-RAM stub. See
 * gnw_h7b0_ospi.h -- still no real command/address/data-phase transfer
 * semantics (no bytes actually move), just enough for HAL to see
 * "command accepted, transfer complete" and not error out.
 */
#define OCTOSPI1_BASE_ADDRESS 0x52005000
#define OCTOSPI2_BASE_ADDRESS 0x5200A000
/*
 * OCTOSPI IO manager -- plain RAM for now (pin-mux config only, no
 * transfer semantics needed there).
 */
#define OCTOSPIM_BASE_ADDRESS 0x5200B400
#define OCTOSPIM_SIZE          0x400

/*
 * ADC1/ADC2 (+ common registers), per STM32H7B0.svd (0x40022000/
 * 0x40022100, 0x100 each, plus shared ADC_COMMON registers at +0x300).
 * Modeled as a real minimal device (hw/misc/gnw_h7b0_adc.c), RCC/PWR-
 * style: found by a gnw-chainloader boot hang in board_adc_init()
 * spinning on ADC1's ISR.ADRDY bit after setting CR.ADEN, which a
 * plain-RAM stub never sets. See gnw_h7b0_adc.h -- still no real
 * conversion semantics (reads always return 0).
 */
#define ADC_BASE_ADDRESS 0x40022000

/*
 * SPI2, per STM32H7B0.svd baseAddress 0x40003800. This is also the
 * LCD-panel init-command path (separate from the LTDC pixel-data
 * path; see gnw-chainloader's gw_lcd_spi_tx()) as well as the Tim
 * Scheuerwegen SD-card mod's SPI path (per CLAUDE.md's repo
 * conventions -- SPI2/OSPI2, not the yota9 mod). Modeled as a real
 * minimal device (hw/misc/gnw_h7b0_spi.c): found by a gnw-chainloader
 * boot hang in gw_lcd_spi_tx() spinning on SR.TXP/SR.EOT, which a
 * plain-RAM stub never sets. No real byte transfer happens yet (no
 * LCD-panel or SD-card model attached).
 */
#define SPI2_BASE_ADDRESS 0x40003800

/*
 * SPI1, per STM32H7B0.svd baseAddress 0x40013000. This is the "Tim"
 * SD-card mod's dedicated-pin bus, tried first by gnw-chainloader's
 * sdcard_detect() -> spi1_init(). Modeled as a real minimal device
 * (hw/misc/gnw_h7b0_spi.c, same as SPI2): found by a boot hang in
 * spi1_init() spinning on SR.TXP/SR.EOT the same way SPI2's LCD path
 * did, which a plain-RAM stub never sets.
 */
#define SPI1_BASE_ADDRESS 0x40013000

/*
 * EXTI + SYSCFG, per STM32H7B0.svd (contiguous 0x400-per-block,
 * 0x58000000-0x580007FF). Modeled as plain RAM for now -- same
 * rationale as the other not-yet-modeled peripherals above; covers
 * both in one region since they're adjacent and both commonly touched
 * during GPIO/interrupt pin-mux init.
 */
#define EXTI_SYSCFG_BASE_ADDRESS 0x58000000
#define EXTI_SYSCFG_SIZE          0x800

/*
 * DMA1 + DMA2 + DMAMUX1, per STM32H7B0.svd (contiguous 0x400-per-block,
 * 0x40020000-0x40020BFF). Modeled as plain RAM for now -- same
 * rationale as the other not-yet-modeled peripherals above.
 */
#define DMA_BASE_ADDRESS 0x40020000
#define DMA_SIZE          0xC00

/*
 * SAI1 (Serial Audio Interface), per STM32H7B0.svd baseAddress
 * 0x40015800. Modeled as plain RAM for now -- same rationale as the
 * other not-yet-modeled peripherals above.
 */
#define SAI1_BASE_ADDRESS 0x40015800
#define SAI1_SIZE          0x400

/*
 * RTC, per STM32H7B0.svd baseAddress 0x58004000. Modeled as plain RAM
 * for now -- same rationale as the other not-yet-modeled peripherals
 * above.
 */
#define RTC_BASE_ADDRESS 0x58004000
#define RTC_SIZE          0x400

/*
 * LTDC (LCD-TFT Display Controller), per STM32H7B0.svd baseAddress
 * 0x50001000. Modeled as a real minimal device
 * (hw/display/gnw_h7b0_ltdc.c): Layer1-only, RGB565-only, no
 * timing/IRQ modeling. Found by a gnw-chainloader boot reaching real
 * LTDC init after all other Phase 1 boot-path gaps were closed -- see
 * STATUS.md and gnw_h7b0_ltdc.h.
 */
#define LTDC_BASE_ADDRESS 0x50001000

struct GnwH7B0State {
    SysBusDevice parent_obj;

    ARMv7MState armv7m;
    GnwH7B0RccState rcc;
    GnwH7B0PwrState pwr;
    GnwH7B0OspiState octospi1;
    GnwH7B0OspiState octospi2;
    GnwH7B0AdcState adc;
    GnwH7B0LtdcState ltdc;
    GnwH7B0SpiState spi2;
    GnwH7B0SpiState spi1;

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
    MemoryRegion dbgmcu;
    MemoryRegion flash_r;
    MemoryRegion fmc;
    MemoryRegion gpio;
    MemoryRegion crs;
    MemoryRegion octospim;
    MemoryRegion exti_syscfg;
    MemoryRegion dma;
    MemoryRegion sai1;
    MemoryRegion rtc;

    Clock *sysclk;
};

#endif
