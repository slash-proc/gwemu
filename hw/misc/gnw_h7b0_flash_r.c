/* FLASH_R (embedded flash controller) -- see gnw_h7b0_flash_r.h for the
 * real-erase-side-effect rationale. */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "system/physmem.h"
#include "migration/vmstate.h"
#include "hw/misc/gnw_h7b0_flash_r.h"
#include "hw/misc/gnw_h7b0_regs_flash_r.h"

#define FLASH_CR1_OFFSET GNW_H7B0_FLASH_CR1_OFFSET
#define FLASH_CR2_OFFSET GNW_H7B0_FLASH_CR2_OFFSET
#define OPTKEY1 0x08192a3bU
#define OPTKEY2 0x4c5d6e7fU
#define OPTLOCK (1U << 0)
#define OPTSTART (1U << 1)
#define OPTCHANGEERR (1U << 30)
#define RDP_SHIFT 8
#define RDP_MASK (0xffU << RDP_SHIFT)
#define RDP_LEVEL0 0xaa
#define RDP_LEVEL2 0xcc

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

static bool rdp_is_level2(uint8_t rdp)
{
    return rdp == RDP_LEVEL2;
}

static bool rdp_is_level0(uint8_t rdp)
{
    return rdp == RDP_LEVEL0;
}

static void update_rdp_registers(GnwH7B0FlashRState *s, uint8_t rdp)
{
    uint32_t *regs = s->regs;
    s->rdp_value = rdp;
    regs[GNW_H7B0_FLASH_OPTSR_CUR_OFFSET >> 2] =
        (regs[GNW_H7B0_FLASH_OPTSR_CUR_OFFSET >> 2] & ~RDP_MASK) |
        ((uint32_t)rdp << RDP_SHIFT);
    regs[GNW_H7B0_FLASH_OPTSR_CUR__OFFSET >> 2] =
        (regs[GNW_H7B0_FLASH_OPTSR_CUR__OFFSET >> 2] & ~RDP_MASK) |
        ((uint32_t)rdp << RDP_SHIFT);
    regs[GNW_H7B0_FLASH_OPTSR_PRG_OFFSET >> 2] =
        (regs[GNW_H7B0_FLASH_OPTSR_PRG_OFFSET >> 2] & ~RDP_MASK) |
        ((uint32_t)rdp << RDP_SHIFT);
    regs[GNW_H7B0_FLASH_OPTSR_PRG__OFFSET >> 2] =
        (regs[GNW_H7B0_FLASH_OPTSR_PRG__OFFSET >> 2] & ~RDP_MASK) |
        ((uint32_t)rdp << RDP_SHIFT);
}

static void persist_rdp(GnwH7B0FlashRState *s)
{
    GError *err = NULL;
    if (!s->rdp_image || !g_file_set_contents(s->rdp_image,
                                               (const char *)&s->rdp_value,
                                               1, &err)) {
        if (err) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "gnw-h7b0-flash_r: could not persist RDP option "
                          "byte to '%s': %s\n",
                          s->rdp_image, err->message);
            g_error_free(err);
        }
    }
}

static void apply_option_start(GnwH7B0FlashRState *s)
{
    uint32_t *regs = s->regs;
    uint8_t old_rdp = s->rdp_value;
    uint8_t new_rdp = (regs[GNW_H7B0_FLASH_OPTSR_PRG_OFFSET >> 2] &
                       RDP_MASK) >> RDP_SHIFT;

    /* RDP2 is an irreversible fuse state; only an unchanged RDP value is
     * accepted after it has been selected. */
    if (rdp_is_level2(old_rdp) && new_rdp != RDP_LEVEL2) {
        regs[GNW_H7B0_FLASH_OPTSR_CUR_OFFSET >> 2] |= OPTCHANGEERR;
        regs[GNW_H7B0_FLASH_OPTSR_CUR__OFFSET >> 2] |= OPTCHANGEERR;
        return;
    }

    /* RM0455 §4.5.3: regression from level 1 to level 0 erases user flash.
     * The H7B0 has two 256 KiB banks in real silicon (see the repo's flash
     * discrepancy note); raising protection from level 0 to level 1 does
     * not erase either bank. */
    if (!rdp_is_level0(old_rdp) && rdp_is_level0(new_rdp)) {
        erase_bank(s->bank1_mr);
        erase_bank(s->bank2_mr);
    }

    update_rdp_registers(s, new_rdp);
    persist_rdp(s);
}

static bool flash_debug_access_allowed(void *opaque, hwaddr addr,
                                       hwaddr len, bool is_write)
{
    GnwH7B0FlashRState *s = opaque;

    (void)addr;
    (void)len;
    (void)is_write;
    return rdp_is_level0(s->rdp_value);
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

    /* OPTSR_CUR/OPTSR_CUR_ (bank1/bank2 aliases, offsets 0x1c/0x11c):
     * RM0455 documents this register's reset value as "0xXXXX XXXX (see
     * Table 21: Option byte organization)" -- unlike this device's other
     * registers, it isn't a fixed silicon constant; it's the shadow copy
     * of the non-volatile option bytes, loaded at reset. RM0455's option
     * byte section is explicit about the factory-programmed default
     * STMicroelectronics ships: "RDP level 0 (option byte value = 0xAA)".
     * Leaving RDP (bits [15:8]) at the auto-generated stub's 0x00000000
     * reads back as an *unrelated* non-0xAA/0xCC value, which per RM0455's
     * own RDP-level decoding is level 1 (protected) -- the opposite of
     * the real, unprotected factory-default level 0 this hardware
     * actually boots with. Confirmed independently against real hardware
     * (OPTSR_CUR reads 0xAA there too). Every other OPTSR_CUR field
     * (BOR_LEV, IWDG_SW, NRST_STOP/STDY, WDG_FZ_STOP/SDBY, ST_RAM_SIZE,
     * SECURITY, VDDIO_HSLV, SWAP_BANK_OPT) has no documented non-zero
     * factory default in RM0455 -- SWAP_BANK_OPT is explicitly called out
     * as "not available on STM32H7B0 devices... must be kept at '0'" --
     * so only RDP is seeded here, not a blanket non-zero value. */
    if (!s->rdp_initialized) {
        char *contents = NULL;
        gsize length = 0;
        GError *err = NULL;
        if (s->rdp_image &&
            g_file_get_contents(s->rdp_image, &contents, &length, &err) &&
            length == 1) {
            s->rdp_value = (uint8_t)contents[0];
        } else {
            s->rdp_value = s->initial_locked ? 0x55 : RDP_LEVEL0;
        }
        g_free(contents);
        g_clear_error(&err);
        s->rdp_initialized = true;
    }
    update_rdp_registers(s, s->rdp_value);
    s->optkey_stage = 0;
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
    uint32_t reg_offset;
    unsigned shift;
    uint32_t lane_mask, mask, value;
    if (addr >= GNW_H7B0_FLASH_R_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return;
    }
    reg_offset = addr & ~3U;
    shift = (addr & 3U) * 8;
    lane_mask = size == 4 ? UINT32_MAX : ((1U << (size * 8)) - 1) << shift;
    mask = get_flash_r_write_mask(reg_offset) & lane_mask;
    value = (s->regs[reg_offset >> 2] & ~mask) |
            (((uint32_t)val64 << shift) & mask);

    if (reg_offset == GNW_H7B0_FLASH_OPTSR_CUR_OFFSET ||
        reg_offset == GNW_H7B0_FLASH_OPTSR_CUR__OFFSET) {
        return; /* _CUR option registers are read-only. */
    }

    if (reg_offset == GNW_H7B0_FLASH_OPTKEYR_OFFSET ||
        reg_offset == GNW_H7B0_FLASH_OPTKEYR__OFFSET) {
        if (size == 4 && shift == 0 && val64 == OPTKEY1) {
            s->optkey_stage = 1;
        } else if (size == 4 && shift == 0 && val64 == OPTKEY2 &&
                   s->optkey_stage == 1) {
            s->optkey_stage = 0;
            s->regs[GNW_H7B0_FLASH_OPTCR_OFFSET >> 2] &= ~OPTLOCK;
            s->regs[GNW_H7B0_FLASH_OPTCR__OFFSET >> 2] &= ~OPTLOCK;
        } else {
            s->optkey_stage = 0;
        }
        return;
    }

    if ((reg_offset == GNW_H7B0_FLASH_OPTSR_PRG_OFFSET ||
         reg_offset == GNW_H7B0_FLASH_OPTSR_PRG__OFFSET) &&
        (s->regs[GNW_H7B0_FLASH_OPTCR_OFFSET >> 2] & OPTLOCK)) {
        return;
    }

    if (reg_offset == GNW_H7B0_FLASH_OPTCR_OFFSET ||
        reg_offset == GNW_H7B0_FLASH_OPTCR__OFFSET) {
        unsigned int optcr_index = GNW_H7B0_FLASH_OPTCR_OFFSET >> 2;
        if (s->regs[optcr_index] & OPTLOCK) {
            return;
        }
        s->regs[reg_offset >> 2] = value;
        if (value & OPTSTART) {
            apply_option_start(s);
            s->regs[reg_offset >> 2] &= ~OPTSTART;
        }
        return;
    }

    if (reg_offset == FLASH_CR1_OFFSET) {
        value = apply_erase_side_effect(s, value, s->bank1_mr);
    } else if (reg_offset == FLASH_CR2_OFFSET) {
        value = apply_erase_side_effect(s, value, s->bank2_mr);
    }

    s->regs[reg_offset >> 2] = value;
}

static const MemoryRegionOps gnw_h7b0_flash_r_ops = {
    .read = gnw_h7b0_flash_r_read,
    .write = gnw_h7b0_flash_r_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static int gnw_h7b0_flash_r_post_load(void *opaque, int version_id)
{
    GnwH7B0FlashRState *s = GNW_H7B0_FLASH_R(opaque);

    if (version_id < 2) {
        /* Version 1 migrated the raw option registers before RDP state was
         * modeled. Recover the existing shadow value so migration does not
         * silently reset a protected target to the factory default. */
        s->rdp_value = (s->regs[GNW_H7B0_FLASH_OPTSR_CUR_OFFSET >> 2] &
                        RDP_MASK) >> RDP_SHIFT;
        s->rdp_initialized = true;
        s->optkey_stage = 0;
    }
    update_rdp_registers(s, s->rdp_value);
    return 0;
}

void gnw_h7b0_flash_r_set_banks(GnwH7B0FlashRState *s, MemoryRegion *bank1,
                                 MemoryRegion *bank2)
{
    s->bank1_mr = bank1;
    s->bank2_mr = bank2;

    physical_memory_register_debug_access_filter(
        0x08000000, memory_region_size(bank1), flash_debug_access_allowed, s);
    physical_memory_register_debug_access_filter(
        0x08100000, memory_region_size(bank2), flash_debug_access_allowed, s);
}

void gnw_h7b0_flash_r_set_initial_locked(GnwH7B0FlashRState *s, bool locked)
{
    s->initial_locked = locked;
    s->rdp_value = locked ? 0x55 : RDP_LEVEL0;
    s->rdp_initialized = false;
}

void gnw_h7b0_flash_r_set_rdp_image(GnwH7B0FlashRState *s,
                                    const char *image)
{
    g_free(s->rdp_image);
    s->rdp_image = g_strdup(image);
}

static void gnw_h7b0_flash_r_init(Object *obj)
{
    GnwH7B0FlashRState *s = GNW_H7B0_FLASH_R(obj);
    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_flash_r_ops, s, TYPE_GNW_H7B0_FLASH_R, GNW_H7B0_FLASH_R_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const VMStateDescription vmstate_gnw_h7b0_flash_r = {
    .name = TYPE_GNW_H7B0_FLASH_R,
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = gnw_h7b0_flash_r_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0FlashRState, GNW_H7B0_FLASH_R_SIZE / 4),
        VMSTATE_UINT8_V(rdp_value, GnwH7B0FlashRState, 2),
        VMSTATE_BOOL_V(rdp_initialized, GnwH7B0FlashRState, 2),
        VMSTATE_UINT8_V(optkey_stage, GnwH7B0FlashRState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_flash_r_unrealize(DeviceState *dev)
{
    physical_memory_unregister_debug_access_filters(GNW_H7B0_FLASH_R(dev));
}

static void gnw_h7b0_flash_r_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &vmstate_gnw_h7b0_flash_r;
    dc->unrealize = gnw_h7b0_flash_r_unrealize;
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
