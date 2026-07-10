/*
 * STM32H7B0 SPI minimal stub (Nintendo Game & Watch)
 *
 * Not cycle-accurate, no real bit-banging to any peripheral -- bytes
 * written to TXDR go nowhere (there's no attached LCD-panel model yet)
 * and RXDR always reads 0. Purpose: real firmware's polled SPI
 * transmit loop (gw_lcd_spi_tx() in gnw-chainloader's src/chainloader/
 * gui.c, used for LCD panel init commands, separate from the LTDC
 * pixel-data path) sets CR1.CSTART, waits for SR.TXP before each
 * TXDR write, then waits for SR.EOT after the last one, and must not
 * hang forever doing so. Every other register is a plain
 * read-what-was-written shadow with no side effects. Covers a single
 * SPI instance; the SoC maps one of these per SPI peripheral it needs
 * (currently just SPI2 -- see gnw_h7b0_soc.c).
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

#ifndef HW_MISC_GNW_H7B0_SPI_H
#define HW_MISC_GNW_H7B0_SPI_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_SPI "gnw-h7b0-spi"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0SpiState, GNW_H7B0_SPI)

/*
 * Register offsets/bits below are from
 * sdk/cmsis-device-h7/Include/stm32h7b0xx.h's SPI_TypeDef and
 * SPI_CR1_x / SPI_SR_x / SPI_IFCR_x bit definitions.
 */
#define GNW_H7B0_SPI_SIZE   0x400

#define GNW_H7B0_SPI_CR1    0x00
#define SPI_CR1_SPE         (1U << 0)
#define SPI_CR1_CSTART      (1U << 9)

#define GNW_H7B0_SPI_SR     0x14
#define SPI_SR_TXP          (1U << 1)
#define SPI_SR_EOT          (1U << 3)

#define GNW_H7B0_SPI_IFCR   0x18
/* IFCR bit positions mirror SR's TXP/EOT/etc exactly. */

#define GNW_H7B0_SPI_TXDR   0x20

struct GnwH7B0SpiState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_SPI_SIZE / 4];
};

#endif
