/*
 * STM32H7B0 OTFDEC minimal model (Nintendo Game & Watch)
 *
 * See gnw_h7b0_otfdec.h for scope/rationale.
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
#include "hw/misc/gnw_h7b0_otfdec.h"

/*
 * Exact reimplementation of the ROM'd key-CRC the peripheral computes
 * over a freshly loaded 128-bit key, mirrored from the HAL's software
 * twin, HAL_OTFDEC_KeyCRCComputation()
 * (sdk/stm32h7xx-hal-driver/Src/stm32h7xx_hal_otfdec.c) -- firmware
 * compares the two and refuses the key on mismatch, so this must match
 * bit-for-bit.
 */
static uint8_t otfdec_key_crc(const uint32_t *key)
{
    static const uint32_t key_strobe[4] = { 0xAA55AA55U, 0x3U, 0x18U, 0xC0U };
    uint8_t crc = 0;

    for (uint32_t j = 0; j < 4; j++) {
        uint32_t keyval = key[j];

        if (j == 0) {
            keyval ^= key_strobe[0];
        } else {
            keyval ^= (key_strobe[j] << 24) | ((uint32_t)crc << 16) |
                      (key_strobe[j] << 8) | crc;
        }

        crc = 0;
        for (uint8_t i = 0; i < 32; i++) {
            uint32_t k = (((uint32_t)crc >> 7) ^
                          ((keyval >> (31 - i)) & 0xFU)) & 1U;
            crc <<= 1;
            if (k) {
                crc ^= 0x7; /* CRC-7 polynomial */
            }
        }

        crc ^= 0x55;
    }

    return crc;
}

static void gnw_h7b0_otfdec_reset(DeviceState *dev)
{
    GnwH7B0OtfdecState *s = GNW_H7B0_OTFDEC(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static uint64_t gnw_h7b0_otfdec_read(void *opaque, hwaddr addr,
                                      unsigned int size)
{
    GnwH7B0OtfdecState *s = GNW_H7B0_OTFDEC(opaque);

    if (addr >= GNW_H7B0_OTFDEC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_otfdec_write(void *opaque, hwaddr addr,
                                   uint64_t val64, unsigned int size)
{
    GnwH7B0OtfdecState *s = GNW_H7B0_OTFDEC(opaque);

    if (addr >= GNW_H7B0_OTFDEC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    s->regs[addr >> 2] = (uint32_t)val64;

    /*
     * Writing a region's KEYR3 completes a key load: real hardware
     * then exposes the key's CRC in that region's CONFIGR.KEYCRC,
     * which HAL_OTFDEC_RegionSetKey() reads back to confirm the load.
     */
    if (addr >= OTFDEC_REGION_BASE &&
        addr < OTFDEC_REGION_BASE +
               OTFDEC_NUM_REGIONS * OTFDEC_REGION_STRIDE) {
        hwaddr region_base = OTFDEC_REGION_BASE +
            ((addr - OTFDEC_REGION_BASE) / OTFDEC_REGION_STRIDE) *
            OTFDEC_REGION_STRIDE;

        if (addr - region_base == OTFDEC_REG_KEYR3) {
            uint32_t key[4];
            uint32_t *configr = &s->regs[(region_base +
                                          OTFDEC_REG_CONFIGR) >> 2];

            for (int i = 0; i < 4; i++) {
                key[i] = s->regs[((region_base + OTFDEC_REG_KEYR0) >> 2) + i];
            }
            *configr = (*configr & ~OTFDEC_CONFIGR_KEYCRC_MASK) |
                       ((uint32_t)otfdec_key_crc(key)
                        << OTFDEC_CONFIGR_KEYCRC_SHIFT);
        }
    }
}

static const MemoryRegionOps gnw_h7b0_otfdec_ops = {
    .read = gnw_h7b0_otfdec_read,
    .write = gnw_h7b0_otfdec_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void gnw_h7b0_otfdec_init(Object *obj)
{
    GnwH7B0OtfdecState *s = GNW_H7B0_OTFDEC(obj);

    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_otfdec_ops, s,
                           TYPE_GNW_H7B0_OTFDEC, GNW_H7B0_OTFDEC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const VMStateDescription vmstate_gnw_h7b0_otfdec = {
    .name = TYPE_GNW_H7B0_OTFDEC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0OtfdecState,
                             GNW_H7B0_OTFDEC_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_otfdec_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_otfdec;
    device_class_set_legacy_reset(dc, gnw_h7b0_otfdec_reset);
}

static const TypeInfo gnw_h7b0_otfdec_info = {
    .name          = TYPE_GNW_H7B0_OTFDEC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0OtfdecState),
    .instance_init = gnw_h7b0_otfdec_init,
    .class_init    = gnw_h7b0_otfdec_class_init,
};

static void gnw_h7b0_otfdec_register_types(void)
{
    type_register_static(&gnw_h7b0_otfdec_info);
}
type_init(gnw_h7b0_otfdec_register_types)
