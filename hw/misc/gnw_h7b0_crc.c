/*
 * STM32H7B0 hardware CRC-32 unit (Nintendo Game & Watch)
 *
 * See gnw_h7b0_crc.h for scope/rationale.
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
#include "hw/misc/gnw_h7b0_crc.h"
#include "hw/misc/gnw_h7b0_regs_crc.h"

static uint32_t bit_reverse(uint32_t v, unsigned int width_bits)
{
    uint32_t r = 0;

    for (unsigned int i = 0; i < width_bits; i++) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

/*
 * One bit-serial CRC step, matching the real STM32 CRC unit's
 * documented behaviour (RM0455 "CRC calculation unit" chapter): the
 * new data word (after optional REV_IN bit-reversal) is XORed into
 * the top of the 32-bit accumulator, then shifted/XORed against the
 * polynomial one bit per input bit. gnw-chainloader's ofw_crc32()
 * only ever exercises the 32-bit/no-reversal path (explicitly forces
 * CR to that state before every use), but the smaller POLYSIZE widths
 * and REV_IN/REV_OUT are cheap to support faithfully too.
 */
static void crc_feed(GnwH7B0CrcState *s, uint32_t data, unsigned int width_bits)
{
    unsigned int rev_in = (s->cr & CRC_CR_REV_IN_MASK) >> CRC_CR_REV_IN_SHIFT;
    uint32_t crc = s->dr;

    if (rev_in) {
        data = bit_reverse(data, width_bits);
    }

    crc ^= data << (32 - width_bits);
    for (unsigned int i = 0; i < width_bits; i++) {
        crc = (crc & 0x80000000U) ? (crc << 1) ^ s->pol : (crc << 1);
    }

    s->dr = crc;
}

static uint32_t crc_read_dr(GnwH7B0CrcState *s)
{
    if (s->cr & CRC_CR_REV_OUT) {
        return bit_reverse(s->dr, 32);
    }
    return s->dr;
}

static void gnw_h7b0_crc_reset(DeviceState *dev)
{
    GnwH7B0CrcState *s = GNW_H7B0_CRC(dev);

    s->dr = get_crc_reset_value(GNW_H7B0_CRC_DR);
    s->idr = get_crc_reset_value(GNW_H7B0_CRC_IDR);
    s->cr = get_crc_reset_value(GNW_H7B0_CRC_CR);
    s->init = get_crc_reset_value(GNW_H7B0_CRC_INIT);
    s->pol = get_crc_reset_value(GNW_H7B0_CRC_POL);
}

static uint64_t gnw_h7b0_crc_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0CrcState *s = GNW_H7B0_CRC(opaque);

    switch (addr) {
    case GNW_H7B0_CRC_DR:
        return crc_read_dr(s);
    case GNW_H7B0_CRC_IDR:
        return s->idr;
    case GNW_H7B0_CRC_CR:
        return s->cr;
    case GNW_H7B0_CRC_INIT:
        return s->init;
    case GNW_H7B0_CRC_POL:
        return s->pol;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }
}

static void gnw_h7b0_crc_write(void *opaque, hwaddr addr,
                                uint64_t val64, unsigned int size)
{
    GnwH7B0CrcState *s = GNW_H7B0_CRC(opaque);
    uint32_t value = val64;

    switch (addr) {
    case GNW_H7B0_CRC_DR:
        /*
         * `size` is the actual STR width the firmware used (matches
         * ofw_crc32()'s `CRC->DR = *p++` 32-bit word writes) -- real
         * hardware instead derives the input width from CR.POLYSIZE,
         * but since firmware is expected to keep those in sync (an
         * 8/16-bit POLYSIZE config would normally be paired with
         * matching-width DR writes), using the access size directly
         * here gets the same answer without extra bookkeeping.
         */
        value = (value & get_crc_write_mask(addr));
        crc_feed(s, value, size * 8);
        return;
    case GNW_H7B0_CRC_IDR:
        s->idr = (s->idr & ~get_crc_write_mask(addr)) | (value & get_crc_write_mask(addr));
        return;
    case GNW_H7B0_CRC_CR:
        value = (s->cr & ~get_crc_write_mask(addr)) | (value & get_crc_write_mask(addr));
        s->cr = value & ~CRC_CR_RESET; /* RESET is write-only/self-clearing. */
        if (value & CRC_CR_RESET) {
            s->dr = s->init;
        }
        return;
    case GNW_H7B0_CRC_INIT:
        s->init = (s->init & ~get_crc_write_mask(addr)) | (value & get_crc_write_mask(addr));
        return;
    case GNW_H7B0_CRC_POL:
        s->pol = (s->pol & ~get_crc_write_mask(addr)) | (value & get_crc_write_mask(addr));
        return;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
    }
}

static const MemoryRegionOps gnw_h7b0_crc_ops = {
    .read = gnw_h7b0_crc_read,
    .write = gnw_h7b0_crc_write,
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

static void gnw_h7b0_crc_init(Object *obj)
{
    GnwH7B0CrcState *s = GNW_H7B0_CRC(obj);

    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_crc_ops, s,
                           TYPE_GNW_H7B0_CRC, GNW_H7B0_CRC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const VMStateDescription vmstate_gnw_h7b0_crc = {
    .name = TYPE_GNW_H7B0_CRC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(dr, GnwH7B0CrcState),
        VMSTATE_UINT32(idr, GnwH7B0CrcState),
        VMSTATE_UINT32(cr, GnwH7B0CrcState),
        VMSTATE_UINT32(init, GnwH7B0CrcState),
        VMSTATE_UINT32(pol, GnwH7B0CrcState),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_crc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_crc;
    device_class_set_legacy_reset(dc, gnw_h7b0_crc_reset);
}

static const TypeInfo gnw_h7b0_crc_info = {
    .name          = TYPE_GNW_H7B0_CRC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0CrcState),
    .instance_init = gnw_h7b0_crc_init,
    .class_init    = gnw_h7b0_crc_class_init,
};

static void gnw_h7b0_crc_register_types(void)
{
    type_register_static(&gnw_h7b0_crc_info);
}

type_init(gnw_h7b0_crc_register_types)
