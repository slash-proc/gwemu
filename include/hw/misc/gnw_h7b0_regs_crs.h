/* Auto-generated from STM32H7B0.svd for CRS */
#ifndef GNW_H7B0_REGS_CRS_H
#define GNW_H7B0_REGS_CRS_H

#include <stdint.h>

#define GNW_H7B0_CRS_CR_OFFSET 0x0
#define GNW_H7B0_CRS_CR_RESET  0x00002000
#define GNW_H7B0_CRS_CR_WMASK  0x00003f6f

#define GNW_H7B0_CRS_CFGR_OFFSET 0x4
#define GNW_H7B0_CRS_CFGR_RESET  0x2022bb7f
#define GNW_H7B0_CRS_CFGR_WMASK  0xb7ffffff

#define GNW_H7B0_CRS_ISR_OFFSET 0x8
#define GNW_H7B0_CRS_ISR_RESET  0x00000000
#define GNW_H7B0_CRS_ISR_WMASK  0x00000000

#define GNW_H7B0_CRS_ICR_OFFSET 0xc
#define GNW_H7B0_CRS_ICR_RESET  0x00000000
#define GNW_H7B0_CRS_ICR_WMASK  0x0000000f

static inline uint32_t get_crs_write_mask(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_CRS_CR_OFFSET:
            return GNW_H7B0_CRS_CR_WMASK;
        case GNW_H7B0_CRS_CFGR_OFFSET:
            return GNW_H7B0_CRS_CFGR_WMASK;
        case GNW_H7B0_CRS_ISR_OFFSET:
            return GNW_H7B0_CRS_ISR_WMASK;
        case GNW_H7B0_CRS_ICR_OFFSET:
            return GNW_H7B0_CRS_ICR_WMASK;
        default:
            return 0x00000000; /* Read-only or unmapped by default */
    }
}

static inline uint32_t get_crs_reset_value(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_CRS_CR_OFFSET:
            return GNW_H7B0_CRS_CR_RESET;
        case GNW_H7B0_CRS_CFGR_OFFSET:
            return GNW_H7B0_CRS_CFGR_RESET;
        case GNW_H7B0_CRS_ISR_OFFSET:
            return GNW_H7B0_CRS_ISR_RESET;
        case GNW_H7B0_CRS_ICR_OFFSET:
            return GNW_H7B0_CRS_ICR_RESET;
        default:
            return 0x00000000;
    }
}

#endif /* GNW_H7B0_REGS_CRS_H */
