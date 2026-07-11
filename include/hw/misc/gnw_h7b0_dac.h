/* Auto-generated stub for DAC */
#ifndef HW_MISC_GNW_H7B0_DAC_H
#define HW_MISC_GNW_H7B0_DAC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_DAC "gnw-h7b0-dac"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0DacState, GNW_H7B0_DAC)

#define GNW_H7B0_DAC_SIZE 0x400

struct GnwH7B0DacState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_DAC_SIZE / 4];
};

#endif
