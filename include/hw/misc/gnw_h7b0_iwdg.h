/* Auto-generated stub for IWDG */
#ifndef HW_MISC_GNW_H7B0_IWDG_H
#define HW_MISC_GNW_H7B0_IWDG_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_IWDG "gnw-h7b0-iwdg"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0IwdgState, GNW_H7B0_IWDG)

#define GNW_H7B0_IWDG_SIZE 0x400

struct GnwH7B0IwdgState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_IWDG_SIZE / 4];
};

#endif
