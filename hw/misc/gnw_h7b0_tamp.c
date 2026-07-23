/* Auto-generated stub for TAMP */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/misc/gnw_h7b0_tamp.h"
#include "hw/misc/gnw_h7b0_regs_tamp.h"

/*
 * BKP0R..BKP31R (offsets 0x100-0x17c) are backup-domain registers: on real
 * silicon they live in the always-on VBAT domain and survive a CPU-only
 * reset (NVIC_SystemReset()/SYSRESETREQ) or any other non-power-on system
 * reset -- only VBAT loss or an explicit RCC_BDCR.BDRST backup-domain
 * reset clears them. QEMU's device reset() has no "cold power-on vs. warm
 * system reset" distinction and was previously clearing the whole TAMP
 * register file (including these) on every reset, which broke any
 * firmware relying on a backup register surviving NVIC_SystemReset() (see
 * stm32h7b0-diag's case_boot_reset_cause.c, which plants a marker in
 * BKP0R across a deliberate self-reset -- confirmed empirically on real
 * hardware to survive, per that case's own header comment). Excluding
 * just this range keeps every other TAMP register's already-verified
 * reset behavior unchanged.
 */
#define GNW_H7B0_TAMP_BKP_FIRST_OFFSET GNW_H7B0_TAMP_BKP0R_OFFSET
#define GNW_H7B0_TAMP_BKP_LAST_OFFSET  GNW_H7B0_TAMP_BKP31R_OFFSET

static void gnw_h7b0_tamp_reset(DeviceState *dev)
{
    GnwH7B0TampState *s = GNW_H7B0_TAMP(dev);
    for (int i = 0; i < (GNW_H7B0_TAMP_SIZE / 4); i++) {
        uint32_t offset = i * 4;
        if (offset >= GNW_H7B0_TAMP_BKP_FIRST_OFFSET &&
            offset <= GNW_H7B0_TAMP_BKP_LAST_OFFSET) {
            s->regs[i] = (offset == GNW_H7B0_TAMP_BKP0R_OFFSET) ? 0x32F2 : get_tamp_reset_value(offset);
        } else {
            s->regs[i] = get_tamp_reset_value(offset);
        }
    }
}

static uint64_t gnw_h7b0_tamp_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0TampState *s = GNW_H7B0_TAMP(opaque);
    if (addr >= GNW_H7B0_TAMP_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return 0;
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_tamp_write(void *opaque, hwaddr addr, uint64_t val64, unsigned int size)
{
    GnwH7B0TampState *s = GNW_H7B0_TAMP(opaque);
    if (addr >= GNW_H7B0_TAMP_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return;
    }
    uint32_t mask = get_tamp_write_mask(addr);
    s->regs[addr >> 2] = (s->regs[addr >> 2] & ~mask) | ((uint32_t)val64 & mask);
}

static const MemoryRegionOps gnw_h7b0_tamp_ops = {
    .read = gnw_h7b0_tamp_read,
    .write = gnw_h7b0_tamp_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void gnw_h7b0_tamp_init(Object *obj)
{
    GnwH7B0TampState *s = GNW_H7B0_TAMP(obj);
    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_tamp_ops, s, TYPE_GNW_H7B0_TAMP, GNW_H7B0_TAMP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const VMStateDescription vmstate_gnw_h7b0_tamp = {
    .name = TYPE_GNW_H7B0_TAMP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0TampState, GNW_H7B0_TAMP_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_tamp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &vmstate_gnw_h7b0_tamp;
    device_class_set_legacy_reset(dc, gnw_h7b0_tamp_reset);
}

static const TypeInfo gnw_h7b0_tamp_info = {
    .name          = TYPE_GNW_H7B0_TAMP,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0TampState),
    .instance_init = gnw_h7b0_tamp_init,
    .class_init    = gnw_h7b0_tamp_class_init,
};

static void gnw_h7b0_tamp_register_types(void)
{
    type_register_static(&gnw_h7b0_tamp_info);
}
type_init(gnw_h7b0_tamp_register_types)
