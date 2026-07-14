/* Auto-generated stub for LPUART1 */
#ifndef HW_MISC_GNW_H7B0_LPUART1_H
#define HW_MISC_GNW_H7B0_LPUART1_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_LPUART1 "gnw-h7b0-lpuart1"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0Lpuart1State, GNW_H7B0_LPUART1)

#define GNW_H7B0_LPUART1_SIZE 0x400

struct GnwH7B0Lpuart1State {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_LPUART1_SIZE / 4];
};

#endif
