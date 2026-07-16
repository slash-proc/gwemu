/*
 * QEMU model of the STM32H7B0 MDMA (master DMA) controller.
 *
 * Real (if synchronous) SW-triggered memory-to-memory channel transfers --
 * enough to satisfy game-and-watch-retro-go-sd/diag firmware that uses
 * HAL_MDMA_Start()+HAL_MDMA_PollForTransfer() with MDMA_REQUEST_SW. Register-
 * request-triggered / linked-list transfers are not modeled (no known
 * firmware here uses them).
 */
#ifndef HW_MISC_GNW_H7B0_MDMA_H
#define HW_MISC_GNW_H7B0_MDMA_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_MDMA "gnw-h7b0-mdma"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0MdmaState, GNW_H7B0_MDMA)

#define GNW_H7B0_MDMA_SIZE 0x1000

struct GnwH7B0MdmaState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_MDMA_SIZE / 4];
};

#endif
