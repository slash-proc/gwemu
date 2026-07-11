/* Auto-generated stub for FMC */
#ifndef HW_MISC_GNW_H7B0_FMC_H
#define HW_MISC_GNW_H7B0_FMC_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_FMC "gnw-h7b0-fmc"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0FmcState, GNW_H7B0_FMC)

#define GNW_H7B0_FMC_SIZE 0x1000

struct GnwH7B0FmcState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_FMC_SIZE / 4];
};

#endif
