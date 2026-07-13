/*
 * STM32H7B0 OTFDEC (on-the-fly decryption) minimal model
 * (Nintendo Game & Watch)
 *
 * Scope: enough for stock firmware's HAL_OTFDEC_RegionSetKey() to
 * succeed -- registers are plain read/write shadows, plus the one real
 * behavior firmware checks: writing a region's KEYR3 recomputes the
 * region's key CRC (HAL_OTFDEC_KeyCRCComputation()'s exact algorithm,
 * from sdk/stm32h7xx-hal-driver/Src/stm32h7xx_hal_otfdec.c) into
 * CONFIGR[15:8] (KEYCRC), which HAL_OTFDEC_RegionSetKey() reads back
 * and compares against its own software-computed CRC before reporting
 * success. Found via 2026-07-12 lockstep tracing: stock Zelda's boot
 * traps in its own "spin forever on HAL error" loop when SetKey fails.
 *
 * NOT modeled: actual decryption of memory-mapped OCTOSPI reads.
 * Stock extflash dumps used with this machine are the raw (encrypted
 * where applicable) images; if firmware later depends on OTFDEC
 * actually transforming fetched data, that needs a real AES-CTR
 * implementation hooked into the OSPI memory-mapped path.
 *
 * Region layout (sdk/cmsis-device-h7/Include/stm32h7b0xx.h): 4 regions
 * at base+0x20, stride 0x30; per region: CONFIGR, START_ADDR, END_ADDR,
 * NONCER0, NONCER1, KEYR0..KEYR3.
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
#ifndef HW_MISC_GNW_H7B0_OTFDEC_H
#define HW_MISC_GNW_H7B0_OTFDEC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_OTFDEC "gnw-h7b0-otfdec"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0OtfdecState, GNW_H7B0_OTFDEC)

#define GNW_H7B0_OTFDEC_SIZE 0x400

#define OTFDEC_REGION_BASE   0x20
#define OTFDEC_REGION_STRIDE 0x30
#define OTFDEC_NUM_REGIONS   4

/* Per-region register offsets (from the region's own base). */
#define OTFDEC_REG_CONFIGR    0x00
#define OTFDEC_REG_START_ADDR 0x04
#define OTFDEC_REG_END_ADDR   0x08
#define OTFDEC_REG_NONCER0    0x0c
#define OTFDEC_REG_NONCER1    0x10
#define OTFDEC_REG_KEYR0      0x14
#define OTFDEC_REG_KEYR1      0x18
#define OTFDEC_REG_KEYR2      0x1c
#define OTFDEC_REG_KEYR3      0x20

#define OTFDEC_CONFIGR_KEYCRC_SHIFT 8
#define OTFDEC_CONFIGR_KEYCRC_MASK  (0xFFU << OTFDEC_CONFIGR_KEYCRC_SHIFT)

struct GnwH7B0OtfdecState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_OTFDEC_SIZE / 4];
};

#endif
