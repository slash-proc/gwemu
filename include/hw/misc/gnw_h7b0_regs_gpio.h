/* Auto-generated from STM32H7B0.svd for GPIO */
#ifndef GNW_H7B0_REGS_GPIO_H
#define GNW_H7B0_REGS_GPIO_H

#include <stdint.h>

#define GNW_H7B0_GPIO_MODER_OFFSET 0x0
#define GNW_H7B0_GPIO_MODER_RESET  0xabffffff
#define GNW_H7B0_GPIO_MODER_WMASK  0xffffffff

#define GNW_H7B0_GPIO_OTYPER_OFFSET 0x4
#define GNW_H7B0_GPIO_OTYPER_RESET  0x00000000
#define GNW_H7B0_GPIO_OTYPER_WMASK  0x0000ffff

#define GNW_H7B0_GPIO_OSPEEDR_OFFSET 0x8
#define GNW_H7B0_GPIO_OSPEEDR_RESET  0x0c000000
#define GNW_H7B0_GPIO_OSPEEDR_WMASK  0xffffffff

#define GNW_H7B0_GPIO_PUPDR_OFFSET 0xc
#define GNW_H7B0_GPIO_PUPDR_RESET  0x12100000
#define GNW_H7B0_GPIO_PUPDR_WMASK  0xffffffff

#define GNW_H7B0_GPIO_IDR_OFFSET 0x10
#define GNW_H7B0_GPIO_IDR_RESET  0x00000000
#define GNW_H7B0_GPIO_IDR_WMASK  0x00000000

#define GNW_H7B0_GPIO_ODR_OFFSET 0x14
#define GNW_H7B0_GPIO_ODR_RESET  0x00000000
#define GNW_H7B0_GPIO_ODR_WMASK  0x0000ffff

#define GNW_H7B0_GPIO_BSRR_OFFSET 0x18
#define GNW_H7B0_GPIO_BSRR_RESET  0x00000000
#define GNW_H7B0_GPIO_BSRR_WMASK  0xffffffff

#define GNW_H7B0_GPIO_LCKR_OFFSET 0x1c
#define GNW_H7B0_GPIO_LCKR_RESET  0x00000000
#define GNW_H7B0_GPIO_LCKR_WMASK  0x0001ffff

#define GNW_H7B0_GPIO_AFRL_OFFSET 0x20
#define GNW_H7B0_GPIO_AFRL_RESET  0x00000000
#define GNW_H7B0_GPIO_AFRL_WMASK  0xffffffff

#define GNW_H7B0_GPIO_AFRH_OFFSET 0x24
#define GNW_H7B0_GPIO_AFRH_RESET  0x00000000
#define GNW_H7B0_GPIO_AFRH_WMASK  0xffffffff

static inline uint32_t get_gpio_write_mask(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_GPIO_MODER_OFFSET:
            return GNW_H7B0_GPIO_MODER_WMASK;
        case GNW_H7B0_GPIO_OTYPER_OFFSET:
            return GNW_H7B0_GPIO_OTYPER_WMASK;
        case GNW_H7B0_GPIO_OSPEEDR_OFFSET:
            return GNW_H7B0_GPIO_OSPEEDR_WMASK;
        case GNW_H7B0_GPIO_PUPDR_OFFSET:
            return GNW_H7B0_GPIO_PUPDR_WMASK;
        case GNW_H7B0_GPIO_IDR_OFFSET:
            return GNW_H7B0_GPIO_IDR_WMASK;
        case GNW_H7B0_GPIO_ODR_OFFSET:
            return GNW_H7B0_GPIO_ODR_WMASK;
        case GNW_H7B0_GPIO_BSRR_OFFSET:
            return GNW_H7B0_GPIO_BSRR_WMASK;
        case GNW_H7B0_GPIO_LCKR_OFFSET:
            return GNW_H7B0_GPIO_LCKR_WMASK;
        case GNW_H7B0_GPIO_AFRL_OFFSET:
            return GNW_H7B0_GPIO_AFRL_WMASK;
        case GNW_H7B0_GPIO_AFRH_OFFSET:
            return GNW_H7B0_GPIO_AFRH_WMASK;
        default:
            return 0x00000000; /* Read-only or unmapped by default */
    }
}

static inline uint32_t get_gpio_reset_value(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_GPIO_MODER_OFFSET:
            return GNW_H7B0_GPIO_MODER_RESET;
        case GNW_H7B0_GPIO_OTYPER_OFFSET:
            return GNW_H7B0_GPIO_OTYPER_RESET;
        case GNW_H7B0_GPIO_OSPEEDR_OFFSET:
            return GNW_H7B0_GPIO_OSPEEDR_RESET;
        case GNW_H7B0_GPIO_PUPDR_OFFSET:
            return GNW_H7B0_GPIO_PUPDR_RESET;
        case GNW_H7B0_GPIO_IDR_OFFSET:
            return GNW_H7B0_GPIO_IDR_RESET;
        case GNW_H7B0_GPIO_ODR_OFFSET:
            return GNW_H7B0_GPIO_ODR_RESET;
        case GNW_H7B0_GPIO_BSRR_OFFSET:
            return GNW_H7B0_GPIO_BSRR_RESET;
        case GNW_H7B0_GPIO_LCKR_OFFSET:
            return GNW_H7B0_GPIO_LCKR_RESET;
        case GNW_H7B0_GPIO_AFRL_OFFSET:
            return GNW_H7B0_GPIO_AFRL_RESET;
        case GNW_H7B0_GPIO_AFRH_OFFSET:
            return GNW_H7B0_GPIO_AFRH_RESET;
        default:
            return 0x00000000;
    }
}

#endif /* GNW_H7B0_REGS_GPIO_H */
