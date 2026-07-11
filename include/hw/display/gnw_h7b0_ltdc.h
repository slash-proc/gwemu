/*
 * STM32H7B0 LTDC minimal stub (Nintendo Game & Watch)
 *
 * Real device, not a full LTDC model: composites Layer1 only (Layer2,
 * blending, color-keying, CLUT are not modeled -- real G&W firmware
 * uses a single RGB565 layer at the LCD's native 320x240, per
 * gnw-chainloader's src/chainloader/gui.c), and only the RGB565 and L8
 * (CLUT-indexed, used by retro-go's RAM-saving framebuffer mode) pixel
 * formats are drawn (other LxPFCR values log unimplemented and draw
 * nothing). QEMU's own display-refresh timer drives redraws, not a
 * modeled per-pixel/per-line VSYNC scan.
 *
 * Line interrupt IS modeled, though, via a plain ~60Hz QEMU timer (not
 * derived from any real pixel-clock/AWCR/TWCR timing): both
 * gnw-chainloader's gui_refresh() and retro-go-sd's
 * lcd_wait_for_vblank() (Core/Src/gw_lcd.c) __WFI()-busy-wait on a
 * frame counter that only HAL_LTDC_LineEventCallback() (the real LI
 * IRQ handler) increments -- which never fired without a real
 * interrupt. Found once gnw_h7b0_gpio.c's button fix got
 * gnw-chainloader far enough to reach its real menu (the flicker
 * reported there was a symptom of this same gap: gui_refresh()
 * grabbing an in-progress-redraw frame because it never actually
 * waited on anything) and once retro-go-sd's boot got far enough
 * (WWDG/TIM2-block fixes) to hang in lcd_wait_for_vblank() outright.
 * IER.LIE/RRIE gate the interrupt like real hardware; ISR/ICR are real
 * (not shadow) with W1C-on-ICR semantics. LIPCR is still a plain
 * shadow -- the timer fires unconditionally at a fixed rate once
 * enabled, not at the configured line position.
 *
 * SRCR's VBR (vertical-blank reload) now really defers the
 * CFBAR/CFBLR/CFBLNR/PFCR shadow->active copy to the next vblank tick
 * (see gnw_h7b0_ltdc_reload_active()/_vblank_tick()) instead of being
 * instant like IMR. That was the root cause of the menu-tearing/
 * flicker documented above: firmware's double-buffer flip (a VBR
 * reload) used to take effect the instant SRCR was written, so a host
 * redraw could catch a CFBAR pointing at a buffer firmware hadn't
 * actually finished drawing into yet.
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
#include "hw/irq.h"
#include "exec/memory.h"
#include "qemu/timer.h"
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

#define GNW_H7B0_LTDC_TWCR      0x14

/* Layer1 registers, offsets relative to the LTDC base (0x84 + sub-offset). */
#define GNW_H7B0_LTDC_L1CR      0x84
#define LTDC_LxCR_LEN           (1U << 0)
#define GNW_H7B0_LTDC_L1WHPCR   0x88
#define GNW_H7B0_LTDC_L1WVPCR   0x8C
#define GNW_H7B0_LTDC_L1PFCR    0x94
#define LTDC_LxPFCR_PF_MASK     0x7U
#define LTDC_PF_RGB565          2U
#define LTDC_PF_L8              5U
#define GNW_H7B0_LTDC_L1CLUTWR  0xC4
#define GNW_H7B0_LTDC_L1CFBAR   0xAC
#define GNW_H7B0_LTDC_L1CFBLR   0xB0
#define LTDC_LxCFBLR_CFBLL_MASK 0x1FFFU
#define LTDC_LxCFBLR_CFBP_SHIFT 16
#define LTDC_LxCFBLR_CFBP_MASK  0x1FFFU
#define GNW_H7B0_LTDC_L1CFBLNR  0xB4

/* Layer2 registers */
#define GNW_H7B0_LTDC_L2CR      0x104
#define GNW_H7B0_LTDC_L2PFCR    0x114
#define GNW_H7B0_LTDC_L2CACR    0x118
#define GNW_H7B0_LTDC_L2CFBAR   0x12C
#define GNW_H7B0_LTDC_L2CFBLR   0x130
#define GNW_H7B0_LTDC_L2CFBLNR  0x134

#define GNW_H7B0_LTDC_IER    0x34
#define LTDC_IER_LIE         (1U << 0)
#define LTDC_IER_FUIE        (1U << 1)
#define LTDC_IER_TERRIE      (1U << 2)
#define LTDC_IER_RRIE        (1U << 3)

#define GNW_H7B0_LTDC_ISR    0x38
#define LTDC_ISR_LIF         (1U << 0)
#define LTDC_ISR_FUIF        (1U << 1)
#define LTDC_ISR_TERRIF      (1U << 2)
#define LTDC_ISR_RRIF        (1U << 3)

#define GNW_H7B0_LTDC_ICR    0x3C
/* ICR bit positions mirror ISR's LIF/FUIF/TERRIF/RRIF exactly. */

#define GNW_H7B0_LTDC_LIPCR  0x40

/* Plain fixed-rate vblank approximation -- see the file-header comment. */
#define GNW_H7B0_LTDC_VBLANK_HZ 60

struct GnwH7B0LtdcState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    QemuConsole *con;
    qemu_irq irq;
    QEMUTimer *vblank_timer;
    QEMUTimer *line_timer;
    int invalidate;
    bool pf_warned;

    uint32_t regs[GNW_H7B0_LTDC_SIZE / 4];

    /*
     * Real "active" copies of the Layer1 and Layer2 registers that actually
     * affect what gets drawn (CFBAR/CFBLR/CFBLNR/PFCR) -- see
     * gnw_h7b0_ltdc_write()'s SRCR handling. s->regs holds what
     * firmware last wrote (the "shadow" set, read back as-is); these
     * only change on an SRCR reload (IMR: immediately; VBR: deferred
     * to the next real vblank tick), same as real hardware, instead
     * of every layer-register write being instantly visible to the
     * next redraw.
     */
    uint32_t active_l1cr;
    uint32_t active_l1cfbar;
    uint32_t active_l1cfblr;
    uint32_t active_l1cfblnr;
    uint32_t active_l1pfcr;
    uint32_t active_l2cr;
    uint32_t active_l2cfbar;
    uint32_t active_l2cfblr;
    uint32_t active_l2cfblnr;
    uint32_t active_l2pfcr;
    uint32_t active_l2cacr;
    bool vbr_reload_pending;

    /*
     * Shadow buffer to decouple QEMU's asynchronous UI refresh from the
     * guest's rendering. The guest often draws overlays (via DMA2D) directly
     * into the active frontbuffer immediately after a VBR flip. If the UI
     * thread samples guest RAM mid-draw, overlays flicker out of existence.
     * We capture the fully finished frame at the very end of its 16ms window
     * (the instant before the next VBLANK) to guarantee a tear-free image.
     * (A progressive/scanline-timed capture was tried here and reverted --
     * see gnw_h7b0_ltdc_vblank_tick()'s comment for why.)
     */
    uint32_t *shadow_buffer;
    int shadow_width;
    int shadow_height;

    /*
     * Layer1's hardware CLUT (L1CLUTWR), used only when active_l1pfcr
     * selects L8 -- retro-go switches Layer1 into L8 at runtime to save
     * framebuffer RAM (see Core/Src/gw_lcd.c's lcd_setup_framebuffers()/
     * HAL_LTDC_ConfigCLUT() in the real firmware). Entries are already
     * converted to this device's host pixel32 format (see
     * gnw_h7b0_ltdc_write()'s L1CLUTWR case), not left as raw RGB888, so
     * capture_rows() can index straight into it like the RGB565 path.
     */
    uint32_t clut[256];

    /*
     * Set whenever gnw_h7b0_ltdc_vblank_tick() captures a fresh frame
     * into shadow_buffer; cleared once gnw_h7b0_ltdc_update_display()
     * actually redraws from it. QEMU's own UI refresh timer
     * (graphic_hw_update -> our gfx_update callback) fires on its own
     * cadence, independent of and often faster than our ~60Hz capture --
     * without this, update_display() was redoing its full per-pixel
     * upscale/blit (profiled at over a third of this process's total
     * CPU time) on every one of those extra calls even when the
     * frontbuffer hadn't changed at all, which is what was actually
     * behind the "one core pegged" / audio-and-video stutter reports:
     * the guest CPU emulation and the host UI-refresh work compete for
     * the same single QEMU main thread. */
    bool content_dirty;
};

#endif
