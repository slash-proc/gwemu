/* Auto-generated from STM32H7B0.svd for LPUART1 */
#ifndef GNW_H7B0_REGS_LPUART1_H
#define GNW_H7B0_REGS_LPUART1_H

#include <stdint.h>

#define GNW_H7B0_LPUART1_CR1_OFFSET 0x0
#define GNW_H7B0_LPUART1_CR1_RESET  0x00000000
#define GNW_H7B0_LPUART1_CR1_WMASK  0xf3ff7fff

#define GNW_H7B0_LPUART1_CR2_OFFSET 0x4
#define GNW_H7B0_LPUART1_CR2_RESET  0x00000000
#define GNW_H7B0_LPUART1_CR2_WMASK  0xff0fb010

#define GNW_H7B0_LPUART1_CR3_OFFSET 0x8
#define GNW_H7B0_LPUART1_CR3_RESET  0x00000000
#define GNW_H7B0_LPUART1_CR3_WMASK  0xfef0f7c9

#define GNW_H7B0_LPUART1_BRR_OFFSET 0xc
#define GNW_H7B0_LPUART1_BRR_RESET  0x00000000
#define GNW_H7B0_LPUART1_BRR_WMASK  0x000fffff

#define GNW_H7B0_LPUART1_GTPR_OFFSET 0x10
#define GNW_H7B0_LPUART1_GTPR_RESET  0x00000000
#define GNW_H7B0_LPUART1_GTPR_WMASK  0x0000ffff

#define GNW_H7B0_LPUART1_RTOR_OFFSET 0x14
#define GNW_H7B0_LPUART1_RTOR_RESET  0x00000000
#define GNW_H7B0_LPUART1_RTOR_WMASK  0xffffffff

#define GNW_H7B0_LPUART1_RQR_OFFSET 0x18
#define GNW_H7B0_LPUART1_RQR_RESET  0x00000000
#define GNW_H7B0_LPUART1_RQR_WMASK  0x0000001f

#define GNW_H7B0_LPUART1_ISR_OFFSET 0x1c
#define GNW_H7B0_LPUART1_ISR_RESET  0x000000c0
#define GNW_H7B0_LPUART1_ISR_WMASK  0x00000000

#define GNW_H7B0_LPUART1_ICR_OFFSET 0x20
#define GNW_H7B0_LPUART1_ICR_RESET  0x00000000
#define GNW_H7B0_LPUART1_ICR_WMASK  0x0012025f

#define GNW_H7B0_LPUART1_RDR_OFFSET 0x24
#define GNW_H7B0_LPUART1_RDR_RESET  0x00000000
#define GNW_H7B0_LPUART1_RDR_WMASK  0x00000000

#define GNW_H7B0_LPUART1_TDR_OFFSET 0x28
#define GNW_H7B0_LPUART1_TDR_RESET  0x00000000
#define GNW_H7B0_LPUART1_TDR_WMASK  0x000001ff

#define GNW_H7B0_LPUART1_PRESC_OFFSET 0x2c
#define GNW_H7B0_LPUART1_PRESC_RESET  0x00000000
#define GNW_H7B0_LPUART1_PRESC_WMASK  0x0000000f

static inline uint32_t get_lpuart1_write_mask(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_LPUART1_CR1_OFFSET:
            return 0xf3ff7fff;
        case GNW_H7B0_LPUART1_CR2_OFFSET:
            return 0xff0fb010;
        case GNW_H7B0_LPUART1_CR3_OFFSET:
            return 0xfef0f7c9;
        case GNW_H7B0_LPUART1_BRR_OFFSET:
            return 0x000fffff;
        case GNW_H7B0_LPUART1_GTPR_OFFSET:
            return 0x0000ffff;
        case GNW_H7B0_LPUART1_RTOR_OFFSET:
            return 0xffffffff;
        case GNW_H7B0_LPUART1_RQR_OFFSET:
            return 0x0000001f;
        case GNW_H7B0_LPUART1_ISR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_LPUART1_ICR_OFFSET:
            return 0x0012025f;
        case GNW_H7B0_LPUART1_RDR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_LPUART1_TDR_OFFSET:
            return 0x000001ff;
        case GNW_H7B0_LPUART1_PRESC_OFFSET:
            return 0x0000000f;
        default:
            return 0x00000000; /* Read-only or unmapped by default */
    }
}

static inline uint32_t get_lpuart1_reset_value(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_LPUART1_CR1_OFFSET:
            return 0x00000000;
        case GNW_H7B0_LPUART1_CR2_OFFSET:
            return 0x00000000;
        case GNW_H7B0_LPUART1_CR3_OFFSET:
            return 0x00000000;
        case GNW_H7B0_LPUART1_BRR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_LPUART1_GTPR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_LPUART1_RTOR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_LPUART1_RQR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_LPUART1_ISR_OFFSET:
            return 0x000000c0;
        case GNW_H7B0_LPUART1_ICR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_LPUART1_RDR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_LPUART1_TDR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_LPUART1_PRESC_OFFSET:
            return 0x00000000;
        default:
            return 0x00000000;
    }
}

#endif /* GNW_H7B0_REGS_LPUART1_H */
