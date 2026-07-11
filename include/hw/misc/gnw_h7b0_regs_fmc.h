/* Auto-generated from STM32H7B0.svd for FMC */
#ifndef GNW_H7B0_REGS_FMC_H
#define GNW_H7B0_REGS_FMC_H

#include <stdint.h>

#define GNW_H7B0_FMC_BCR1_OFFSET 0x0
#define GNW_H7B0_FMC_BCR1_RESET  0x000030db
#define GNW_H7B0_FMC_BCR1_WMASK  0x833ffb7f

#define GNW_H7B0_FMC_BTR1_OFFSET 0x4
#define GNW_H7B0_FMC_BTR1_RESET  0x0fffffff
#define GNW_H7B0_FMC_BTR1_WMASK  0x3fffffff

#define GNW_H7B0_FMC_BCR2_OFFSET 0x8
#define GNW_H7B0_FMC_BCR2_RESET  0x000030d2
#define GNW_H7B0_FMC_BCR2_WMASK  0x833ffb7f

#define GNW_H7B0_FMC_BTR2_OFFSET 0xc
#define GNW_H7B0_FMC_BTR2_RESET  0x0fffffff
#define GNW_H7B0_FMC_BTR2_WMASK  0x3fffffff

#define GNW_H7B0_FMC_BCR3_OFFSET 0x10
#define GNW_H7B0_FMC_BCR3_RESET  0x000030d2
#define GNW_H7B0_FMC_BCR3_WMASK  0x833ffb7f

#define GNW_H7B0_FMC_BTR3_OFFSET 0x14
#define GNW_H7B0_FMC_BTR3_RESET  0x0fffffff
#define GNW_H7B0_FMC_BTR3_WMASK  0x3fffffff

#define GNW_H7B0_FMC_BCR4_OFFSET 0x18
#define GNW_H7B0_FMC_BCR4_RESET  0x000030d2
#define GNW_H7B0_FMC_BCR4_WMASK  0x833ffb7f

#define GNW_H7B0_FMC_BTR4_OFFSET 0x1c
#define GNW_H7B0_FMC_BTR4_RESET  0x0fffffff
#define GNW_H7B0_FMC_BTR4_WMASK  0x3fffffff

#define GNW_H7B0_FMC_PCR_OFFSET 0x80
#define GNW_H7B0_FMC_PCR_RESET  0x00000018
#define GNW_H7B0_FMC_PCR_WMASK  0x000ffe76

#define GNW_H7B0_FMC_SR_OFFSET 0x84
#define GNW_H7B0_FMC_SR_RESET  0x00000040
#define GNW_H7B0_FMC_SR_WMASK  0x0000003f

#define GNW_H7B0_FMC_PMEM_OFFSET 0x88
#define GNW_H7B0_FMC_PMEM_RESET  0xfcfcfcfc
#define GNW_H7B0_FMC_PMEM_WMASK  0xffffffff

#define GNW_H7B0_FMC_PATT_OFFSET 0x8c
#define GNW_H7B0_FMC_PATT_RESET  0xfcfcfcfc
#define GNW_H7B0_FMC_PATT_WMASK  0xffffffff

#define GNW_H7B0_FMC_ECCR_OFFSET 0x94
#define GNW_H7B0_FMC_ECCR_RESET  0x00000000
#define GNW_H7B0_FMC_ECCR_WMASK  0x00000000

#define GNW_H7B0_FMC_BWTR1_OFFSET 0x104
#define GNW_H7B0_FMC_BWTR1_RESET  0x0fffffff
#define GNW_H7B0_FMC_BWTR1_WMASK  0x300fffff

#define GNW_H7B0_FMC_BWTR2_OFFSET 0x10c
#define GNW_H7B0_FMC_BWTR2_RESET  0x0fffffff
#define GNW_H7B0_FMC_BWTR2_WMASK  0x300fffff

#define GNW_H7B0_FMC_BWTR3_OFFSET 0x114
#define GNW_H7B0_FMC_BWTR3_RESET  0x0fffffff
#define GNW_H7B0_FMC_BWTR3_WMASK  0x300fffff

#define GNW_H7B0_FMC_BWTR4_OFFSET 0x11c
#define GNW_H7B0_FMC_BWTR4_RESET  0x0fffffff
#define GNW_H7B0_FMC_BWTR4_WMASK  0x300fffff

#define GNW_H7B0_FMC_SDCR1_OFFSET 0x140
#define GNW_H7B0_FMC_SDCR1_RESET  0x000002d0
#define GNW_H7B0_FMC_SDCR1_WMASK  0x00007fff

#define GNW_H7B0_FMC_SDCR2_OFFSET 0x144
#define GNW_H7B0_FMC_SDCR2_RESET  0x000002d0
#define GNW_H7B0_FMC_SDCR2_WMASK  0x00007fff

#define GNW_H7B0_FMC_SDTR1_OFFSET 0x148
#define GNW_H7B0_FMC_SDTR1_RESET  0x0fffffff
#define GNW_H7B0_FMC_SDTR1_WMASK  0x0fffffff

#define GNW_H7B0_FMC_SDTR2_OFFSET 0x14c
#define GNW_H7B0_FMC_SDTR2_RESET  0x0fffffff
#define GNW_H7B0_FMC_SDTR2_WMASK  0x0fffffff

#define GNW_H7B0_FMC_SDCMR_OFFSET 0x150
#define GNW_H7B0_FMC_SDCMR_RESET  0x00000000
#define GNW_H7B0_FMC_SDCMR_WMASK  0x007fffff

#define GNW_H7B0_FMC_SDRTR_OFFSET 0x154
#define GNW_H7B0_FMC_SDRTR_RESET  0x00000000
#define GNW_H7B0_FMC_SDRTR_WMASK  0x00007fff

#define GNW_H7B0_FMC_SDSR_OFFSET 0x158
#define GNW_H7B0_FMC_SDSR_RESET  0x00000000
#define GNW_H7B0_FMC_SDSR_WMASK  0x00000000

static inline uint32_t get_fmc_write_mask(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_FMC_BCR1_OFFSET:
            return GNW_H7B0_FMC_BCR1_WMASK;
        case GNW_H7B0_FMC_BTR1_OFFSET:
            return GNW_H7B0_FMC_BTR1_WMASK;
        case GNW_H7B0_FMC_BCR2_OFFSET:
            return GNW_H7B0_FMC_BCR2_WMASK;
        case GNW_H7B0_FMC_BTR2_OFFSET:
            return GNW_H7B0_FMC_BTR2_WMASK;
        case GNW_H7B0_FMC_BCR3_OFFSET:
            return GNW_H7B0_FMC_BCR3_WMASK;
        case GNW_H7B0_FMC_BTR3_OFFSET:
            return GNW_H7B0_FMC_BTR3_WMASK;
        case GNW_H7B0_FMC_BCR4_OFFSET:
            return GNW_H7B0_FMC_BCR4_WMASK;
        case GNW_H7B0_FMC_BTR4_OFFSET:
            return GNW_H7B0_FMC_BTR4_WMASK;
        case GNW_H7B0_FMC_PCR_OFFSET:
            return GNW_H7B0_FMC_PCR_WMASK;
        case GNW_H7B0_FMC_SR_OFFSET:
            return GNW_H7B0_FMC_SR_WMASK;
        case GNW_H7B0_FMC_PMEM_OFFSET:
            return GNW_H7B0_FMC_PMEM_WMASK;
        case GNW_H7B0_FMC_PATT_OFFSET:
            return GNW_H7B0_FMC_PATT_WMASK;
        case GNW_H7B0_FMC_ECCR_OFFSET:
            return GNW_H7B0_FMC_ECCR_WMASK;
        case GNW_H7B0_FMC_BWTR1_OFFSET:
            return GNW_H7B0_FMC_BWTR1_WMASK;
        case GNW_H7B0_FMC_BWTR2_OFFSET:
            return GNW_H7B0_FMC_BWTR2_WMASK;
        case GNW_H7B0_FMC_BWTR3_OFFSET:
            return GNW_H7B0_FMC_BWTR3_WMASK;
        case GNW_H7B0_FMC_BWTR4_OFFSET:
            return GNW_H7B0_FMC_BWTR4_WMASK;
        case GNW_H7B0_FMC_SDCR1_OFFSET:
            return GNW_H7B0_FMC_SDCR1_WMASK;
        case GNW_H7B0_FMC_SDCR2_OFFSET:
            return GNW_H7B0_FMC_SDCR2_WMASK;
        case GNW_H7B0_FMC_SDTR1_OFFSET:
            return GNW_H7B0_FMC_SDTR1_WMASK;
        case GNW_H7B0_FMC_SDTR2_OFFSET:
            return GNW_H7B0_FMC_SDTR2_WMASK;
        case GNW_H7B0_FMC_SDCMR_OFFSET:
            return GNW_H7B0_FMC_SDCMR_WMASK;
        case GNW_H7B0_FMC_SDRTR_OFFSET:
            return GNW_H7B0_FMC_SDRTR_WMASK;
        case GNW_H7B0_FMC_SDSR_OFFSET:
            return GNW_H7B0_FMC_SDSR_WMASK;
        default:
            return 0x00000000; /* Read-only or unmapped by default */
    }
}

static inline uint32_t get_fmc_reset_value(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_FMC_BCR1_OFFSET:
            return GNW_H7B0_FMC_BCR1_RESET;
        case GNW_H7B0_FMC_BTR1_OFFSET:
            return GNW_H7B0_FMC_BTR1_RESET;
        case GNW_H7B0_FMC_BCR2_OFFSET:
            return GNW_H7B0_FMC_BCR2_RESET;
        case GNW_H7B0_FMC_BTR2_OFFSET:
            return GNW_H7B0_FMC_BTR2_RESET;
        case GNW_H7B0_FMC_BCR3_OFFSET:
            return GNW_H7B0_FMC_BCR3_RESET;
        case GNW_H7B0_FMC_BTR3_OFFSET:
            return GNW_H7B0_FMC_BTR3_RESET;
        case GNW_H7B0_FMC_BCR4_OFFSET:
            return GNW_H7B0_FMC_BCR4_RESET;
        case GNW_H7B0_FMC_BTR4_OFFSET:
            return GNW_H7B0_FMC_BTR4_RESET;
        case GNW_H7B0_FMC_PCR_OFFSET:
            return GNW_H7B0_FMC_PCR_RESET;
        case GNW_H7B0_FMC_SR_OFFSET:
            return GNW_H7B0_FMC_SR_RESET;
        case GNW_H7B0_FMC_PMEM_OFFSET:
            return GNW_H7B0_FMC_PMEM_RESET;
        case GNW_H7B0_FMC_PATT_OFFSET:
            return GNW_H7B0_FMC_PATT_RESET;
        case GNW_H7B0_FMC_ECCR_OFFSET:
            return GNW_H7B0_FMC_ECCR_RESET;
        case GNW_H7B0_FMC_BWTR1_OFFSET:
            return GNW_H7B0_FMC_BWTR1_RESET;
        case GNW_H7B0_FMC_BWTR2_OFFSET:
            return GNW_H7B0_FMC_BWTR2_RESET;
        case GNW_H7B0_FMC_BWTR3_OFFSET:
            return GNW_H7B0_FMC_BWTR3_RESET;
        case GNW_H7B0_FMC_BWTR4_OFFSET:
            return GNW_H7B0_FMC_BWTR4_RESET;
        case GNW_H7B0_FMC_SDCR1_OFFSET:
            return GNW_H7B0_FMC_SDCR1_RESET;
        case GNW_H7B0_FMC_SDCR2_OFFSET:
            return GNW_H7B0_FMC_SDCR2_RESET;
        case GNW_H7B0_FMC_SDTR1_OFFSET:
            return GNW_H7B0_FMC_SDTR1_RESET;
        case GNW_H7B0_FMC_SDTR2_OFFSET:
            return GNW_H7B0_FMC_SDTR2_RESET;
        case GNW_H7B0_FMC_SDCMR_OFFSET:
            return GNW_H7B0_FMC_SDCMR_RESET;
        case GNW_H7B0_FMC_SDRTR_OFFSET:
            return GNW_H7B0_FMC_SDRTR_RESET;
        case GNW_H7B0_FMC_SDSR_OFFSET:
            return GNW_H7B0_FMC_SDSR_RESET;
        default:
            return 0x00000000;
    }
}

#endif /* GNW_H7B0_REGS_FMC_H */
