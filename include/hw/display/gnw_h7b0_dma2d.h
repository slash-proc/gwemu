/*
 * STM32H7B0 DMA2D (Chrom-ART Accelerator) device model (Nintendo Game & Watch)
 *
 * Real device, not a register-shadow stub (see gnw_h7b0_ltdc.c for this
 * project's established distinction): when CR.START goes high, an actual
 * memory-to-memory pixel transfer runs against guest RAM using
 * cpu_physical_memory_read/write (matching gnw_h7b0_ltdc.c's convention),
 * and ISR.TCIF is set on completion so real HAL polling
 * (HAL_DMA2D_PollForTransfer(), which waits on ISR.TCIF -- see
 * sdk/stm32h7xx-hal-driver/Src/stm32h7xx_hal_dma2d.c) terminates instead of
 * spinning forever, same bug class as gnw_h7b0_spi.c's SR.RXP and
 * gnw_h7b0_jpeg.c's SR.EOCF fixes.
 *
 * Modeled modes: R2M (solid fill), M2M (raw copy, no conversion), M2M_PFC
 * (format-converting copy), M2M_BLEND (alpha blend FG over BG). Pixel
 * format conversion math (RGB565/ARGB8888/ARGB1555/ARGB4444, L8-via-CLUT)
 * is a clean-room reimplementation from understood DMA2D semantics -- see
 * CLAUDE.md's DMA2D roadmap note -- cross-checked against
 * ../minicraft-gnw/tools/retro-go-porting-toolkit/host/qemu/dma2d_emu.h's
 * documented register/mode semantics (that project's own fault-trap-based
 * software model, not third-party code), not copied from any source with
 * unclear license status.
 *
 * Register offsets/bits below are from STM32H7B0.svd's DMA2D peripheral
 * (authoritative per CLAUDE.md), cross-checked against
 * sdk/stm32h7xx-hal-driver/Inc/stm32h7xx_hal_dma2d.h's DMA2D_INPUT_x,
 * DMA2D_OUTPUT_x, DMA2D_M2Mx, DMA2D_R2M encodings.
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

#ifndef HW_DISPLAY_GNW_H7B0_DMA2D_H
#define HW_DISPLAY_GNW_H7B0_DMA2D_H

#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "system/memory.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_DMA2D "gnw-h7b0-dma2d"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0Dma2dState, GNW_H7B0_DMA2D)

/* addressBlock size from STM32H7B0.svd; only offsets up through AMTCR
 * (0x4C) are meaningfully implemented, the rest of the window reads back
 * as a plain shadow. */
#define GNW_H7B0_DMA2D_SIZE 0x400

#define GNW_H7B0_DMA2D_CR      0x00
#define DMA2D_CR_START         (1U << 0)
#define DMA2D_CR_SUSP          (1U << 1)
#define DMA2D_CR_ABORT         (1U << 2)
#define DMA2D_CR_TEIE          (1U << 8)
#define DMA2D_CR_TCIE          (1U << 9)
#define DMA2D_CR_MODE_SHIFT    16
/* Real hardware's MODE field in CR is 3 bits wide (confirmed against
 * sdk/stm32h7xx-hal-driver/Src/stm32h7xx_hal_dma2d.h) -- a 2-bit mask here
 * silently truncated real firmware's MODE=5 (M2M_BLEND_BG) down to 1
 * (M2M_PFC), which is why the real cover-art blend path never actually ran
 * in emulation. */
#define DMA2D_CR_MODE_MASK     (0x7U << DMA2D_CR_MODE_SHIFT)
#define DMA2D_MODE_M2M         0U
#define DMA2D_MODE_M2M_PFC     1U
#define DMA2D_MODE_M2M_BLEND   2U
#define DMA2D_MODE_R2M         3U
#define DMA2D_MODE_M2M_BLEND_FG 4U
#define DMA2D_MODE_M2M_BLEND_BG 5U

#define GNW_H7B0_DMA2D_ISR     0x04
#define DMA2D_ISR_TEIF         (1U << 0)
#define DMA2D_ISR_TCIF         (1U << 1)
#define DMA2D_ISR_TWIF         (1U << 2)
#define DMA2D_ISR_CAEIF        (1U << 3)
#define DMA2D_ISR_CTCIF        (1U << 4)
#define DMA2D_ISR_CEIF         (1U << 5)

#define GNW_H7B0_DMA2D_IFCR    0x08
/* IFCR bit positions mirror ISR's exactly (write-1-to-clear). */

#define GNW_H7B0_DMA2D_FGMAR   0x0C
#define GNW_H7B0_DMA2D_FGOR    0x10
#define GNW_H7B0_DMA2D_BGMAR   0x14
#define GNW_H7B0_DMA2D_BGOR    0x18

#define GNW_H7B0_DMA2D_FGPFCCR 0x1C
#define GNW_H7B0_DMA2D_BGPFCCR 0x24
#define DMA2D_PFCCR_CM_MASK    0xFU
#define DMA2D_PFCCR_START      (1U << 5)
#define DMA2D_PFCCR_AM_SHIFT   16
#define DMA2D_PFCCR_AM_MASK    (0x3U << DMA2D_PFCCR_AM_SHIFT)
#define DMA2D_PFCCR_ALPHA_SHIFT 24
#define DMA2D_PFCCR_ALPHA_MASK (0xFFU << DMA2D_PFCCR_ALPHA_SHIFT)

#define DMA2D_AM_NO_MODIF      0U
#define DMA2D_AM_REPLACE       1U
#define DMA2D_AM_MULTIPLY      2U

#define DMA2D_INPUT_ARGB8888   0U
#define DMA2D_INPUT_RGB888     1U
#define DMA2D_INPUT_RGB565     2U
#define DMA2D_INPUT_ARGB1555   3U
#define DMA2D_INPUT_ARGB4444   4U
#define DMA2D_INPUT_L8         5U
#define DMA2D_INPUT_AL44       6U
#define DMA2D_INPUT_AL88       7U
#define DMA2D_INPUT_L4         8U
#define DMA2D_INPUT_A8         9U
#define DMA2D_INPUT_A4         10U

#define GNW_H7B0_DMA2D_FGCOLR  0x20
#define GNW_H7B0_DMA2D_BGCOLR  0x28
#define GNW_H7B0_DMA2D_FGCMAR  0x2C
#define GNW_H7B0_DMA2D_BGCMAR  0x30

#define GNW_H7B0_DMA2D_OPFCCR  0x34
#define DMA2D_OPFCCR_CM_MASK   0x7U
#define DMA2D_OUTPUT_ARGB8888  0U
#define DMA2D_OUTPUT_RGB888    1U
#define DMA2D_OUTPUT_RGB565    2U
#define DMA2D_OUTPUT_ARGB1555  3U
#define DMA2D_OUTPUT_ARGB4444  4U

#define GNW_H7B0_DMA2D_OCOLR   0x38
#define GNW_H7B0_DMA2D_OMAR    0x3C
#define GNW_H7B0_DMA2D_OOR     0x40

#define GNW_H7B0_DMA2D_NLR     0x44
#define DMA2D_NLR_NL_MASK      0xFFFFU
#define DMA2D_NLR_PL_SHIFT     16
#define DMA2D_NLR_PL_MASK      (0x3FFFU << DMA2D_NLR_PL_SHIFT)

#define GNW_H7B0_DMA2D_LWR     0x48
#define GNW_H7B0_DMA2D_AMTCR   0x4C

typedef struct GnwH7B0Dma2dTimingProfile {
    uint32_t identity;
    bool eligible;
    uint32_t supported_modes;
    uint32_t setup_cycles;
    uint32_t cycles_per_pixel_num;
    uint32_t cycles_per_line_num;
    uint32_t cycles_per_pixel_den;
} GnwH7B0Dma2dTimingProfile;

typedef struct GnwH7B0Dma2dModeTiming {
    uint64_t setup_num;
    uint32_t cycles_per_pixel_num;
    uint32_t cycles_per_pixel_den;
} GnwH7B0Dma2dModeTiming;

struct GnwH7B0Dma2dState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;

    uint32_t regs[GNW_H7B0_DMA2D_SIZE / 4];

    bool timing_mode;
    bool migration_timing_mode;
    bool clock_known;
    bool clock_gate_enabled;
    uint32_t clock_hz;
    GnwH7B0Dma2dTimingProfile profile;
    GnwH7B0Dma2dModeTiming mode_timing[4];
    uint32_t evidence_reference_hz;
    uint32_t evidence_required_clock_hz;
    GnwH7B0Dma2dModeTiming active_timing;
    uint32_t migration_profile_identity;
    uint64_t unsupported_count;

    bool active;
    uint32_t active_mode;
    uint32_t current_line;
    uint32_t total_lines;
    uint32_t pixels_per_line;
    uint32_t source;
    uint32_t destination;
    uint32_t source_stride;
    uint32_t destination_stride;
    uint32_t bytes_per_pixel;
    uint32_t fill_color;
    uint64_t phase_need;
    uint64_t phase_progress;
    uint32_t phase_subcycle;
    int64_t last_update_ns;
    int64_t migration_clock_ns;
    QEMUTimer *timer;

    bool test_profile;
    bool dma_engine_successor_profile;
    char *dma_engine_successor_receipt_sha256;
    bool dma2d_poll_r4_profile;
    char *dma2d_poll_r4_receipt_sha256;
    bool dma2d_poll_r6_profile;
    char *dma2d_poll_r6_scope_sha256;
    uint32_t test_cycles_per_line;
    bool test_clock_known;
    uint32_t test_clock_hz;
};

void gnw_h7b0_dma2d_set_timing_profile(
    GnwH7B0Dma2dState *s, const GnwH7B0Dma2dTimingProfile *profile);
void gnw_h7b0_dma2d_set_clock_info(GnwH7B0Dma2dState *s, bool known,
                                    uint32_t reference_hz,
                                    bool gate_enabled);
uint32_t gnw_h7b0_dma2d_contends_for_range(void *opaque, uint64_t address,
                                           unsigned size);

#endif
