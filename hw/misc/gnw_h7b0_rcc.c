/*
 * STM32H7B0 RCC minimal stub (Nintendo Game & Watch)
 *
 * See gnw_h7b0_rcc.h for scope/rationale. Deliberately not the existing
 * hw/misc/stm32_rcc.c device: that model is F4-family register layout
 * (different offsets) and, more importantly, doesn't mirror *ON bits
 * into *RDY bits at all -- reused as-is, real H7B0 firmware's clock-init
 * polling loops would hang forever waiting for a status bit nothing ever
 * sets.
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
#include "migration/vmstate.h"
#include "hw/misc/gnw_h7b0_rcc.h"

static void gnw_h7b0_rcc_reset(DeviceState *dev)
{
    GnwH7B0RccState *s = GNW_H7B0_RCC(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static uint64_t gnw_h7b0_rcc_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0RccState *s = GNW_H7B0_RCC(opaque);

    if (addr >= GNW_H7B0_RCC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_rcc_write(void *opaque, hwaddr addr,
                                uint64_t val64, unsigned int size)
{
    GnwH7B0RccState *s = GNW_H7B0_RCC(opaque);
    uint32_t value = val64;

    if (addr >= GNW_H7B0_RCC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    switch (addr) {
    case GNW_H7B0_RCC_CR:
        /*
         * Mirror each *ON bit into its *RDY bit instantly (not
         * cycle-accurate -- real hardware takes some cycles to lock).
         * Good enough to unblock a polling loop, which is this stub's
         * only job.
         */
        if (value & RCC_CR_HSION) {
            value |= RCC_CR_HSIRDY;
        } else {
            value &= ~RCC_CR_HSIRDY;
        }
        if (value & RCC_CR_HSEON) {
            value |= RCC_CR_HSERDY;
        } else {
            value &= ~RCC_CR_HSERDY;
        }
        if (value & RCC_CR_PLL1ON) {
            value |= RCC_CR_PLL1RDY;
        } else {
            value &= ~RCC_CR_PLL1RDY;
        }
        if (value & RCC_CR_PLL2ON) {
            value |= RCC_CR_PLL2RDY;
        } else {
            value &= ~RCC_CR_PLL2RDY;
        }
        if (value & RCC_CR_PLL3ON) {
            value |= RCC_CR_PLL3RDY;
        } else {
            value &= ~RCC_CR_PLL3RDY;
        }
        s->regs[addr >> 2] = value;
        return;
    case GNW_H7B0_RCC_CFGR:
        /* Mirror SW (requested source) straight into SWS (switch status). */
        value = (value & ~RCC_CFGR_SWS_MASK) |
                (((value & RCC_CFGR_SW_MASK) >> RCC_CFGR_SW_SHIFT)
                 << RCC_CFGR_SWS_SHIFT);
        s->regs[addr >> 2] = value;
        return;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: offset 0x%"HWADDR_PRIx" is a plain read/write "
                      "shadow, no real clock behavior modeled\n",
                      __func__, addr);
        s->regs[addr >> 2] = value;
    }
}

static const MemoryRegionOps gnw_h7b0_rcc_ops = {
    .read = gnw_h7b0_rcc_read,
    .write = gnw_h7b0_rcc_write,
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

static void gnw_h7b0_rcc_init(Object *obj)
{
    GnwH7B0RccState *s = GNW_H7B0_RCC(obj);

    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_rcc_ops, s,
                           TYPE_GNW_H7B0_RCC, GNW_H7B0_RCC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const VMStateDescription vmstate_gnw_h7b0_rcc = {
    .name = TYPE_GNW_H7B0_RCC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0RccState, GNW_H7B0_RCC_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_rcc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_rcc;
    device_class_set_legacy_reset(dc, gnw_h7b0_rcc_reset);
}

static const TypeInfo gnw_h7b0_rcc_info = {
    .name          = TYPE_GNW_H7B0_RCC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0RccState),
    .instance_init = gnw_h7b0_rcc_init,
    .class_init    = gnw_h7b0_rcc_class_init,
};

static void gnw_h7b0_rcc_register_types(void)
{
    type_register_static(&gnw_h7b0_rcc_info);
}

type_init(gnw_h7b0_rcc_register_types)
