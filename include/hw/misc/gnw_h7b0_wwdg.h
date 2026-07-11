/* Auto-generated stub for WWDG */
#ifndef HW_MISC_GNW_H7B0_WWDG_H
#define HW_MISC_GNW_H7B0_WWDG_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_WWDG "gnw-h7b0-wwdg"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0WwdgState, GNW_H7B0_WWDG)

#define GNW_H7B0_WWDG_SIZE 0x400

struct GnwH7B0WwdgState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_WWDG_SIZE / 4];
};

#endif
