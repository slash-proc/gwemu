/*
 * STM32H7B0 USART1 -- minimal transmit-only model.
 *
 * Only exists because homebrew firmware (the retro-go porting toolkit's test
 * firmware, used by the gnw-doom port) uses USART1 as its printf console and
 * spins forever in `while (!(USART1->ISR & TXE));` against a log-only
 * unimplemented device. See docs/peripheral-coverage.md.
 */
#ifndef HW_MISC_GNW_H7B0_USART1_H
#define HW_MISC_GNW_H7B0_USART1_H

#include "hw/core/sysbus.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_USART1 "gnw-h7b0-usart1"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0Usart1State, GNW_H7B0_USART1)

#define GNW_H7B0_USART1_SIZE 0x400

struct GnwH7B0Usart1State {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    CharFrontend chr;
    uint32_t regs[GNW_H7B0_USART1_SIZE / 4];
};

#endif
