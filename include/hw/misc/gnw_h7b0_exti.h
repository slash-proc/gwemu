/* Auto-generated stub for EXTI */
#ifndef HW_MISC_GNW_H7B0_EXTI_H
#define HW_MISC_GNW_H7B0_EXTI_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_EXTI "gnw-h7b0-exti"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0ExtiState, GNW_H7B0_EXTI)

#define GNW_H7B0_EXTI_SIZE 0x400

struct GnwH7B0ExtiState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_EXTI_SIZE / 4];
};

#endif
