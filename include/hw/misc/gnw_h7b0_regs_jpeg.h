/* Auto-generated from STM32H7B0.svd for JPEG */
#ifndef GNW_H7B0_REGS_JPEG_H
#define GNW_H7B0_REGS_JPEG_H

#include <stdint.h>

#define GNW_H7B0_JPEG_CONFR0_OFFSET 0x0
#define GNW_H7B0_JPEG_CONFR0_RESET  0x00000000
#define GNW_H7B0_JPEG_CONFR0_WMASK  0x00000001
#define JPEG_CONFR0_START            (1U << 0)

#define GNW_H7B0_JPEG_CONFR1_OFFSET 0x4
#define GNW_H7B0_JPEG_CONFR1_RESET  0x00000000
#define GNW_H7B0_JPEG_CONFR1_WMASK  0xffff01fb

#define GNW_H7B0_JPEG_CONFR2_OFFSET 0x8
#define GNW_H7B0_JPEG_CONFR2_RESET  0x00000000
#define GNW_H7B0_JPEG_CONFR2_WMASK  0x03ffffff

#define GNW_H7B0_JPEG_CONFR3_OFFSET 0xc
#define GNW_H7B0_JPEG_CONFR3_RESET  0x00000000
#define GNW_H7B0_JPEG_CONFR3_WMASK  0xffff0000

#define GNW_H7B0_JPEG_CONFRN1_OFFSET 0x10
#define GNW_H7B0_JPEG_CONFRN1_RESET  0x00000000
#define GNW_H7B0_JPEG_CONFRN1_WMASK  0x0000ffff

#define GNW_H7B0_JPEG_CONFRN2_OFFSET 0x14
#define GNW_H7B0_JPEG_CONFRN2_RESET  0x00000000
#define GNW_H7B0_JPEG_CONFRN2_WMASK  0x0000ffff

#define GNW_H7B0_JPEG_CONFRN3_OFFSET 0x18
#define GNW_H7B0_JPEG_CONFRN3_RESET  0x00000000
#define GNW_H7B0_JPEG_CONFRN3_WMASK  0x0000ffff

#define GNW_H7B0_JPEG_CONFRN4_OFFSET 0x1c
#define GNW_H7B0_JPEG_CONFRN4_RESET  0x00000000
#define GNW_H7B0_JPEG_CONFRN4_WMASK  0x0000ffff

#define GNW_H7B0_JPEG_CR_OFFSET 0x30
#define GNW_H7B0_JPEG_CR_RESET  0x00000000
#define GNW_H7B0_JPEG_CR_WMASK  0x0000787f

#define GNW_H7B0_JPEG_SR_OFFSET 0x34
#define GNW_H7B0_JPEG_SR_RESET  0x00000006
#define GNW_H7B0_JPEG_SR_WMASK  0x00000000
/* SR bit positions, per sdk/cmsis-device-h7/Include/stm32h7b0xx.h's JPEG_SR_*. */
#define JPEG_SR_IFTF  (1U << 1)
#define JPEG_SR_IFNFF (1U << 2)
#define JPEG_SR_OFTF  (1U << 3)
#define JPEG_SR_OFNEF (1U << 4)
#define JPEG_SR_EOCF  (1U << 5)
#define JPEG_SR_COF   (1U << 7)

#define GNW_H7B0_JPEG_CFR_OFFSET 0x38
#define GNW_H7B0_JPEG_CFR_RESET  0x00000000
#define GNW_H7B0_JPEG_CFR_WMASK  0x00000060

#define GNW_H7B0_JPEG_DIR_OFFSET 0x40
#define GNW_H7B0_JPEG_DIR_RESET  0x00000000
#define GNW_H7B0_JPEG_DIR_WMASK  0xffffffff

#define GNW_H7B0_JPEG_DOR_OFFSET 0x44
#define GNW_H7B0_JPEG_DOR_RESET  0x00000000
#define GNW_H7B0_JPEG_DOR_WMASK  0x00000000

static inline uint32_t get_jpeg_write_mask(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_JPEG_CONFR0_OFFSET:
            return GNW_H7B0_JPEG_CONFR0_WMASK;
        case GNW_H7B0_JPEG_CONFR1_OFFSET:
            return GNW_H7B0_JPEG_CONFR1_WMASK;
        case GNW_H7B0_JPEG_CONFR2_OFFSET:
            return GNW_H7B0_JPEG_CONFR2_WMASK;
        case GNW_H7B0_JPEG_CONFR3_OFFSET:
            return GNW_H7B0_JPEG_CONFR3_WMASK;
        case GNW_H7B0_JPEG_CONFRN1_OFFSET:
            return GNW_H7B0_JPEG_CONFRN1_WMASK;
        case GNW_H7B0_JPEG_CONFRN2_OFFSET:
            return GNW_H7B0_JPEG_CONFRN2_WMASK;
        case GNW_H7B0_JPEG_CONFRN3_OFFSET:
            return GNW_H7B0_JPEG_CONFRN3_WMASK;
        case GNW_H7B0_JPEG_CONFRN4_OFFSET:
            return GNW_H7B0_JPEG_CONFRN4_WMASK;
        case GNW_H7B0_JPEG_CR_OFFSET:
            return GNW_H7B0_JPEG_CR_WMASK;
        case GNW_H7B0_JPEG_SR_OFFSET:
            return GNW_H7B0_JPEG_SR_WMASK;
        case GNW_H7B0_JPEG_CFR_OFFSET:
            return GNW_H7B0_JPEG_CFR_WMASK;
        case GNW_H7B0_JPEG_DIR_OFFSET:
            return GNW_H7B0_JPEG_DIR_WMASK;
        case GNW_H7B0_JPEG_DOR_OFFSET:
            return GNW_H7B0_JPEG_DOR_WMASK;
        default:
            return 0x00000000; /* Read-only or unmapped by default */
    }
}

static inline uint32_t get_jpeg_reset_value(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_JPEG_CONFR0_OFFSET:
            return GNW_H7B0_JPEG_CONFR0_RESET;
        case GNW_H7B0_JPEG_CONFR1_OFFSET:
            return GNW_H7B0_JPEG_CONFR1_RESET;
        case GNW_H7B0_JPEG_CONFR2_OFFSET:
            return GNW_H7B0_JPEG_CONFR2_RESET;
        case GNW_H7B0_JPEG_CONFR3_OFFSET:
            return GNW_H7B0_JPEG_CONFR3_RESET;
        case GNW_H7B0_JPEG_CONFRN1_OFFSET:
            return GNW_H7B0_JPEG_CONFRN1_RESET;
        case GNW_H7B0_JPEG_CONFRN2_OFFSET:
            return GNW_H7B0_JPEG_CONFRN2_RESET;
        case GNW_H7B0_JPEG_CONFRN3_OFFSET:
            return GNW_H7B0_JPEG_CONFRN3_RESET;
        case GNW_H7B0_JPEG_CONFRN4_OFFSET:
            return GNW_H7B0_JPEG_CONFRN4_RESET;
        case GNW_H7B0_JPEG_CR_OFFSET:
            return GNW_H7B0_JPEG_CR_RESET;
        case GNW_H7B0_JPEG_SR_OFFSET:
            return GNW_H7B0_JPEG_SR_RESET;
        case GNW_H7B0_JPEG_CFR_OFFSET:
            return GNW_H7B0_JPEG_CFR_RESET;
        case GNW_H7B0_JPEG_DIR_OFFSET:
            return GNW_H7B0_JPEG_DIR_RESET;
        case GNW_H7B0_JPEG_DOR_OFFSET:
            return GNW_H7B0_JPEG_DOR_RESET;
        default:
            return 0x00000000;
    }
}

#endif /* GNW_H7B0_REGS_JPEG_H */
