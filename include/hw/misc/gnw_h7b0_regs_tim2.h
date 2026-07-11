/* Auto-generated from STM32H7B0.svd for TIM2 */
#ifndef GNW_H7B0_REGS_TIM2_H
#define GNW_H7B0_REGS_TIM2_H

#include <stdint.h>

#define GNW_H7B0_TIM2_CR1_OFFSET 0x0
#define GNW_H7B0_TIM2_CR1_RESET  0x00000000
#define GNW_H7B0_TIM2_CR1_WMASK  0x00000bff

#define GNW_H7B0_TIM2_CR2_OFFSET 0x4
#define GNW_H7B0_TIM2_CR2_RESET  0x00000000
#define GNW_H7B0_TIM2_CR2_WMASK  0x000000f8

#define GNW_H7B0_TIM2_SMCR_OFFSET 0x8
#define GNW_H7B0_TIM2_SMCR_RESET  0x00000000
#define GNW_H7B0_TIM2_SMCR_WMASK  0x0031fff7

#define GNW_H7B0_TIM2_DIER_OFFSET 0xc
#define GNW_H7B0_TIM2_DIER_RESET  0x00000000
#define GNW_H7B0_TIM2_DIER_WMASK  0x00005f5f

#define GNW_H7B0_TIM2_SR_OFFSET 0x10
#define GNW_H7B0_TIM2_SR_RESET  0x00000000
#define GNW_H7B0_TIM2_SR_WMASK  0x00001e5f

#define GNW_H7B0_TIM2_EGR_OFFSET 0x14
#define GNW_H7B0_TIM2_EGR_RESET  0x00000000
#define GNW_H7B0_TIM2_EGR_WMASK  0x0000005f

#define GNW_H7B0_TIM2_CCMR1_Output_OFFSET 0x18
#define GNW_H7B0_TIM2_CCMR1_Output_RESET  0x00000000
#define GNW_H7B0_TIM2_CCMR1_Output_WMASK  0x0101ffff

#define GNW_H7B0_TIM2_CCMR1_Input_OFFSET 0x18
#define GNW_H7B0_TIM2_CCMR1_Input_RESET  0x00000000
#define GNW_H7B0_TIM2_CCMR1_Input_WMASK  0x0000ffff

#define GNW_H7B0_TIM2_CCMR2_Output_OFFSET 0x1c
#define GNW_H7B0_TIM2_CCMR2_Output_RESET  0x00000000
#define GNW_H7B0_TIM2_CCMR2_Output_WMASK  0x0101ffff

#define GNW_H7B0_TIM2_CCMR2_Input_OFFSET 0x1c
#define GNW_H7B0_TIM2_CCMR2_Input_RESET  0x00000000
#define GNW_H7B0_TIM2_CCMR2_Input_WMASK  0x0000ffff

#define GNW_H7B0_TIM2_CCER_OFFSET 0x20
#define GNW_H7B0_TIM2_CCER_RESET  0x00000000
#define GNW_H7B0_TIM2_CCER_WMASK  0x0000bbbb

#define GNW_H7B0_TIM2_CNT_OFFSET 0x24
#define GNW_H7B0_TIM2_CNT_RESET  0x00000000
#define GNW_H7B0_TIM2_CNT_WMASK  0xffffffff

#define GNW_H7B0_TIM2_PSC_OFFSET 0x28
#define GNW_H7B0_TIM2_PSC_RESET  0x00000000
#define GNW_H7B0_TIM2_PSC_WMASK  0x0000ffff

#define GNW_H7B0_TIM2_ARR_OFFSET 0x2c
#define GNW_H7B0_TIM2_ARR_RESET  0x00000000
#define GNW_H7B0_TIM2_ARR_WMASK  0xffffffff

#define GNW_H7B0_TIM2_CCR1_OFFSET 0x34
#define GNW_H7B0_TIM2_CCR1_RESET  0x00000000
#define GNW_H7B0_TIM2_CCR1_WMASK  0xffffffff

#define GNW_H7B0_TIM2_CCR2_OFFSET 0x38
#define GNW_H7B0_TIM2_CCR2_RESET  0x00000000
#define GNW_H7B0_TIM2_CCR2_WMASK  0xffffffff

#define GNW_H7B0_TIM2_CCR3_OFFSET 0x3c
#define GNW_H7B0_TIM2_CCR3_RESET  0x00000000
#define GNW_H7B0_TIM2_CCR3_WMASK  0xffffffff

#define GNW_H7B0_TIM2_CCR4_OFFSET 0x40
#define GNW_H7B0_TIM2_CCR4_RESET  0x00000000
#define GNW_H7B0_TIM2_CCR4_WMASK  0xffffffff

#define GNW_H7B0_TIM2_DCR_OFFSET 0x48
#define GNW_H7B0_TIM2_DCR_RESET  0x00000000
#define GNW_H7B0_TIM2_DCR_WMASK  0x00001f1f

#define GNW_H7B0_TIM2_DMAR_OFFSET 0x4c
#define GNW_H7B0_TIM2_DMAR_RESET  0x00000000
#define GNW_H7B0_TIM2_DMAR_WMASK  0x0000ffff

#define GNW_H7B0_TIM2_AF1_OFFSET 0x60
#define GNW_H7B0_TIM2_AF1_RESET  0x00000000
#define GNW_H7B0_TIM2_AF1_WMASK  0x0003c000

#define GNW_H7B0_TIM2_TISEL_OFFSET 0x68
#define GNW_H7B0_TIM2_TISEL_RESET  0x00000000
#define GNW_H7B0_TIM2_TISEL_WMASK  0x0f0f0f0f

static inline uint32_t get_tim2_write_mask(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_TIM2_CR1_OFFSET:
            return 0x00000bff;
        case GNW_H7B0_TIM2_CR2_OFFSET:
            return 0x000000f8;
        case GNW_H7B0_TIM2_SMCR_OFFSET:
            return 0x0031fff7;
        case GNW_H7B0_TIM2_DIER_OFFSET:
            return 0x00005f5f;
        case GNW_H7B0_TIM2_SR_OFFSET:
            return 0x00001e5f;
        case GNW_H7B0_TIM2_EGR_OFFSET:
            return 0x0000005f;
        case GNW_H7B0_TIM2_CCMR1_Input_OFFSET:
            return 0x0101ffff;
        case GNW_H7B0_TIM2_CCMR2_Input_OFFSET:
            return 0x0101ffff;
        case GNW_H7B0_TIM2_CCER_OFFSET:
            return 0x0000bbbb;
        case GNW_H7B0_TIM2_CNT_OFFSET:
            return 0xffffffff;
        case GNW_H7B0_TIM2_PSC_OFFSET:
            return 0x0000ffff;
        case GNW_H7B0_TIM2_ARR_OFFSET:
            return 0xffffffff;
        case GNW_H7B0_TIM2_CCR1_OFFSET:
            return 0xffffffff;
        case GNW_H7B0_TIM2_CCR2_OFFSET:
            return 0xffffffff;
        case GNW_H7B0_TIM2_CCR3_OFFSET:
            return 0xffffffff;
        case GNW_H7B0_TIM2_CCR4_OFFSET:
            return 0xffffffff;
        case GNW_H7B0_TIM2_DCR_OFFSET:
            return 0x00001f1f;
        case GNW_H7B0_TIM2_DMAR_OFFSET:
            return 0x0000ffff;
        case GNW_H7B0_TIM2_AF1_OFFSET:
            return 0x0003c000;
        case GNW_H7B0_TIM2_TISEL_OFFSET:
            return 0x0f0f0f0f;
        default:
            return 0x00000000; /* Read-only or unmapped by default */
    }
}

static inline uint32_t get_tim2_reset_value(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_TIM2_CR1_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM2_CR2_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM2_SMCR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM2_DIER_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM2_SR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM2_EGR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM2_CCMR1_Input_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM2_CCMR2_Input_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM2_CCER_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM2_CNT_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM2_PSC_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM2_ARR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM2_CCR1_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM2_CCR2_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM2_CCR3_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM2_CCR4_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM2_DCR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM2_DMAR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM2_AF1_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM2_TISEL_OFFSET:
            return 0x00000000;
        default:
            return 0x00000000;
    }
}

#endif /* GNW_H7B0_REGS_TIM2_H */
