/*
 * STM32H7B0 OCTOSPI minimal stub (Nintendo Game & Watch)
 *
 * See gnw_h7b0_ospi.h for scope/rationale.
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
#include "hw/misc/gnw_h7b0_ospi.h"

static void gnw_h7b0_ospi_reset(DeviceState *dev)
{
    GnwH7B0OspiState *s = GNW_H7B0_OSPI(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static uint64_t gnw_h7b0_ospi_read(void *opaque, hwaddr addr,
                                    unsigned int size)
{
    GnwH7B0OspiState *s = GNW_H7B0_OSPI(opaque);

    if (addr >= GNW_H7B0_OSPI_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_ospi_write(void *opaque, hwaddr addr,
                                 uint64_t val64, unsigned int size)
{
    GnwH7B0OspiState *s = GNW_H7B0_OSPI(opaque);
    uint32_t value = val64;

    if (addr >= GNW_H7B0_OSPI_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    switch (addr) {
    case GNW_H7B0_OSPI_CCR:
    case GNW_H7B0_OSPI_IR:
    case GNW_H7B0_OSPI_AR:
        /*
         * Real hardware starts the transaction once the command is
         * fully configured -- CCR/IR for HAL_OSPI_Command()'s no-data
         * path, or AR/IR (re-written by HAL_OSPI_Receive() itself,
         * after HAL_OSPI_Command() already configured but didn't
         * start the transfer) for the indirect-read data phase, per
         * whichever of AR/IR corresponds to the command's address
         * mode. No real transfer happens -- just instantly report it
         * as complete (TCF set, BUSY clear) so either polling loop
         * doesn't time out.
         */
        s->regs[addr >> 2] = value;
        s->regs[GNW_H7B0_OSPI_SR >> 2] |= OSPI_SR_TCF;
        s->regs[GNW_H7B0_OSPI_SR >> 2] &= ~OSPI_SR_BUSY;
        return;
    case GNW_H7B0_OSPI_FCR:
        /* Write-1-to-clear: FCR bit positions mirror SR's exactly. */
        s->regs[GNW_H7B0_OSPI_SR >> 2] &= ~value;
        return;
    case GNW_H7B0_OSPI_SR:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: SR is read-only on real hardware (use FCR to "
                      "clear flags)\n", __func__);
        return;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: offset 0x%"HWADDR_PRIx" is a plain read/write "
                      "shadow, no real command/data-phase transfer "
                      "modeled\n", __func__, addr);
        s->regs[addr >> 2] = value;
    }
}

static const MemoryRegionOps gnw_h7b0_ospi_ops = {
    .read = gnw_h7b0_ospi_read,
    .write = gnw_h7b0_ospi_write,
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

static void gnw_h7b0_ospi_init(Object *obj)
{
    GnwH7B0OspiState *s = GNW_H7B0_OSPI(obj);

    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_ospi_ops, s,
                           TYPE_GNW_H7B0_OSPI, GNW_H7B0_OSPI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const VMStateDescription vmstate_gnw_h7b0_ospi = {
    .name = TYPE_GNW_H7B0_OSPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0OspiState, GNW_H7B0_OSPI_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_ospi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_ospi;
    device_class_set_legacy_reset(dc, gnw_h7b0_ospi_reset);
}

static const TypeInfo gnw_h7b0_ospi_info = {
    .name          = TYPE_GNW_H7B0_OSPI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0OspiState),
    .instance_init = gnw_h7b0_ospi_init,
    .class_init    = gnw_h7b0_ospi_class_init,
};

static void gnw_h7b0_ospi_register_types(void)
{
    type_register_static(&gnw_h7b0_ospi_info);
}

type_init(gnw_h7b0_ospi_register_types)
