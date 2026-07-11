/* Auto-generated from STM32H7B0.svd for CRC */
#ifndef GNW_H7B0_REGS_CRC_H
#define GNW_H7B0_REGS_CRC_H

#include <stdint.h>

#define GNW_H7B0_CRC_DR_OFFSET 0x0
#define GNW_H7B0_CRC_DR_RESET  0xffffffff
#define GNW_H7B0_CRC_DR_WMASK  0xffffffff

#define GNW_H7B0_CRC_IDR_OFFSET 0x4
#define GNW_H7B0_CRC_IDR_RESET  0x00000000
#define GNW_H7B0_CRC_IDR_WMASK  0xffffffff

#define GNW_H7B0_CRC_CR_OFFSET 0x8
#define GNW_H7B0_CRC_CR_RESET  0x00000000
#define GNW_H7B0_CRC_CR_WMASK  0x000000f9

#define GNW_H7B0_CRC_INIT_OFFSET 0x10
#define GNW_H7B0_CRC_INIT_RESET  0x00000000
#define GNW_H7B0_CRC_INIT_WMASK  0xffffffff

#define GNW_H7B0_CRC_POL_OFFSET 0x14
#define GNW_H7B0_CRC_POL_RESET  0x00000000
#define GNW_H7B0_CRC_POL_WMASK  0xffffffff

static inline uint32_t get_crc_write_mask(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_CRC_DR_OFFSET:
            return GNW_H7B0_CRC_DR_WMASK;
        case GNW_H7B0_CRC_IDR_OFFSET:
            return GNW_H7B0_CRC_IDR_WMASK;
        case GNW_H7B0_CRC_CR_OFFSET:
            return GNW_H7B0_CRC_CR_WMASK;
        case GNW_H7B0_CRC_INIT_OFFSET:
            return GNW_H7B0_CRC_INIT_WMASK;
        case GNW_H7B0_CRC_POL_OFFSET:
            return GNW_H7B0_CRC_POL_WMASK;
        default:
            return 0x00000000; /* Read-only or unmapped by default */
    }
}

static inline uint32_t get_crc_reset_value(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_CRC_DR_OFFSET:
            return GNW_H7B0_CRC_DR_RESET;
        case GNW_H7B0_CRC_IDR_OFFSET:
            return GNW_H7B0_CRC_IDR_RESET;
        case GNW_H7B0_CRC_CR_OFFSET:
            return GNW_H7B0_CRC_CR_RESET;
        case GNW_H7B0_CRC_INIT_OFFSET:
            return GNW_H7B0_CRC_INIT_RESET;
        case GNW_H7B0_CRC_POL_OFFSET:
            return GNW_H7B0_CRC_POL_RESET;
        default:
            return 0x00000000;
    }
}

#endif /* GNW_H7B0_REGS_CRC_H */
