/*
 * STM32H7B0 LPTIM1 free-running-counter minimal stub (Nintendo Game & Watch)
 *
 * See gnw_h7b0_lptim1.h for scope/rationale.
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
#include "hw/core/irq.h"
#include "hw/misc/gnw_h7b0_lptim1.h"

static uint32_t gnw_h7b0_lptim1_cnt(GnwH7B0Lptim1State *s)
{
    uint32_t arr = s->regs[GNW_H7B0_LPTIM1_ARR >> 2];
    uint64_t modulus = (uint64_t)arr + 1;

    if (!s->running) {
        return s->base_count % modulus;
    }

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t elapsed_ns = now - s->enabled_since_ns;
    uint64_t elapsed_counts = (uint64_t)elapsed_ns * GNW_H7B0_LPTIM1_ASSUMED_HZ
                               / NANOSECONDS_PER_SECOND;

    return (uint32_t)((s->base_count + elapsed_counts) % modulus);
}

static void gnw_h7b0_lptim1_reset(DeviceState *dev)
{
    GnwH7B0Lptim1State *s = GNW_H7B0_LPTIM1(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[GNW_H7B0_LPTIM1_ARR >> 2] = 1; /* SVD reset value */
    s->running = false;
    s->base_count = 0;
    s->enabled_since_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static uint64_t gnw_h7b0_lptim1_read(void *opaque, hwaddr addr,
                                      unsigned int size)
{
    GnwH7B0Lptim1State *s = GNW_H7B0_LPTIM1(opaque);

    if (addr >= GNW_H7B0_LPTIM1_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }
    if (addr == GNW_H7B0_LPTIM1_CNT) {
        return gnw_h7b0_lptim1_cnt(s);
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_lptim1_write(void *opaque, hwaddr addr,
                                   uint64_t val64, unsigned int size)
{
    GnwH7B0Lptim1State *s = GNW_H7B0_LPTIM1(opaque);
    uint32_t value = val64;

    if (addr >= GNW_H7B0_LPTIM1_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    if (addr == GNW_H7B0_LPTIM1_ARR) {
        s->regs[addr >> 2] = value;
        /*
         * Real hardware sets ISR.ARROK once the write to ARR (a
         * different clock domain) is actually committed; HAL_LPTIM_
         * Counter_Start() (sdk/stm32h7xx-hal-driver/Src/
         * stm32h7xx_hal_lptim.c) writes ARR then calls
         * LPTIM_WaitForFlag(ARROK) BEFORE issuing CNTSTRT -- without
         * this, that wait always times out and Counter_Start() returns
         * early, so CNTSTRT (and thus any actual counting) never
         * happens at all. Same bug class as this project's recurring
         * "auto-generated stub never sets the completion flag a real
         * HAL polling loop depends on" pattern; set it instantly like
         * every other such fix here. */
        s->regs[GNW_H7B0_LPTIM1_ISR >> 2] |= LPTIM_ISR_ARROK;
        return;
    }
    if (addr == GNW_H7B0_LPTIM1_CMP) {
        s->regs[addr >> 2] = value;
        /* Same rationale as ARR/ARROK above, for CMPOK. */
        s->regs[GNW_H7B0_LPTIM1_ISR >> 2] |= LPTIM_ISR_CMPOK;
        return;
    }
    if (addr == GNW_H7B0_LPTIM1_ICR) {
        /* Write-1-to-clear; ICR bit positions mirror ISR's. */
        s->regs[GNW_H7B0_LPTIM1_ISR >> 2] &= ~(value &
            (LPTIM_ICR_ARROKCF | LPTIM_ICR_CMPOKCF));
        return;
    }
    if (addr == GNW_H7B0_LPTIM1_CR) {
        bool was_running = s->running;

        s->regs[addr >> 2] = value;

        if ((value & LPTIM_CR_ENABLE) &&
            (value & (LPTIM_CR_SNGSTRT | LPTIM_CR_CNTSTRT))) {
            if (!was_running) {
                s->base_count = 0;
            }
            s->enabled_since_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            s->running = true;
        } else if (!(value & LPTIM_CR_ENABLE)) {
            s->base_count = gnw_h7b0_lptim1_cnt(s);
            s->running = false;
        }
        return;
    }

    qemu_log_mask(LOG_UNIMP,
                  "%s: offset 0x%"HWADDR_PRIx" is a plain read/write "
                  "shadow, no real behavior modeled\n",
                  __func__, addr);
    s->regs[addr >> 2] = value;
}

static const MemoryRegionOps gnw_h7b0_lptim1_ops = {
    .read = gnw_h7b0_lptim1_read,
    .write = gnw_h7b0_lptim1_write,
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

static void gnw_h7b0_lptim1_init(Object *obj)
{
    GnwH7B0Lptim1State *s = GNW_H7B0_LPTIM1(obj);

    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_lptim1_ops, s,
                           TYPE_GNW_H7B0_LPTIM1, GNW_H7B0_LPTIM1_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription vmstate_gnw_h7b0_lptim1 = {
    .name = TYPE_GNW_H7B0_LPTIM1,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0Lptim1State,
                              GNW_H7B0_LPTIM1_SIZE / 4),
        VMSTATE_BOOL(running, GnwH7B0Lptim1State),
        VMSTATE_UINT32(base_count, GnwH7B0Lptim1State),
        VMSTATE_INT64(enabled_since_ns, GnwH7B0Lptim1State),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_lptim1_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_lptim1;
    device_class_set_legacy_reset(dc, gnw_h7b0_lptim1_reset);
}

static const TypeInfo gnw_h7b0_lptim1_info = {
    .name          = TYPE_GNW_H7B0_LPTIM1,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0Lptim1State),
    .instance_init = gnw_h7b0_lptim1_init,
    .class_init    = gnw_h7b0_lptim1_class_init,
};

static void gnw_h7b0_lptim1_register_types(void)
{
    type_register_static(&gnw_h7b0_lptim1_info);
}

type_init(gnw_h7b0_lptim1_register_types)
