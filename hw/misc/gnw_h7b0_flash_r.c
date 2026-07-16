/* FLASH_R (embedded flash controller) -- see gnw_h7b0_flash_r.h for the
 * real-erase-side-effect rationale. */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/misc/gnw_h7b0_flash_r.h"
#include "hw/misc/gnw_h7b0_regs_flash_r.h"

#define FLASH_CR1_OFFSET GNW_H7B0_FLASH_CR1_OFFSET
#define FLASH_CR2_OFFSET GNW_H7B0_FLASH_CR2_OFFSET

/* CR1/CR2 share this bit layout (see STM32H7B0.svd's FLASH_CR1/FLASH_CR2). */
#define CR_LOCK  (1U << 0)
#define CR_SER   (1U << 2)
#define CR_BER   (1U << 3)
#define CR_START (1U << 5)
#define CR_SSN_SHIFT 6
#define CR_SSN_MASK  (0x7fU << CR_SSN_SHIFT)

/* Real hardware: 256 KiB/bank; gnwmanager/gnw-flasher's own erase
 * granularity (INT_FLASH_ALIGN = 8192) is ground truth here per
 * docs/h7b0-flash-discrepancy.md -- RM0455/SVD's sector count for this
 * silicon doesn't match reality, this project already trusts the
 * community-verified value over the reference manual. */
#define SECTOR_SIZE (8 * 1024)

static void erase_sector(MemoryRegion *bank_mr, uint32_t sector)
{
    if (bank_mr == NULL) {
        return;
    }
    uint64_t bank_size = memory_region_size(bank_mr);
    uint64_t offset = (uint64_t)sector * SECTOR_SIZE;
    if (offset >= bank_size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gnw-h7b0-flash_r: sector %u out of range (bank size %"
                      PRIu64 ")\n", sector, bank_size);
        return;
    }
    uint64_t len = SECTOR_SIZE;
    if (offset + len > bank_size) {
        len = bank_size - offset;
    }
    uint8_t *host = memory_region_get_ram_ptr(bank_mr) + offset;
    memset(host, 0xff, len);
}

static void erase_bank(MemoryRegion *bank_mr)
{
    if (bank_mr == NULL) {
        return;
    }
    memset(memory_region_get_ram_ptr(bank_mr), 0xff, memory_region_size(bank_mr));
}

/* Apply CR1/CR2's real hardware side effect for whatever erase this write
 * just requested, then return the CR value with START/SER/BER/SSN cleared
 * -- real hardware "resets START when the operation has been acknowledged"
 * (STM32H7B0.svd's START1/START2 field description), and since this model
 * completes the erase synchronously within the write itself, that's
 * immediately. */
static uint32_t apply_erase_side_effect(GnwH7B0FlashRState *s, uint32_t cr,
                                         MemoryRegion *bank_mr)
{
    if (!(cr & CR_START)) {
        return cr;
    }
    if (cr & CR_BER) {
        erase_bank(bank_mr);
    } else if (cr & CR_SER) {
        erase_sector(bank_mr, (cr & CR_SSN_MASK) >> CR_SSN_SHIFT);
    }
    return cr & ~(CR_START | CR_SER | CR_BER | CR_SSN_MASK);
}

static void gnw_h7b0_flash_r_reset(DeviceState *dev)
{
    GnwH7B0FlashRState *s = GNW_H7B0_FLASH_R(dev);
    for (int i = 0; i < (GNW_H7B0_FLASH_R_SIZE / 4); i++) {
        s->regs[i] = get_flash_r_reset_value(i * 4);
    }
}

static uint64_t gnw_h7b0_flash_r_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0FlashRState *s = GNW_H7B0_FLASH_R(opaque);
    if (addr >= GNW_H7B0_FLASH_R_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return 0;
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_flash_r_write(void *opaque, hwaddr addr, uint64_t val64, unsigned int size)
{
    GnwH7B0FlashRState *s = GNW_H7B0_FLASH_R(opaque);
    if (addr >= GNW_H7B0_FLASH_R_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return;
    }
    uint32_t mask = get_flash_r_write_mask(addr);
    uint32_t value = (s->regs[addr >> 2] & ~mask) | ((uint32_t)val64 & mask);

    if (addr == FLASH_CR1_OFFSET) {
        value = apply_erase_side_effect(s, value, s->bank1_mr);
    } else if (addr == FLASH_CR2_OFFSET) {
        value = apply_erase_side_effect(s, value, s->bank2_mr);
    }

    s->regs[addr >> 2] = value;
}

static const MemoryRegionOps gnw_h7b0_flash_r_ops = {
    .read = gnw_h7b0_flash_r_read,
    .write = gnw_h7b0_flash_r_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

void gnw_h7b0_flash_r_set_banks(GnwH7B0FlashRState *s, MemoryRegion *bank1,
                                 MemoryRegion *bank2)
{
    s->bank1_mr = bank1;
    s->bank2_mr = bank2;
}

static void gnw_h7b0_flash_r_init(Object *obj)
{
    GnwH7B0FlashRState *s = GNW_H7B0_FLASH_R(obj);
    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_flash_r_ops, s, TYPE_GNW_H7B0_FLASH_R, GNW_H7B0_FLASH_R_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const VMStateDescription vmstate_gnw_h7b0_flash_r = {
    .name = TYPE_GNW_H7B0_FLASH_R,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0FlashRState, GNW_H7B0_FLASH_R_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_flash_r_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &vmstate_gnw_h7b0_flash_r;
    device_class_set_legacy_reset(dc, gnw_h7b0_flash_r_reset);
}

static const TypeInfo gnw_h7b0_flash_r_info = {
    .name          = TYPE_GNW_H7B0_FLASH_R,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0FlashRState),
    .instance_init = gnw_h7b0_flash_r_init,
    .class_init    = gnw_h7b0_flash_r_class_init,
};

static void gnw_h7b0_flash_r_register_types(void)
{
    type_register_static(&gnw_h7b0_flash_r_info);
}
type_init(gnw_h7b0_flash_r_register_types)
