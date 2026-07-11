/* Auto-generated stub for OCTOSPIM */
#ifndef HW_MISC_GNW_H7B0_OCTOSPIM_H
#define HW_MISC_GNW_H7B0_OCTOSPIM_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_OCTOSPIM "gnw-h7b0-octospim"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0OctospimState, GNW_H7B0_OCTOSPIM)

#define GNW_H7B0_OCTOSPIM_SIZE 0x400

struct GnwH7B0OctospimState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_OCTOSPIM_SIZE / 4];
};

#endif
