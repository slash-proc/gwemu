/* Auto-generated stub for IWDG */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/misc/gnw_h7b0_iwdg.h"
#include "hw/misc/gnw_h7b0_regs_iwdg.h"

static void gnw_h7b0_iwdg_reset(DeviceState *dev)
{
    GnwH7B0IwdgState *s = GNW_H7B0_IWDG(dev);
    for (int i = 0; i < (GNW_H7B0_IWDG_SIZE / 4); i++) {
        s->regs[i] = get_iwdg_reset_value(i * 4);
    }
}

static uint64_t gnw_h7b0_iwdg_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0IwdgState *s = GNW_H7B0_IWDG(opaque);
    if (addr >= GNW_H7B0_IWDG_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return 0;
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_iwdg_write(void *opaque, hwaddr addr, uint64_t val64, unsigned int size)
{
    GnwH7B0IwdgState *s = GNW_H7B0_IWDG(opaque);
    if (addr >= GNW_H7B0_IWDG_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return;
    }
    uint32_t mask = get_iwdg_write_mask(addr);
    s->regs[addr >> 2] = (s->regs[addr >> 2] & ~mask) | ((uint32_t)val64 & mask);
}

static const MemoryRegionOps gnw_h7b0_iwdg_ops = {
    .read = gnw_h7b0_iwdg_read,
    .write = gnw_h7b0_iwdg_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void gnw_h7b0_iwdg_init(Object *obj)
{
    GnwH7B0IwdgState *s = GNW_H7B0_IWDG(obj);
    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_iwdg_ops, s, TYPE_GNW_H7B0_IWDG, GNW_H7B0_IWDG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const VMStateDescription vmstate_gnw_h7b0_iwdg = {
    .name = TYPE_GNW_H7B0_IWDG,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0IwdgState, GNW_H7B0_IWDG_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_iwdg_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &vmstate_gnw_h7b0_iwdg;
    device_class_set_legacy_reset(dc, gnw_h7b0_iwdg_reset);
}

static const TypeInfo gnw_h7b0_iwdg_info = {
    .name          = TYPE_GNW_H7B0_IWDG,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0IwdgState),
    .instance_init = gnw_h7b0_iwdg_init,
    .class_init    = gnw_h7b0_iwdg_class_init,
};

static void gnw_h7b0_iwdg_register_types(void)
{
    type_register_static(&gnw_h7b0_iwdg_info);
}
type_init(gnw_h7b0_iwdg_register_types)
