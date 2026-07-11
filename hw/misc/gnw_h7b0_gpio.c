/*
 * STM32H7B0 GPIO minimal stub (Nintendo Game & Watch)
 *
 * See gnw_h7b0_gpio.h for scope/rationale.
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

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "ui/console.h"
#include "ui/input.h"
#include "hw/misc/gnw_h7b0_gpio.h"
#include "hw/misc/gnw_h7b0_regs_gpio.h"

/*
 * Button -> port/pin table, transcribed from gnw-chainloader's own
 * board.h pin definitions (ground truth for what real firmware wires
 * up), not re-derived from the SVD. Port letters map to the alphabetical
 * port index baked into GNW_H7B0_GPIO_SIZE's layout (A=0, B=1, C=2, D=3).
 */
enum {
    GNW_BTN_PAUSE,
    GNW_BTN_GAME,
    GNW_BTN_TIME,
    GNW_BTN_A,
    GNW_BTN_B,
    GNW_BTN_LEFT,
    GNW_BTN_DOWN,
    GNW_BTN_RIGHT,
    GNW_BTN_UP,
    GNW_BTN_PWR,
    GNW_BTN_START,
    GNW_BTN_SELECT,
    GNW_BTN__COUNT,
};

typedef struct GnwButtonPin {
    int port;
    uint32_t pin;
} GnwButtonPin;

static const GnwButtonPin gnw_h7b0_button_pins[GNW_BTN__COUNT] = {
    [GNW_BTN_PAUSE]  = { 2, 1U << 13 },
    [GNW_BTN_GAME]   = { 2, 1U << 1  },
    [GNW_BTN_TIME]   = { 2, 1U << 5  },
    [GNW_BTN_A]      = { 3, 1U << 9  },
    [GNW_BTN_B]      = { 3, 1U << 5  },
    [GNW_BTN_LEFT]   = { 3, 1U << 11 },
    [GNW_BTN_DOWN]   = { 3, 1U << 14 },
    [GNW_BTN_RIGHT]  = { 3, 1U << 15 },
    [GNW_BTN_UP]     = { 3, 1U << 0  },
    [GNW_BTN_PWR]    = { 0, 1U << 0  },
    [GNW_BTN_START]  = { 2, 1U << 11 },
    [GNW_BTN_SELECT] = { 2, 1U << 12 },
};

/*
 * Keyboard mapping. Arrow keys for the d-pad are the obvious choice;
 * A/B follow the Z/X convention most emulators default to (Z=B, X=A,
 * left-to-right mirroring the physical button layout on real
 * controllers). Enter/right-shift for Start/Select mirror the same
 * emulator convention. GAME/TIME/PWR get first-letter mnemonics since
 * they're G&W-specific and have no existing convention to borrow;
 * Escape for Pause reads naturally as "pause/menu" on any keyboard.
 */
static const int gnw_h7b0_key_map[GNW_BTN__COUNT] = {
    [GNW_BTN_PAUSE]  = Q_KEY_CODE_ESC,
    [GNW_BTN_GAME]   = Q_KEY_CODE_G,
    [GNW_BTN_TIME]   = Q_KEY_CODE_T,
    [GNW_BTN_A]      = Q_KEY_CODE_X,
    [GNW_BTN_B]      = Q_KEY_CODE_Z,
    [GNW_BTN_LEFT]   = Q_KEY_CODE_LEFT,
    [GNW_BTN_DOWN]   = Q_KEY_CODE_DOWN,
    [GNW_BTN_RIGHT]  = Q_KEY_CODE_RIGHT,
    [GNW_BTN_UP]     = Q_KEY_CODE_UP,
    [GNW_BTN_PWR]    = Q_KEY_CODE_P,
    [GNW_BTN_START]  = Q_KEY_CODE_RET,
    [GNW_BTN_SELECT] = Q_KEY_CODE_SHIFT_R,
};

/*
 * Pointer-button mapping. This project's pinned QEMU base (v9.2, see
 * CLAUDE.md) has no SDL2 game-controller/joystick backend -- InputButton
 * in this tree (qapi/ui.json) is a mouse-button enum (left, middle,
 * right, wheel directions, side, extra, touch), not a real gamepad
 * face-button enum. There
 * is no in-tree code path today by which an xpad-style controller's
 * face buttons reach a QEMU device, so this is a best-effort mapping of
 * the mouse-shaped events QEMU's input core can actually deliver, kept
 * here (rather than skipped) so the INPUT_EVENT_MASK_BTN path is wired
 * correctly for whenever this base gets a real joystick backend. left/
 * right double as the two main face buttons (A/B), middle/side/extra as
 * Start/Select/Pause, and the wheel directions as the d-pad.
 */
static const int gnw_h7b0_btn_map[INPUT_BUTTON__MAX] = {
    [0 ... INPUT_BUTTON__MAX - 1] = -1,
    [INPUT_BUTTON_LEFT]        = GNW_BTN_A,
    [INPUT_BUTTON_RIGHT]       = GNW_BTN_B,
    [INPUT_BUTTON_MIDDLE]      = GNW_BTN_START,
    [INPUT_BUTTON_SIDE]        = GNW_BTN_SELECT,
    [INPUT_BUTTON_EXTRA]       = GNW_BTN_PAUSE,
    [INPUT_BUTTON_WHEEL_UP]    = GNW_BTN_UP,
    [INPUT_BUTTON_WHEEL_DOWN]  = GNW_BTN_DOWN,
    [INPUT_BUTTON_WHEEL_LEFT]  = GNW_BTN_LEFT,
    [INPUT_BUTTON_WHEEL_RIGHT] = GNW_BTN_RIGHT,
};

static void gnw_h7b0_gpio_set_button(GnwH7B0GpioState *s, int button,
                                      bool pressed)
{
    const GnwButtonPin *bp = &gnw_h7b0_button_pins[button];
    hwaddr idr = bp->port * GNW_H7B0_GPIO_PORT_SIZE + GNW_H7B0_GPIO_IDR_OFFSET;

    /* Active-low: pressed clears the bit, released sets it back. */
    if (pressed) {
        s->regs[idr >> 2] &= ~bp->pin;
    } else {
        s->regs[idr >> 2] |= bp->pin;
    }
}

static void gnw_h7b0_gpio_input_event(DeviceState *dev, QemuConsole *src,
                                       InputEvent *evt)
{
    GnwH7B0GpioState *s = GNW_H7B0_GPIO(dev);

    switch (evt->type) {
    case INPUT_EVENT_KIND_KEY: {
        InputKeyEvent *key = evt->u.key.data;
        int qcode = qemu_input_key_value_to_qcode(key->key);

        for (int i = 0; i < GNW_BTN__COUNT; i++) {
            if (gnw_h7b0_key_map[i] == qcode) {
                fprintf(stderr, "[gpio-debug] t=%"PRId64" btn=%d down=%d\n",
                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), i, key->down);
                gnw_h7b0_gpio_set_button(s, i, key->down);
                break;
            }
        }
        break;
    }
    case INPUT_EVENT_KIND_BTN: {
        InputBtnEvent *btn = evt->u.btn.data;

        if (btn->button < INPUT_BUTTON__MAX &&
            gnw_h7b0_btn_map[btn->button] != -1) {
            gnw_h7b0_gpio_set_button(s, gnw_h7b0_btn_map[btn->button],
                                      btn->down);
        }
        break;
    }
    default:
        break;
    }
}

static const QemuInputHandler gnw_h7b0_gpio_input_handler = {
    .name = "gnw-h7b0 buttons",
    .mask = INPUT_EVENT_MASK_KEY | INPUT_EVENT_MASK_BTN,
    .event = gnw_h7b0_gpio_input_event,
};

static void gnw_h7b0_gpio_reset(DeviceState *dev)
{
    GnwH7B0GpioState *s = GNW_H7B0_GPIO(dev);

    for (int i = 0; i < (GNW_H7B0_GPIO_SIZE / 4); i++) {
        uint32_t port_offset = (i * 4) % GNW_H7B0_GPIO_PORT_SIZE;
        s->regs[i] = get_gpio_reset_value(port_offset);
    }
    for (int port = 0; port < GNW_H7B0_GPIO_NUM_PORTS; port++) {
        hwaddr idr = port * GNW_H7B0_GPIO_PORT_SIZE + GNW_H7B0_GPIO_IDR_OFFSET;
        s->regs[idr >> 2] = 0xFFFFU;
    }
}

static uint64_t gnw_h7b0_gpio_read(void *opaque, hwaddr addr,
                                    unsigned int size)
{
    GnwH7B0GpioState *s = GNW_H7B0_GPIO(opaque);

    if (addr >= GNW_H7B0_GPIO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_gpio_write(void *opaque, hwaddr addr,
                                 uint64_t val64, unsigned int size)
{
    GnwH7B0GpioState *s = GNW_H7B0_GPIO(opaque);

    if (addr >= GNW_H7B0_GPIO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }
    
    uint32_t port_offset = addr % GNW_H7B0_GPIO_PORT_SIZE;
    uint32_t mask = get_gpio_write_mask(port_offset);
    s->regs[addr >> 2] = (s->regs[addr >> 2] & ~mask) | (val64 & mask);
}

static const MemoryRegionOps gnw_h7b0_gpio_ops = {
    .read = gnw_h7b0_gpio_read,
    .write = gnw_h7b0_gpio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void gnw_h7b0_gpio_init(Object *obj)
{
    GnwH7B0GpioState *s = GNW_H7B0_GPIO(obj);

    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_gpio_ops, s,
                           TYPE_GNW_H7B0_GPIO, GNW_H7B0_GPIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void gnw_h7b0_gpio_realize(DeviceState *dev, Error **errp)
{
    qemu_input_handler_register(dev, &gnw_h7b0_gpio_input_handler);
}

static const VMStateDescription vmstate_gnw_h7b0_gpio = {
    .name = TYPE_GNW_H7B0_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0GpioState, GNW_H7B0_GPIO_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_gpio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_gpio;
    dc->realize = gnw_h7b0_gpio_realize;
    device_class_set_legacy_reset(dc, gnw_h7b0_gpio_reset);
}

static const TypeInfo gnw_h7b0_gpio_info = {
    .name          = TYPE_GNW_H7B0_GPIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0GpioState),
    .instance_init = gnw_h7b0_gpio_init,
    .class_init    = gnw_h7b0_gpio_class_init,
};

static void gnw_h7b0_gpio_register_types(void)
{
    type_register_static(&gnw_h7b0_gpio_info);
}

type_init(gnw_h7b0_gpio_register_types)
