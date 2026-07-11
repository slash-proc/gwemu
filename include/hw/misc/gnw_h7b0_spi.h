/*
 * STM32H7B0 SPI minimal stub (Nintendo Game & Watch)
 *
 * Not cycle-accurate. Two modes, selected by the "sd-card" qdev
 * property (default off):
 *
 *  - off (SPI2, the LCD-panel init-command path): bytes written to
 *    TXDR go nowhere and RXDR always reads 0. Purpose: real
 *    firmware's polled SPI transmit loop (gw_lcd_spi_tx() in
 *    gnw-chainloader's src/chainloader/gui.c, separate from the LTDC
 *    pixel-data path) sets CR1.CSTART, waits for SR.TXP before each
 *    TXDR write, then waits for SR.EOT after the last one, and must
 *    not hang forever doing so.
 *  - on (SPI1, the "Tim" dedicated-pin SD-card mod path, see
 *    gnw-chainloader's src/chainloader/storage/sdcard.c): each TXDR
 *    write is forwarded as a real SSI byte exchange (ssi_transfer())
 *    to an attached hw/sd/ssi-sd.c + hw/sd/sd.c virtual SD card (real
 *    CMD0/CMD8/ACMD41/CMD58/CMD16/CMD17/CMD24 protocol, validated
 *    upstream against real Linux/U-Boot SD-over-SPI drivers), and the
 *    response byte is placed in RXDR. No chip-select GPIO is wired up
 *    (real firmware toggles CS via a plain GPIO pin, GPIOB9, not SPI1
 *    hardware NSS) -- deliberately relying on ssi-sd's default
 *    always-selected behavior (SSI_CS_LOW polarity + an unwired cs
 *    input defaults inactive-low, i.e. asserted) since this is a
 *    single-device dedicated bus (nothing else to conflict with) and
 *    the SD-over-SPI command parser only acts on bytes matching a
 *    real command frame's marker bit, so stray idle/reset 0xFF clocks
 *    sent while real firmware thinks the card is deselected are
 *    harmless no-ops to it either way. With no `-drive if=sd,...`
 *    passed, the card model has no backing image and behaves as
 *    "ejected" (same fast "no card" signal as before this device
 *    existed, now via real protocol bytes instead of a stub).
 *
 *    SR.RXP ("Rx-Packet available") must also be set alongside TXP/EOT
 *    on every completed byte transfer -- real firmware built against
 *    the STM32H7 HAL (HAL_SPI_TransmitReceive et al) polls RXP, not
 *    EOT, before reading each received byte out of RXDR. Missing this
 *    made every real receive silently time out and return whatever
 *    stale/uninitialized stack memory the caller's buffer happened to
 *    hold instead of the real byte already sitting in RXDR -- found
 *    via retro-go-sd's SD_SendCmd(CMD0) returning the literal CMD0 CRC
 *    byte (0x95) it had just transmitted moments earlier, rather than
 *    the card's real R1 response, causing a spurious "No SD Card
 *    found" even with a real -drive if=sd image attached.
 *
 * Every other register is a plain read-what-was-written shadow with
 * no side effects. Covers a single SPI instance; the SoC maps one of
 * these per SPI peripheral it needs (SPI1 and SPI2 -- see
 * gnw_h7b0_soc.c).
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

#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
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
#define SPI_SR_RXP          (1U << 0)
#define SPI_SR_TXP          (1U << 1)
#define SPI_SR_EOT          (1U << 3)

#define GNW_H7B0_SPI_IFCR   0x18
/* IFCR bit positions mirror SR's TXP/EOT/etc exactly. */

#define GNW_H7B0_SPI_TXDR   0x20
#define GNW_H7B0_SPI_RXDR   0x30

struct GnwH7B0SpiState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_SPI_SIZE / 4];
    /* See gnw_h7b0_stub_log.h -- one-shot LOG_UNIMP per plain-shadow offset. */
    bool logged_unimp[GNW_H7B0_SPI_SIZE / 4];

    bool sd_card;
    SSIBus *ssi;
};

#endif
