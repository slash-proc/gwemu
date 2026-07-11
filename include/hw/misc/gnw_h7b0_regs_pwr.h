/* Auto-generated from STM32H7B0.svd for PWR */
#ifndef GNW_H7B0_REGS_PWR_H
#define GNW_H7B0_REGS_PWR_H

#include <stdint.h>

#define GNW_H7B0_PWR_CR1_OFFSET 0x0
#define GNW_H7B0_PWR_CR1_RESET  0xf000c000
#define GNW_H7B0_PWR_CR1_WMASK  0x0ffff3f1

#define GNW_H7B0_PWR_CSR1_OFFSET 0x4
#define GNW_H7B0_PWR_CSR1_RESET  0x00004000
#define GNW_H7B0_PWR_CSR1_WMASK  0x00000000

#define GNW_H7B0_PWR_CR2_OFFSET 0x8
#define GNW_H7B0_PWR_CR2_RESET  0x00000000
#define GNW_H7B0_PWR_CR2_WMASK  0x00000011

#define GNW_H7B0_PWR_CR3_OFFSET 0xc
#define GNW_H7B0_PWR_CR3_RESET  0x00000006
#define GNW_H7B0_PWR_CR3_WMASK  0x0300033f

#define GNW_H7B0_PWR_CPUCR_OFFSET 0x10
#define GNW_H7B0_PWR_CPUCR_RESET  0x00000000
#define GNW_H7B0_PWR_CPUCR_WMASK  0x00000a05

#define GNW_H7B0_PWR_SRDCR_OFFSET 0x18
#define GNW_H7B0_PWR_SRDCR_RESET  0x00004000
#define GNW_H7B0_PWR_SRDCR_WMASK  0x0000c000

#define GNW_H7B0_PWR_WKUPCR_OFFSET 0x20
#define GNW_H7B0_PWR_WKUPCR_RESET  0x00000000
#define GNW_H7B0_PWR_WKUPCR_WMASK  0x0000003f

#define GNW_H7B0_PWR_WKUPFR_OFFSET 0x24
#define GNW_H7B0_PWR_WKUPFR_RESET  0x00000000
#define GNW_H7B0_PWR_WKUPFR_WMASK  0x0000003f

#define GNW_H7B0_PWR_WKUPEPR_OFFSET 0x28
#define GNW_H7B0_PWR_WKUPEPR_RESET  0x00000000
#define GNW_H7B0_PWR_WKUPEPR_WMASK  0x0fff3f3f

static inline uint32_t get_pwr_write_mask(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_PWR_CR1_OFFSET:
            return GNW_H7B0_PWR_CR1_WMASK;
        case GNW_H7B0_PWR_CSR1_OFFSET:
            return GNW_H7B0_PWR_CSR1_WMASK;
        case GNW_H7B0_PWR_CR2_OFFSET:
            return GNW_H7B0_PWR_CR2_WMASK;
        case GNW_H7B0_PWR_CR3_OFFSET:
            return GNW_H7B0_PWR_CR3_WMASK;
        case GNW_H7B0_PWR_CPUCR_OFFSET:
            return GNW_H7B0_PWR_CPUCR_WMASK;
        case GNW_H7B0_PWR_SRDCR_OFFSET:
            return GNW_H7B0_PWR_SRDCR_WMASK;
        case GNW_H7B0_PWR_WKUPCR_OFFSET:
            return GNW_H7B0_PWR_WKUPCR_WMASK;
        case GNW_H7B0_PWR_WKUPFR_OFFSET:
            return GNW_H7B0_PWR_WKUPFR_WMASK;
        case GNW_H7B0_PWR_WKUPEPR_OFFSET:
            return GNW_H7B0_PWR_WKUPEPR_WMASK;
        default:
            return 0x00000000; /* Read-only or unmapped by default */
    }
}

static inline uint32_t get_pwr_reset_value(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_PWR_CR1_OFFSET:
            return GNW_H7B0_PWR_CR1_RESET;
        case GNW_H7B0_PWR_CSR1_OFFSET:
            return GNW_H7B0_PWR_CSR1_RESET;
        case GNW_H7B0_PWR_CR2_OFFSET:
            return GNW_H7B0_PWR_CR2_RESET;
        case GNW_H7B0_PWR_CR3_OFFSET:
            return GNW_H7B0_PWR_CR3_RESET;
        case GNW_H7B0_PWR_CPUCR_OFFSET:
            return GNW_H7B0_PWR_CPUCR_RESET;
        case GNW_H7B0_PWR_SRDCR_OFFSET:
            return GNW_H7B0_PWR_SRDCR_RESET;
        case GNW_H7B0_PWR_WKUPCR_OFFSET:
            return GNW_H7B0_PWR_WKUPCR_RESET;
        case GNW_H7B0_PWR_WKUPFR_OFFSET:
            return GNW_H7B0_PWR_WKUPFR_RESET;
        case GNW_H7B0_PWR_WKUPEPR_OFFSET:
            return GNW_H7B0_PWR_WKUPEPR_RESET;
        default:
            return 0x00000000;
    }
}

#endif /* GNW_H7B0_REGS_PWR_H */
