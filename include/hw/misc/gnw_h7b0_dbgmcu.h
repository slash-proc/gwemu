/* Auto-generated stub for DBGMCU */
#ifndef HW_MISC_GNW_H7B0_DBGMCU_H
#define HW_MISC_GNW_H7B0_DBGMCU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_DBGMCU "gnw-h7b0-dbgmcu"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0DbgmcuState, GNW_H7B0_DBGMCU)

#define GNW_H7B0_DBGMCU_SIZE 0x400

struct GnwH7B0DbgmcuState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_DBGMCU_SIZE / 4];
};

#endif
