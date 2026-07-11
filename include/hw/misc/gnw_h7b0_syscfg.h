/* Auto-generated stub for SYSCFG */
#ifndef HW_MISC_GNW_H7B0_SYSCFG_H
#define HW_MISC_GNW_H7B0_SYSCFG_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_SYSCFG "gnw-h7b0-syscfg"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0SyscfgState, GNW_H7B0_SYSCFG)

#define GNW_H7B0_SYSCFG_SIZE 0x400

struct GnwH7B0SyscfgState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_SYSCFG_SIZE / 4];
};

#endif
