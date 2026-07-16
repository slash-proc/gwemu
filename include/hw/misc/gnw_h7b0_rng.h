/*
 * QEMU model of the STM32H7B0 RNG (true random number generator).
 *
 * Real (if instantaneous/synchronous, not cycle-accurate) behavior: enough
 * to satisfy game-and-watch-retro-go-sd/diag firmware polling
 * RNG_SR.DRDY/RNG->DR via raw CMSIS register access (no vendored HAL RNG
 * driver in this project). See gnw_h7b0_rng.c for the full rationale.
 */
#ifndef HW_MISC_GNW_H7B0_RNG_H
#define HW_MISC_GNW_H7B0_RNG_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_RNG "gnw-h7b0-rng"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0RngState, GNW_H7B0_RNG)

#define GNW_H7B0_RNG_SIZE 0x400

struct GnwH7B0RngState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t cr;
    uint32_t sr;
};

#endif
