/* Auto-generated from STM32H7B0.svd for OctoSPII_O_Manager */
#ifndef GNW_H7B0_REGS_OCTOSPII_O_MANAGER_H
#define GNW_H7B0_REGS_OCTOSPII_O_MANAGER_H

#include <stdint.h>

#define GNW_H7B0_OCTOSPII_O_MANAGER_CR_OFFSET 0x0
#define GNW_H7B0_OCTOSPII_O_MANAGER_CR_RESET  0x00000000
#define GNW_H7B0_OCTOSPII_O_MANAGER_CR_WMASK  0x00ff0001

#define GNW_H7B0_OCTOSPII_O_MANAGER_P1CR_OFFSET 0x4
#define GNW_H7B0_OCTOSPII_O_MANAGER_P1CR_RESET  0x03010111
#define GNW_H7B0_OCTOSPII_O_MANAGER_P1CR_WMASK  0x07070333

#define GNW_H7B0_OCTOSPII_O_MANAGER_P2CR_OFFSET 0x8
#define GNW_H7B0_OCTOSPII_O_MANAGER_P2CR_RESET  0x07050333
#define GNW_H7B0_OCTOSPII_O_MANAGER_P2CR_WMASK  0x07070333

static inline uint32_t get_octospim_write_mask(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_OCTOSPII_O_MANAGER_CR_OFFSET:
            return GNW_H7B0_OCTOSPII_O_MANAGER_CR_WMASK;
        case GNW_H7B0_OCTOSPII_O_MANAGER_P1CR_OFFSET:
            return GNW_H7B0_OCTOSPII_O_MANAGER_P1CR_WMASK;
        case GNW_H7B0_OCTOSPII_O_MANAGER_P2CR_OFFSET:
            return GNW_H7B0_OCTOSPII_O_MANAGER_P2CR_WMASK;
        default:
            return 0x00000000; /* Read-only or unmapped by default */
    }
}

static inline uint32_t get_octospim_reset_value(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_OCTOSPII_O_MANAGER_CR_OFFSET:
            return GNW_H7B0_OCTOSPII_O_MANAGER_CR_RESET;
        case GNW_H7B0_OCTOSPII_O_MANAGER_P1CR_OFFSET:
            return GNW_H7B0_OCTOSPII_O_MANAGER_P1CR_RESET;
        case GNW_H7B0_OCTOSPII_O_MANAGER_P2CR_OFFSET:
            return GNW_H7B0_OCTOSPII_O_MANAGER_P2CR_RESET;
        default:
            return 0x00000000;
    }
}

#endif /* GNW_H7B0_REGS_OCTOSPII_O_MANAGER_H */
