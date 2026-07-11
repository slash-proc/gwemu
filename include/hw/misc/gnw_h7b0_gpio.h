/*
 * STM32H7B0 GPIO minimal stub (Nintendo Game & Watch)
 *
 * Not a real pin/electrical model -- no MODER-driven input/output
 * behavior, no ODR->IDR loopback for pins configured as outputs, no
 * button/LCD control-line side effects. Every register in every port
 * (A-K, 0x400 each) is a plain read/write shadow, *except* that IDR
 * (offset 0x10 within each port) resets to all-1s instead of the
 * generic RAM placeholder's all-0s.
 *
 * That one bit of behavior matters a lot: real G&W buttons are
 * active-low with external pull-ups (see gnw-chainloader's
 * board_check_button(): `(port->IDR & pin) == 0` means pressed), so a
 * zero-initialized IDR reads as "every button held" forever. This was
 * the previous plain-RAM GPIO placeholder's behavior, and it silently
 * forced gnw-chainloader's main.c app_early_logic() "God Mode" path
 * (hold_left || hold_right) on every single boot -- not a real
 * button press, not a QEMU input event, just an unpressed button
 * reading as pressed. Found by the user noticing gnw-chainloader
 * jumping straight to a "FLASHING..." OFW-install screen it had no
 * business reaching un-prompted, after gnw_h7b0_crc.c's real CRC unit
 * let that code path's checksum actually pass instead of failing
 * closed. Defaulting IDR high fixes the false-positive without
 * needing any real button-input plumbing yet.
 *
 * Button input plumbing (added this session): registers a
 * QemuInputHandler covering both keyboard and pointer-button events
 * and clears/sets the mapped port's IDR bit on press/release, per
 * board_check_button()'s `(port->IDR & pin) == 0` == pressed polarity.
 * See gnw_h7b0_gpio.c for the button->pin table and the keyboard/
 * pointer-button mapping, and its rationale comment for why "gamepad"
 * support here is best-effort: QEMU v9.2 (this project's pinned base)
 * has no SDL2 game-controller/joystick backend, so InputButton in this
 * tree is a mouse-button enum only -- there is no code path by which a
 * real xpad-style controller's face buttons reach a QEMU device today.
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

#ifndef HW_MISC_GNW_H7B0_GPIO_H
#define HW_MISC_GNW_H7B0_GPIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_GPIO "gnw-h7b0-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0GpioState, GNW_H7B0_GPIO)

/* Per-port layout, per STM32H7B0.svd's GPIOx peripherals (11 ports, A-K). */
#define GNW_H7B0_GPIO_PORT_SIZE   0x400
#define GNW_H7B0_GPIO_NUM_PORTS   11
#define GNW_H7B0_GPIO_SIZE        (GNW_H7B0_GPIO_NUM_PORTS * GNW_H7B0_GPIO_PORT_SIZE)

#define GNW_H7B0_GPIO_IDR_OFFSET  0x10

struct GnwH7B0GpioState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_GPIO_SIZE / 4];
};

#endif
