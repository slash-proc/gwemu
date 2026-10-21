/*
 * STM32H7B0 core and HCLK divider decoding.
 *
 * The encoding-to-shift table is CMSIS D1CorePrescTable from
 * stm32h7xx_hal_rcc.c and system_stm32h7xx.c.
 */
#ifndef HW_MISC_GNW_H7B0_CLOCK_DIVIDERS_H
#define HW_MISC_GNW_H7B0_CLOCK_DIVIDERS_H

#include <stdint.h>

static inline unsigned gnw_h7b0_rcc_prescaler_shift(uint32_t encoding)
{
    static const uint8_t shifts[16] = {
        0, 0, 0, 0, 1, 2, 3, 4,
        1, 2, 3, 4, 6, 7, 8, 9,
    };

    return shifts[encoding & 0xf];
}

static inline uint32_t gnw_h7b0_rcc_core_hz_from_regs(uint32_t sysclk_hz,
                                                      uint32_t cdcfgr1)
{
    return sysclk_hz >>
           gnw_h7b0_rcc_prescaler_shift(cdcfgr1 >> 8);
}

static inline uint32_t gnw_h7b0_rcc_hclk_hz_from_regs(uint32_t sysclk_hz,
                                                      uint32_t cdcfgr1)
{
    return gnw_h7b0_rcc_core_hz_from_regs(sysclk_hz, cdcfgr1) >>
           gnw_h7b0_rcc_prescaler_shift(cdcfgr1);
}

#endif
