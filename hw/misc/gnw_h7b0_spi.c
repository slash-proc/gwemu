/*
 * STM32H7B0 SPI minimal stub (Nintendo Game & Watch)
 *
 * See gnw_h7b0_spi.h for scope/rationale.
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
#include "hw/misc/gnw_h7b0_spi.h"

static void gnw_h7b0_spi_reset(DeviceState *dev)
{
    GnwH7B0SpiState *s = GNW_H7B0_SPI(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static uint64_t gnw_h7b0_spi_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0SpiState *s = GNW_H7B0_SPI(opaque);

    if (addr >= GNW_H7B0_SPI_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_spi_write(void *opaque, hwaddr addr,
                                uint64_t val64, unsigned int size)
{
    GnwH7B0SpiState *s = GNW_H7B0_SPI(opaque);
    uint32_t value = val64;

    if (addr >= GNW_H7B0_SPI_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    switch (addr) {
    case GNW_H7B0_SPI_CR1:
        s->regs[addr >> 2] = value;
        if (value & SPI_CR1_SPE) {
            /*
             * No real transfer backpressure modeled -- TXP is always
             * "ready" once the peripheral is enabled, so the
             * byte-at-a-time send loop never blocks.
             */
            s->regs[GNW_H7B0_SPI_SR >> 2] |= SPI_SR_TXP;
        } else {
            s->regs[GNW_H7B0_SPI_SR >> 2] &= ~(SPI_SR_TXP | SPI_SR_EOT);
        }
        return;
    case GNW_H7B0_SPI_TXDR:
        /*
         * The byte goes nowhere (no LCD-panel model to receive it).
         * Real hardware only sets EOT once the whole configured
         * transfer finishes; approximated here as "every write
         * instantly completes the transfer" since nothing currently
         * tracks a configured byte count.
         */
        s->regs[addr >> 2] = value;
        s->regs[GNW_H7B0_SPI_SR >> 2] |= SPI_SR_EOT;
        return;
    case GNW_H7B0_SPI_IFCR:
        /* Write-1-to-clear: IFCR bit positions mirror SR's exactly. */
        s->regs[GNW_H7B0_SPI_SR >> 2] &= ~value;
        return;
    case GNW_H7B0_SPI_SR:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: SR is read-only on real hardware (use IFCR to "
                      "clear flags)\n", __func__);
        return;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: offset 0x%"HWADDR_PRIx" is a plain read/write "
                      "shadow, no real transfer modeled\n", __func__, addr);
        s->regs[addr >> 2] = value;
    }
}

static const MemoryRegionOps gnw_h7b0_spi_ops = {
    .read = gnw_h7b0_spi_read,
    .write = gnw_h7b0_spi_write,
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

static void gnw_h7b0_spi_init(Object *obj)
{
    GnwH7B0SpiState *s = GNW_H7B0_SPI(obj);

    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_spi_ops, s,
                           TYPE_GNW_H7B0_SPI, GNW_H7B0_SPI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const VMStateDescription vmstate_gnw_h7b0_spi = {
    .name = TYPE_GNW_H7B0_SPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0SpiState, GNW_H7B0_SPI_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_spi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_spi;
    device_class_set_legacy_reset(dc, gnw_h7b0_spi_reset);
}

static const TypeInfo gnw_h7b0_spi_info = {
    .name          = TYPE_GNW_H7B0_SPI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0SpiState),
    .instance_init = gnw_h7b0_spi_init,
    .class_init    = gnw_h7b0_spi_class_init,
};

static void gnw_h7b0_spi_register_types(void)
{
    type_register_static(&gnw_h7b0_spi_info);
}

type_init(gnw_h7b0_spi_register_types)
