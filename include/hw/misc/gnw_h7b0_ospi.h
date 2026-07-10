/*
 * STM32H7B0 OCTOSPI minimal stub (Nintendo Game & Watch)
 *
 * Not cycle-accurate, not a real command/address/data-phase transfer
 * model -- no actual bytes move to/from the external flash chip yet
 * (that's future work once a real extflash image is available; see
 * docs/roadmap.md). Purpose: ST HAL's HAL_OSPI_Command() (used by
 * both the no-data and indirect-write paths in
 * sdk/stm32h7xx-hal-driver/Src/stm32h7xx_hal_ospi.c) writes the
 * command config registers (CCR/IR last) then polls SR's TCF bit, and
 * __HAL_OSPI_CLEAR_FLAG() writes FCR to W1C the corresponding SR bit.
 * HAL_OSPI_Receive() (the indirect-read path) triggers the actual
 * transfer differently: it re-writes AR (if the command has an
 * address phase, e.g. an SFDP read) or IR (if not) *after*
 * HAL_OSPI_Command() has already configured -- but not started -- the
 * transaction, then polls SR's FT|TC bits per byte. Without TCF also
 * being set on an AR write, that second trigger path hangs forever
 * (found via a gnw-chainloader boot hanging in OSPI_GetFlashSizeSfdp()
 * -- a genuine silent infinite loop, not a BusFault, since AR is a
 * real backed register and the read just never completes). Every
 * other register is a plain read-what-was-written shadow with no side
 * effects.
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

#ifndef HW_MISC_GNW_H7B0_OSPI_H
#define HW_MISC_GNW_H7B0_OSPI_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_OSPI "gnw-h7b0-ospi"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0OspiState, GNW_H7B0_OSPI)

/*
 * Register offsets/bits below are from
 * sdk/cmsis-device-h7/Include/stm32h7b0xx.h's OCTOSPI_TypeDef and
 * OCTOSPI_SR_x / OCTOSPI_FCR_x bit definitions, cross-checked against
 * STM32H7B0.svd.
 */
#define GNW_H7B0_OSPI_SIZE  0x1000

#define GNW_H7B0_OSPI_SR    0x20
#define OSPI_SR_TEF         (1U << 0)
#define OSPI_SR_TCF         (1U << 1)
#define OSPI_SR_FTF         (1U << 2)
#define OSPI_SR_SMF         (1U << 3)
#define OSPI_SR_TOF         (1U << 4)
#define OSPI_SR_BUSY        (1U << 5)

#define GNW_H7B0_OSPI_FCR   0x24
/* FCR bit positions mirror SR's TEF/TCF/FTF/SMF/TOF exactly. */

#define GNW_H7B0_OSPI_CCR   0x100
#define GNW_H7B0_OSPI_IR    0x110
#define GNW_H7B0_OSPI_AR    0x48

struct GnwH7B0OspiState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_OSPI_SIZE / 4];
};

#endif
