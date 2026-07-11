/* Auto-generated from STM32H7B0.svd for TIM1 */
#ifndef GNW_H7B0_REGS_TIM1_H
#define GNW_H7B0_REGS_TIM1_H

#include <stdint.h>

#define GNW_H7B0_TIM1_CR1_OFFSET 0x0
#define GNW_H7B0_TIM1_CR1_RESET  0x00000000
#define GNW_H7B0_TIM1_CR1_WMASK  0x00000bff

#define GNW_H7B0_TIM1_CR2_OFFSET 0x4
#define GNW_H7B0_TIM1_CR2_RESET  0x00000000
#define GNW_H7B0_TIM1_CR2_WMASK  0x00f57ffd

#define GNW_H7B0_TIM1_SMCR_OFFSET 0x8
#define GNW_H7B0_TIM1_SMCR_RESET  0x00000000
#define GNW_H7B0_TIM1_SMCR_WMASK  0x0031fff7

#define GNW_H7B0_TIM1_DIER_OFFSET 0xc
#define GNW_H7B0_TIM1_DIER_RESET  0x00000000
#define GNW_H7B0_TIM1_DIER_WMASK  0x00007fff

#define GNW_H7B0_TIM1_SR_OFFSET 0x10
#define GNW_H7B0_TIM1_SR_RESET  0x00000000
#define GNW_H7B0_TIM1_SR_WMASK  0x00033fff

#define GNW_H7B0_TIM1_EGR_OFFSET 0x14
#define GNW_H7B0_TIM1_EGR_RESET  0x00000000
#define GNW_H7B0_TIM1_EGR_WMASK  0x000001ff

#define GNW_H7B0_TIM1_CCMR1_Output_OFFSET 0x18
#define GNW_H7B0_TIM1_CCMR1_Output_RESET  0x00000000
#define GNW_H7B0_TIM1_CCMR1_Output_WMASK  0x0101ffff

#define GNW_H7B0_TIM1_CCMR1_Input_OFFSET 0x18
#define GNW_H7B0_TIM1_CCMR1_Input_RESET  0x00000000
#define GNW_H7B0_TIM1_CCMR1_Input_WMASK  0x0000ffff

#define GNW_H7B0_TIM1_CCMR2_Output_OFFSET 0x1c
#define GNW_H7B0_TIM1_CCMR2_Output_RESET  0x00000000
#define GNW_H7B0_TIM1_CCMR2_Output_WMASK  0x0101ffff

#define GNW_H7B0_TIM1_CCMR2_Input_OFFSET 0x1c
#define GNW_H7B0_TIM1_CCMR2_Input_RESET  0x00000000
#define GNW_H7B0_TIM1_CCMR2_Input_WMASK  0x0000ffff

#define GNW_H7B0_TIM1_CCER_OFFSET 0x20
#define GNW_H7B0_TIM1_CCER_RESET  0x00000000
#define GNW_H7B0_TIM1_CCER_WMASK  0x0033bfff

#define GNW_H7B0_TIM1_CNT_OFFSET 0x24
#define GNW_H7B0_TIM1_CNT_RESET  0x00000000
#define GNW_H7B0_TIM1_CNT_WMASK  0x0000ffff

#define GNW_H7B0_TIM1_PSC_OFFSET 0x28
#define GNW_H7B0_TIM1_PSC_RESET  0x00000000
#define GNW_H7B0_TIM1_PSC_WMASK  0x0000ffff

#define GNW_H7B0_TIM1_ARR_OFFSET 0x2c
#define GNW_H7B0_TIM1_ARR_RESET  0x00000000
#define GNW_H7B0_TIM1_ARR_WMASK  0x0000ffff

#define GNW_H7B0_TIM1_CCR1_OFFSET 0x34
#define GNW_H7B0_TIM1_CCR1_RESET  0x00000000
#define GNW_H7B0_TIM1_CCR1_WMASK  0x0000ffff

#define GNW_H7B0_TIM1_CCR2_OFFSET 0x38
#define GNW_H7B0_TIM1_CCR2_RESET  0x00000000
#define GNW_H7B0_TIM1_CCR2_WMASK  0x0000ffff

#define GNW_H7B0_TIM1_CCR3_OFFSET 0x3c
#define GNW_H7B0_TIM1_CCR3_RESET  0x00000000
#define GNW_H7B0_TIM1_CCR3_WMASK  0x0000ffff

#define GNW_H7B0_TIM1_CCR4_OFFSET 0x40
#define GNW_H7B0_TIM1_CCR4_RESET  0x00000000
#define GNW_H7B0_TIM1_CCR4_WMASK  0x0000ffff

#define GNW_H7B0_TIM1_DCR_OFFSET 0x48
#define GNW_H7B0_TIM1_DCR_RESET  0x00000000
#define GNW_H7B0_TIM1_DCR_WMASK  0x00001f1f

#define GNW_H7B0_TIM1_DMAR_OFFSET 0x4c
#define GNW_H7B0_TIM1_DMAR_RESET  0x00000000
#define GNW_H7B0_TIM1_DMAR_WMASK  0x0000ffff

#define GNW_H7B0_TIM1_RCR_OFFSET 0x30
#define GNW_H7B0_TIM1_RCR_RESET  0x00000000
#define GNW_H7B0_TIM1_RCR_WMASK  0x000000ff

#define GNW_H7B0_TIM1_BDTR_OFFSET 0x44
#define GNW_H7B0_TIM1_BDTR_RESET  0x00000000
#define GNW_H7B0_TIM1_BDTR_WMASK  0x03ffffff

#define GNW_H7B0_TIM1_CCMR3_Output_OFFSET 0x54
#define GNW_H7B0_TIM1_CCMR3_Output_RESET  0x00000000
#define GNW_H7B0_TIM1_CCMR3_Output_WMASK  0x0101fcfc

#define GNW_H7B0_TIM1_CCR5_OFFSET 0x58
#define GNW_H7B0_TIM1_CCR5_RESET  0x00000000
#define GNW_H7B0_TIM1_CCR5_WMASK  0xe000ffff

#define GNW_H7B0_TIM1_CCR6_OFFSET 0x5c
#define GNW_H7B0_TIM1_CCR6_RESET  0x00000000
#define GNW_H7B0_TIM1_CCR6_WMASK  0x0000ffff

#define GNW_H7B0_TIM1_AF1_OFFSET 0x60
#define GNW_H7B0_TIM1_AF1_RESET  0x00000000
#define GNW_H7B0_TIM1_AF1_WMASK  0x0003cf07

#define GNW_H7B0_TIM1_AF2_OFFSET 0x64
#define GNW_H7B0_TIM1_AF2_RESET  0x00000000
#define GNW_H7B0_TIM1_AF2_WMASK  0x00000f07

#define GNW_H7B0_TIM1_TISEL_OFFSET 0x68
#define GNW_H7B0_TIM1_TISEL_RESET  0x00000000
#define GNW_H7B0_TIM1_TISEL_WMASK  0x0f0f0f0f

static inline uint32_t get_tim1_write_mask(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_TIM1_CR1_OFFSET:
            return 0x00000bff;
        case GNW_H7B0_TIM1_CR2_OFFSET:
            return 0x00f57ffd;
        case GNW_H7B0_TIM1_SMCR_OFFSET:
            return 0x0031fff7;
        case GNW_H7B0_TIM1_DIER_OFFSET:
            return 0x00007fff;
        case GNW_H7B0_TIM1_SR_OFFSET:
            return 0x00033fff;
        case GNW_H7B0_TIM1_EGR_OFFSET:
            return 0x000001ff;
        case GNW_H7B0_TIM1_CCMR1_Input_OFFSET:
            return 0x0101ffff;
        case GNW_H7B0_TIM1_CCMR2_Input_OFFSET:
            return 0x0101ffff;
        case GNW_H7B0_TIM1_CCER_OFFSET:
            return 0x0033bfff;
        case GNW_H7B0_TIM1_CNT_OFFSET:
            return 0x0000ffff;
        case GNW_H7B0_TIM1_PSC_OFFSET:
            return 0x0000ffff;
        case GNW_H7B0_TIM1_ARR_OFFSET:
            return 0x0000ffff;
        case GNW_H7B0_TIM1_CCR1_OFFSET:
            return 0x0000ffff;
        case GNW_H7B0_TIM1_CCR2_OFFSET:
            return 0x0000ffff;
        case GNW_H7B0_TIM1_CCR3_OFFSET:
            return 0x0000ffff;
        case GNW_H7B0_TIM1_CCR4_OFFSET:
            return 0x0000ffff;
        case GNW_H7B0_TIM1_DCR_OFFSET:
            return 0x00001f1f;
        case GNW_H7B0_TIM1_DMAR_OFFSET:
            return 0x0000ffff;
        case GNW_H7B0_TIM1_RCR_OFFSET:
            return 0x000000ff;
        case GNW_H7B0_TIM1_BDTR_OFFSET:
            return 0x03ffffff;
        case GNW_H7B0_TIM1_CCMR3_Output_OFFSET:
            return 0x0101fcfc;
        case GNW_H7B0_TIM1_CCR5_OFFSET:
            return 0xe000ffff;
        case GNW_H7B0_TIM1_CCR6_OFFSET:
            return 0x0000ffff;
        case GNW_H7B0_TIM1_AF1_OFFSET:
            return 0x0003cf07;
        case GNW_H7B0_TIM1_AF2_OFFSET:
            return 0x00000f07;
        case GNW_H7B0_TIM1_TISEL_OFFSET:
            return 0x0f0f0f0f;
        default:
            return 0x00000000; /* Read-only or unmapped by default */
    }
}

static inline uint32_t get_tim1_reset_value(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_TIM1_CR1_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM1_CR2_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM1_SMCR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM1_DIER_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM1_SR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM1_EGR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM1_CCMR1_Input_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM1_CCMR2_Input_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM1_CCER_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM1_CNT_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM1_PSC_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM1_ARR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM1_CCR1_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM1_CCR2_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM1_CCR3_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM1_CCR4_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM1_DCR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM1_DMAR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM1_RCR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM1_BDTR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM1_CCMR3_Output_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM1_CCR5_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM1_CCR6_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM1_AF1_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM1_AF2_OFFSET:
            return 0x00000000;
        case GNW_H7B0_TIM1_TISEL_OFFSET:
            return 0x00000000;
        default:
            return 0x00000000;
    }
}

#endif /* GNW_H7B0_REGS_TIM1_H */
