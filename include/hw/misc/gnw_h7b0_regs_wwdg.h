/* Auto-generated from STM32H7B0.svd for WWDG */
#ifndef GNW_H7B0_REGS_WWDG_H
#define GNW_H7B0_REGS_WWDG_H

#include <stdint.h>

#define GNW_H7B0_WWDG_CR_OFFSET 0x0
#define GNW_H7B0_WWDG_CR_RESET  0x0000007f
#define GNW_H7B0_WWDG_CR_WMASK  0x000000ff

#define GNW_H7B0_WWDG_CFR_OFFSET 0x4
#define GNW_H7B0_WWDG_CFR_RESET  0x0000007f
#define GNW_H7B0_WWDG_CFR_WMASK  0x00001a7f

#define GNW_H7B0_WWDG_SR_OFFSET 0x8
#define GNW_H7B0_WWDG_SR_RESET  0x00000000
#define GNW_H7B0_WWDG_SR_WMASK  0x00000001

static inline uint32_t get_wwdg_write_mask(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_WWDG_CR_OFFSET:
            return GNW_H7B0_WWDG_CR_WMASK;
        case GNW_H7B0_WWDG_CFR_OFFSET:
            return GNW_H7B0_WWDG_CFR_WMASK;
        case GNW_H7B0_WWDG_SR_OFFSET:
            return GNW_H7B0_WWDG_SR_WMASK;
        default:
            return 0x00000000; /* Read-only or unmapped by default */
    }
}

static inline uint32_t get_wwdg_reset_value(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_WWDG_CR_OFFSET:
            return GNW_H7B0_WWDG_CR_RESET;
        case GNW_H7B0_WWDG_CFR_OFFSET:
            return GNW_H7B0_WWDG_CFR_RESET;
        case GNW_H7B0_WWDG_SR_OFFSET:
            return GNW_H7B0_WWDG_SR_RESET;
        default:
            return 0x00000000;
    }
}

#endif /* GNW_H7B0_REGS_WWDG_H */
