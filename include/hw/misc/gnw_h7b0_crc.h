/*
 * STM32H7B0 hardware CRC-32 unit (Nintendo Game & Watch)
 *
 * Real bit-serial CRC engine, not a lookup-table shortcut dressed up
 * as hardware -- see gnw_h7b0_crc.c for why: gnw-chainloader's
 * ofw_verify.c uses this peripheral (not a software CRC32) to verify
 * OSPI flash content against baked-in expected checksums
 * (ofw_crc32(), src/chainloader/system/ofw_verify.c), and a plain-RAM
 * stub that always reads DR back as 0 makes every verification fail
 * -- found after gnw_h7b0_ospi.c's real command decoding let boot
 * progress far enough to reach ofw_crc32() for the first time (it
 * BusFaulted on the entirely-unmapped CRC peripheral before that).
 * A wrong/fake CRC result here is arguably worse than an unmapped
 * BusFault: it fails silently as "flash content is corrupt" instead
 * of a loud, obviously-a-QEMU-gap crash.
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

#ifndef HW_MISC_GNW_H7B0_CRC_H
#define HW_MISC_GNW_H7B0_CRC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_CRC "gnw-h7b0-crc"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0CrcState, GNW_H7B0_CRC)

/* Register offsets/reset values below are from STM32H7B0.svd's CRC peripheral. */
#define GNW_H7B0_CRC_SIZE  0x400

#define GNW_H7B0_CRC_DR    0x00
#define GNW_H7B0_CRC_IDR   0x04
#define GNW_H7B0_CRC_CR    0x08
#define GNW_H7B0_CRC_INIT  0x10
#define GNW_H7B0_CRC_POL   0x14

#define CRC_CR_RESET       (1U << 0)
#define CRC_CR_POLYSIZE_SHIFT  3
#define CRC_CR_POLYSIZE_MASK   (0x3U << CRC_CR_POLYSIZE_SHIFT)
#define CRC_CR_REV_IN_SHIFT    5
#define CRC_CR_REV_IN_MASK     (0x3U << CRC_CR_REV_IN_SHIFT)
#define CRC_CR_REV_OUT         (1U << 7)

#define CRC_DR_RESET_VALUE  0xFFFFFFFFU
#define CRC_POL_RESET_VALUE 0x04C11DB7U

struct GnwH7B0CrcState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;

    uint32_t dr;    /* Current CRC accumulator (also the DR read value). */
    uint32_t idr;   /* Independent scratch byte -- no CRC-engine effect. */
    uint32_t cr;    /* POLYSIZE/REV_IN/REV_OUT; RESET self-clears. */
    uint32_t init;
    uint32_t pol;
};

#endif
