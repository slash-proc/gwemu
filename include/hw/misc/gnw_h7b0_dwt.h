/*
 * ARMv7-M DWT (Data Watchpoint and Trace) unit -- real CYCCNT only
 * (Nintendo Game & Watch)
 *
 * This is architecturally part of the Cortex-M core (fixed at
 * 0xE0001000 for every Cortex-M implementation), not an STM32H7B0-
 * specific peripheral, but QEMU's stock ARMv7-M container doesn't
 * model it, so nothing backs this address at all -- reads/writes
 * silently no-op against whatever catches unmapped MMIO. Every retro-
 * go core's frame-pacing logic (Core/Src/porting/common.c's
 * common_emu_get_dwt_cycles(), used by frame_integrator to measure
 * real elapsed time per loop iteration) reads DWT_CYCCNT (offset
 * 0x04) expecting it to be a free-running cycle counter. With it
 * permanently stuck at 0, every frame measures as "zero time
 * elapsed", so frame_integrator drifts unboundedly negative and
 * common_emu_state.pause_frames latches at 1 forever -- which in turn
 * makes common_emu_sound_sync() wait for two DMA half-transfer ticks
 * per call instead of one, while the per-core sound_submit() (e.g.
 * gwenesis_sound_submit(), Core/Src/porting/gwenesis/main_gwenesis.c)
 * still only refills whichever ONE buffer half is active when it
 * finally runs -- permanently starving the other half of fresh
 * content. Confirmed live: DWT_CYCCNT read back as 0x0 across 2 full
 * real seconds of active gameplay.
 *
 * Only CYCCNT is modeled as real (a free-running counter derived from
 * QEMU_CLOCK_VIRTUAL, scaled by the SoC's SYSCLK_FRQ so 1 count
 * approximates 1 real CPU cycle at 280MHz, matching what firmware's
 * cycle-budget math assumes) -- CTRL/LAR/other DWT registers are
 * plain read/write shadows; comparators and the other counters
 * (CPICNT/EXCCNT/etc) aren't used by any code path seen so far.
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
#ifndef HW_MISC_GNW_H7B0_DWT_H
#define HW_MISC_GNW_H7B0_DWT_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_DWT "gnw-h7b0-dwt"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0DwtState, GNW_H7B0_DWT)

#define GNW_H7B0_DWT_SIZE 0x1000

#define GNW_H7B0_DWT_CTRL_OFFSET   0x000
#define DWT_CTRL_CYCCNTENA         (1U << 0)
#define GNW_H7B0_DWT_CYCCNT_OFFSET 0x004
#define GNW_H7B0_DWT_LAR_OFFSET    0xFB0

/* Matches SYSCLK_FRQ in hw/arm/gnw_h7b0.c -- real STM32H7B0 max clock. */
#define GNW_H7B0_DWT_CYCLE_HZ 280000000ULL

struct GnwH7B0DwtState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_DWT_SIZE / 4];

    /* CYCCNT = base_count + elapsed-since-enabled scaled to cycles,
     * so a write of 0 to CYCCNT (common_emu_enable_dwt_cycles()'s
     * reset-to-0 pattern) and CTRL.CYCCNTENA toggling both behave
     * like a real free-running counter that can be zeroed and
     * paused/resumed. */
    uint64_t base_count;
    int64_t enabled_since_ns;
    bool enabled;
};

#endif
