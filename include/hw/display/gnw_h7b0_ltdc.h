/*
 * STM32H7B0 LTDC minimal stub (Nintendo Game & Watch)
 *
 * Real device, not a full LTDC model: composites Layer1 only (Layer2,
 * blending, color-keying, CLUT are not modeled -- real G&W firmware
 * uses a single RGB565 layer at the LCD's native 320x240, per
 * gnw-chainloader's src/chainloader/gui.c), and only the RGB565 pixel
 * format is drawn (other LxPFCR values log unimplemented and draw
 * nothing). No timing/IRQ modeling (IER/ISR/ICR/LIPCR are plain
 * shadow registers, no real interrupt is ever raised) -- QEMU's own
 * display-refresh timer drives redraws, not a modeled VSYNC.
 *
 * Purpose: get pixels from guest RAM onto a QEMU display surface via
 * the same MemoryRegionSection + framebuffer_update_display() pattern
 * used by hw/display/pl110.c (used as a structural template), not a
 * cycle/timing-accurate LCD-TFT controller. See docs/roadmap.md
 * (Phase 2/3 pulled forward -- LTDC was where a real gnw-chainloader
 * boot stopped after Phase 1's other peripheral gaps were closed).
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef HW_DISPLAY_GNW_H7B0_LTDC_H
#define HW_DISPLAY_GNW_H7B0_LTDC_H

#include "hw/sysbus.h"
#include "exec/memory.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_LTDC "gnw-h7b0-ltdc"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0LtdcState, GNW_H7B0_LTDC)

/*
 * Register offsets/bits below are from
 * sdk/cmsis-device-h7/Include/stm32h7b0xx.h's LTDC_TypeDef /
 * LTDC_Layer_TypeDef and LTDC_xxx_yyy bit definitions, cross-checked
 * against STM32H7B0.svd.
 */
#define GNW_H7B0_LTDC_SIZE   0x1000

#define GNW_H7B0_LTDC_GCR    0x18
#define LTDC_GCR_LTDCEN      (1U << 0)

#define GNW_H7B0_LTDC_SRCR   0x24
#define LTDC_SRCR_IMR        (1U << 0)
#define LTDC_SRCR_VBR        (1U << 1)

/* Layer1 registers, offsets relative to the LTDC base (0x84 + sub-offset). */
#define GNW_H7B0_LTDC_L1CR      0x84
#define LTDC_LxCR_LEN           (1U << 0)
#define GNW_H7B0_LTDC_L1WHPCR   0x88
#define GNW_H7B0_LTDC_L1WVPCR   0x8C
#define GNW_H7B0_LTDC_L1PFCR    0x94
#define LTDC_LxPFCR_PF_MASK     0x7U
#define LTDC_PF_RGB565          2U
#define GNW_H7B0_LTDC_L1CFBAR   0xAC
#define GNW_H7B0_LTDC_L1CFBLR   0xB0
#define LTDC_LxCFBLR_CFBLL_MASK 0x1FFFU
#define LTDC_LxCFBLR_CFBP_SHIFT 16
#define LTDC_LxCFBLR_CFBP_MASK  0x1FFFU
#define GNW_H7B0_LTDC_L1CFBLNR  0xB4

struct GnwH7B0LtdcState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    QemuConsole *con;
    int invalidate;
    bool pf_warned;

    uint32_t regs[GNW_H7B0_LTDC_SIZE / 4];
};

#endif
