/*
 * Auto-generated stub for DAC, extended with real DHR->DOR transfer.
 *
 * Was a plain read/write shadow with no side effects at all -- fine for
 * firmware that only pokes DAC registers without ever reading back the
 * output, but stm32h7b0-diag's dac1_output_value/dac2_output_value cases
 * (this firmware's real LCD-backlight control path, MX_DAC1_Init()/
 * MX_DAC2_Init() in main.c, DAC_Trigger = NONE) write a known code via
 * HAL_DAC_SetValue() (-> DHR12Rx) and read it back via HAL_DAC_GetValue()
 * (-> DORx) to verify the digital output pipeline. Per RM0455: with
 * TENx=0 (untriggered, this firmware's actual config), the DHRx->DORx
 * transfer happens automatically ~1 APB1 clock after the DHRx write, no
 * software trigger needed. With no such transfer modeled at all, DORx
 * stayed permanently 0 regardless of what firmware wrote to DHRx.
 *
 * Only the untriggered (TENx=0) auto-transfer is modeled, on writes to
 * each channel's single-channel DHR registers (DHR12Rx/DHR12Lx/DHR8Rx --
 * the only ones any known firmware here uses; DHR12RD/DHR12LD/DHR8RD
 * dual-channel registers are not handled). Software-triggered transfers
 * (TENx=1 + SWTRGR) are not modeled -- no known firmware here uses them.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/misc/gnw_h7b0_dac.h"
#include "hw/misc/gnw_h7b0_regs_dac.h"

#define DAC_CR_EN1  (1U << 0)
#define DAC_CR_TEN1 (1U << 1)
#define DAC_CR_EN2  (1U << 16)
#define DAC_CR_TEN2 (1U << 17)

static void gnw_h7b0_dac_maybe_transfer(GnwH7B0DacState *s, hwaddr addr)
{
    uint32_t cr = s->regs[GNW_H7B0_DAC1_CR_OFFSET >> 2];
    uint32_t val;
    hwaddr dor_off;

    switch (addr) {
    case GNW_H7B0_DAC1_DHR12R1_OFFSET:
        val = s->regs[addr >> 2] & 0xfffU;
        dor_off = GNW_H7B0_DAC1_DOR1_OFFSET;
        if (cr & DAC_CR_TEN1) {
            return;
        }
        break;
    case GNW_H7B0_DAC1_DHR12L1_OFFSET:
        val = (s->regs[addr >> 2] >> 4) & 0xfffU;
        dor_off = GNW_H7B0_DAC1_DOR1_OFFSET;
        if (cr & DAC_CR_TEN1) {
            return;
        }
        break;
    case GNW_H7B0_DAC1_DHR8R1_OFFSET:
        val = (s->regs[addr >> 2] & 0xffU) << 4;
        dor_off = GNW_H7B0_DAC1_DOR1_OFFSET;
        if (cr & DAC_CR_TEN1) {
            return;
        }
        break;
    case GNW_H7B0_DAC1_DHR12R2_OFFSET:
        val = s->regs[addr >> 2] & 0xfffU;
        dor_off = GNW_H7B0_DAC1_DOR2_OFFSET;
        if (cr & DAC_CR_TEN2) {
            return;
        }
        break;
    case GNW_H7B0_DAC1_DHR12L2_OFFSET:
        val = (s->regs[addr >> 2] >> 4) & 0xfffU;
        dor_off = GNW_H7B0_DAC1_DOR2_OFFSET;
        if (cr & DAC_CR_TEN2) {
            return;
        }
        break;
    case GNW_H7B0_DAC1_DHR8R2_OFFSET:
        val = (s->regs[addr >> 2] & 0xffU) << 4;
        dor_off = GNW_H7B0_DAC1_DOR2_OFFSET;
        if (cr & DAC_CR_TEN2) {
            return;
        }
        break;
    default:
        return;
    }

    s->regs[dor_off >> 2] = val;
}

static void gnw_h7b0_dac_reset(DeviceState *dev)
{
    GnwH7B0DacState *s = GNW_H7B0_DAC(dev);
    for (int i = 0; i < (GNW_H7B0_DAC_SIZE / 4); i++) {
        s->regs[i] = get_dac_reset_value(i * 4);
    }
}

static uint64_t gnw_h7b0_dac_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0DacState *s = GNW_H7B0_DAC(opaque);
    if (addr >= GNW_H7B0_DAC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return 0;
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_dac_write(void *opaque, hwaddr addr, uint64_t val64, unsigned int size)
{
    GnwH7B0DacState *s = GNW_H7B0_DAC(opaque);
    if (addr >= GNW_H7B0_DAC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return;
    }
    uint32_t mask = get_dac_write_mask(addr);
    s->regs[addr >> 2] = (s->regs[addr >> 2] & ~mask) | ((uint32_t)val64 & mask);
    gnw_h7b0_dac_maybe_transfer(s, addr);
}

static const MemoryRegionOps gnw_h7b0_dac_ops = {
    .read = gnw_h7b0_dac_read,
    .write = gnw_h7b0_dac_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void gnw_h7b0_dac_init(Object *obj)
{
    GnwH7B0DacState *s = GNW_H7B0_DAC(obj);
    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_dac_ops, s, TYPE_GNW_H7B0_DAC, GNW_H7B0_DAC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const VMStateDescription vmstate_gnw_h7b0_dac = {
    .name = TYPE_GNW_H7B0_DAC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0DacState, GNW_H7B0_DAC_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_dac_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &vmstate_gnw_h7b0_dac;
    device_class_set_legacy_reset(dc, gnw_h7b0_dac_reset);
}

static const TypeInfo gnw_h7b0_dac_info = {
    .name          = TYPE_GNW_H7B0_DAC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0DacState),
    .instance_init = gnw_h7b0_dac_init,
    .class_init    = gnw_h7b0_dac_class_init,
};

static void gnw_h7b0_dac_register_types(void)
{
    type_register_static(&gnw_h7b0_dac_info);
}
type_init(gnw_h7b0_dac_register_types)
