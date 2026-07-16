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
/*
 * STM32H7B0.svd lists this register's <resetValue> as 0x00000000, but
 * that's wrong -- RM0455 22.4.4 "CRC independent init value (CRC_INIT)"
 * explicitly states "Reset value: 0xFFFF FFFF" (the standard CRC-32
 * initial value, matching stm32h7xx_hal_crc.h's DEFAULT_CRC_INITVALUE).
 * Firmware that doesn't explicitly reprogram INIT (relying on the
 * documented hardware default, e.g. the diag suite's crypto_crc32 case)
 * would otherwise see every CRC computation start from zero instead of
 * all-ones, producing a wrong result. Same discrepancy class as
 * GNW_H7B0_CRC_POL_RESET above.
 */
#define GNW_H7B0_CRC_INIT_RESET  0xffffffff
#define GNW_H7B0_CRC_INIT_WMASK  0xffffffff

#define GNW_H7B0_CRC_POL_OFFSET 0x14
/*
 * STM32H7B0.svd lists this register's <resetValue> as 0x00000000, but
 * that's wrong -- RM0455 22.4.5 "CRC polynomial (CRC_POL)" explicitly
 * states "Reset value: 0x04C1 1DB7" (the standard CRC-32 polynomial,
 * matching stm32h7xx_hal_crc.h's DEFAULT_CRC32_POLY / stm32h7xx_ll_crc.h's
 * LL_CRC_DEFAULT_CRC32_POLY). Firmware that doesn't explicitly reprogram
 * POL (relying on the documented hardware default) would otherwise see
 * every CRC computation run against a zero polynomial here.
 */
#define GNW_H7B0_CRC_POL_RESET  0x04c11db7
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
