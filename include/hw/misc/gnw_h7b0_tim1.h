/* Auto-generated stub for TIM1 */
#ifndef HW_MISC_GNW_H7B0_TIM1_H
#define HW_MISC_GNW_H7B0_TIM1_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_TIM1 "gnw-h7b0-tim1"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0Tim1State, GNW_H7B0_TIM1)

#define GNW_H7B0_TIM1_SIZE 0x400

struct GnwH7B0Tim1State {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_TIM1_SIZE / 4];
};

#endif
