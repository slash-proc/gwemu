/* Auto-generated stub for CRS */
#ifndef HW_MISC_GNW_H7B0_CRS_H
#define HW_MISC_GNW_H7B0_CRS_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_CRS "gnw-h7b0-crs"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0CrsState, GNW_H7B0_CRS)

#define GNW_H7B0_CRS_SIZE 0x400

struct GnwH7B0CrsState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_CRS_SIZE / 4];
};

#endif
