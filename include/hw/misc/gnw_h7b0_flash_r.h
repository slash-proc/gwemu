/* Auto-generated stub for FLASH_R */
#ifndef HW_MISC_GNW_H7B0_FLASH_R_H
#define HW_MISC_GNW_H7B0_FLASH_R_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_FLASH_R "gnw-h7b0-flash_r"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0FlashRState, GNW_H7B0_FLASH_R)

#define GNW_H7B0_FLASH_R_SIZE 0x1000

struct GnwH7B0FlashRState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_FLASH_R_SIZE / 4];
};

#endif
