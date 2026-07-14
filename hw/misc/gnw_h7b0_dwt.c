/*
 * ARMv7-M DWT unit -- real CYCCNT only (Nintendo Game & Watch)
 *
 * See gnw_h7b0_dwt.h for scope/rationale.
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
#include "migration/vmstate.h"
#include "hw/misc/gnw_h7b0_dwt.h"

static uint64_t gnw_h7b0_dwt_cyccnt(GnwH7B0DwtState *s)
{
    if (!s->enabled) {
        return s->base_count;
    }

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t elapsed_ns = now - s->enabled_since_ns;

    return s->base_count +
           (uint64_t)elapsed_ns * GNW_H7B0_DWT_CYCLE_HZ / NANOSECONDS_PER_SECOND;
}

static void gnw_h7b0_dwt_reset(DeviceState *dev)
{
    GnwH7B0DwtState *s = GNW_H7B0_DWT(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->base_count = 0;
    s->enabled_since_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->enabled = false;
}

static uint64_t gnw_h7b0_dwt_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0DwtState *s = GNW_H7B0_DWT(opaque);

    if (addr >= GNW_H7B0_DWT_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }
    if (addr == GNW_H7B0_DWT_CYCCNT_OFFSET) {
        return (uint32_t)gnw_h7b0_dwt_cyccnt(s);
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_dwt_write(void *opaque, hwaddr addr, uint64_t val64,
                                unsigned int size)
{
    GnwH7B0DwtState *s = GNW_H7B0_DWT(opaque);
    uint32_t value = val64;

    if (addr >= GNW_H7B0_DWT_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    switch (addr) {
    case GNW_H7B0_DWT_CYCCNT_OFFSET:
        /*
         * common_emu_enable_dwt_cycles() writes 0 here to reset the
         * counter at the start of each measured loop; real hardware
         * lets CYCCNT be written to any value and keeps counting from
         * there.
         */
        s->base_count = value;
        s->enabled_since_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        return;
    case GNW_H7B0_DWT_CTRL_OFFSET:
        s->regs[addr >> 2] = value;
        if ((value & DWT_CTRL_CYCCNTENA) && !s->enabled) {
            s->base_count = gnw_h7b0_dwt_cyccnt(s);
            s->enabled_since_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            s->enabled = true;
        } else if (!(value & DWT_CTRL_CYCCNTENA) && s->enabled) {
            s->base_count = gnw_h7b0_dwt_cyccnt(s);
            s->enabled = false;
        }
        return;
    default:
        s->regs[addr >> 2] = value;
        return;
    }
}

static const MemoryRegionOps gnw_h7b0_dwt_ops = {
    .read = gnw_h7b0_dwt_read,
    .write = gnw_h7b0_dwt_write,
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

static void gnw_h7b0_dwt_init(Object *obj)
{
    GnwH7B0DwtState *s = GNW_H7B0_DWT(obj);

    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_dwt_ops, s,
                           TYPE_GNW_H7B0_DWT, GNW_H7B0_DWT_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const VMStateDescription vmstate_gnw_h7b0_dwt = {
    .name = TYPE_GNW_H7B0_DWT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0DwtState, GNW_H7B0_DWT_SIZE / 4),
        VMSTATE_UINT64(base_count, GnwH7B0DwtState),
        VMSTATE_INT64(enabled_since_ns, GnwH7B0DwtState),
        VMSTATE_BOOL(enabled, GnwH7B0DwtState),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_dwt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_dwt;
    device_class_set_legacy_reset(dc, gnw_h7b0_dwt_reset);
}

static const TypeInfo gnw_h7b0_dwt_info = {
    .name          = TYPE_GNW_H7B0_DWT,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0DwtState),
    .instance_init = gnw_h7b0_dwt_init,
    .class_init    = gnw_h7b0_dwt_class_init,
};

static void gnw_h7b0_dwt_register_types(void)
{
    type_register_static(&gnw_h7b0_dwt_info);
}

type_init(gnw_h7b0_dwt_register_types)
