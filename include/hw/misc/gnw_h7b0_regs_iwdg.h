/* Auto-generated from STM32H7B0.svd for IWDG */
#ifndef GNW_H7B0_REGS_IWDG_H
#define GNW_H7B0_REGS_IWDG_H

#include <stdint.h>

#define GNW_H7B0_IWDG_KR_OFFSET 0x0
#define GNW_H7B0_IWDG_KR_RESET  0x00000000
#define GNW_H7B0_IWDG_KR_WMASK  0x0000ffff

#define GNW_H7B0_IWDG_PR_OFFSET 0x4
#define GNW_H7B0_IWDG_PR_RESET  0x00000000
#define GNW_H7B0_IWDG_PR_WMASK  0x00000007

#define GNW_H7B0_IWDG_RLR_OFFSET 0x8
#define GNW_H7B0_IWDG_RLR_RESET  0x00000fff
#define GNW_H7B0_IWDG_RLR_WMASK  0x00000fff

#define GNW_H7B0_IWDG_SR_OFFSET 0xc
#define GNW_H7B0_IWDG_SR_RESET  0x00000000
#define GNW_H7B0_IWDG_SR_WMASK  0x00000000

#define GNW_H7B0_IWDG_WINR_OFFSET 0x10
#define GNW_H7B0_IWDG_WINR_RESET  0x00000fff
#define GNW_H7B0_IWDG_WINR_WMASK  0x00000fff

static inline uint32_t get_iwdg_write_mask(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_IWDG_KR_OFFSET:
            return 0x0000ffff;
        case GNW_H7B0_IWDG_PR_OFFSET:
            return 0x00000007;
        case GNW_H7B0_IWDG_RLR_OFFSET:
            return 0x00000fff;
        case GNW_H7B0_IWDG_SR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_IWDG_WINR_OFFSET:
            return 0x00000fff;
        default:
            return 0x00000000; /* Read-only or unmapped by default */
    }
}

static inline uint32_t get_iwdg_reset_value(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_IWDG_KR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_IWDG_PR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_IWDG_RLR_OFFSET:
            return 0x00000fff;
        case GNW_H7B0_IWDG_SR_OFFSET:
            return 0x00000000;
        case GNW_H7B0_IWDG_WINR_OFFSET:
            return 0x00000fff;
        default:
            return 0x00000000;
    }
}

#endif /* GNW_H7B0_REGS_IWDG_H */
