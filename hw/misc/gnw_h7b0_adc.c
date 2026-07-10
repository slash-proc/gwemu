/*
 * STM32H7B0 ADC1/ADC2 minimal stub (Nintendo Game & Watch)
 *
 * See gnw_h7b0_adc.h for scope/rationale.
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
#include "hw/misc/gnw_h7b0_adc.h"

static void gnw_h7b0_adc_reset(DeviceState *dev)
{
    GnwH7B0AdcState *s = GNW_H7B0_ADC(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static uint64_t gnw_h7b0_adc_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0AdcState *s = GNW_H7B0_ADC(opaque);

    if (addr >= GNW_H7B0_ADC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_adc_write(void *opaque, hwaddr addr,
                                uint64_t val64, unsigned int size)
{
    GnwH7B0AdcState *s = GNW_H7B0_ADC(opaque);
    uint32_t value = val64;
    hwaddr instance_base;

    if (addr >= GNW_H7B0_ADC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    instance_base = addr - (addr % GNW_H7B0_ADC_INSTANCE_STRIDE);
    if ((addr % GNW_H7B0_ADC_INSTANCE_STRIDE) == GNW_H7B0_ADC_CR &&
        (instance_base / GNW_H7B0_ADC_INSTANCE_STRIDE) <
        GNW_H7B0_ADC_INSTANCE_COUNT) {
        /*
         * Mirror ADEN into ADRDY instantly (not cycle-accurate -- real
         * hardware takes a stabilization delay). Good enough to unblock
         * the polling loop that always follows ADC enable.
         */
        s->regs[addr >> 2] = value;
        if (value & ADC_CR_ADEN) {
            s->regs[(instance_base + GNW_H7B0_ADC_ISR) >> 2] |= ADC_ISR_ADRDY;
        } else {
            s->regs[(instance_base + GNW_H7B0_ADC_ISR) >> 2] &= ~ADC_ISR_ADRDY;
        }
        return;
    }

    qemu_log_mask(LOG_UNIMP,
                  "%s: offset 0x%"HWADDR_PRIx" is a plain read/write "
                  "shadow, no real conversion modeled (reads always 0)\n",
                  __func__, addr);
    s->regs[addr >> 2] = value;
}

static const MemoryRegionOps gnw_h7b0_adc_ops = {
    .read = gnw_h7b0_adc_read,
    .write = gnw_h7b0_adc_write,
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

static void gnw_h7b0_adc_init(Object *obj)
{
    GnwH7B0AdcState *s = GNW_H7B0_ADC(obj);

    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_adc_ops, s,
                           TYPE_GNW_H7B0_ADC, GNW_H7B0_ADC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const VMStateDescription vmstate_gnw_h7b0_adc = {
    .name = TYPE_GNW_H7B0_ADC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0AdcState, GNW_H7B0_ADC_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_adc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_adc;
    device_class_set_legacy_reset(dc, gnw_h7b0_adc_reset);
}

static const TypeInfo gnw_h7b0_adc_info = {
    .name          = TYPE_GNW_H7B0_ADC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0AdcState),
    .instance_init = gnw_h7b0_adc_init,
    .class_init    = gnw_h7b0_adc_class_init,
};

static void gnw_h7b0_adc_register_types(void)
{
    type_register_static(&gnw_h7b0_adc_info);
}

type_init(gnw_h7b0_adc_register_types)
