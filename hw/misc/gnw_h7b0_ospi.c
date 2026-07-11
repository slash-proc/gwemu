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
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/misc/gnw_h7b0_ospi.h"
#include "hw/misc/gnw_h7b0_regs_ospi.h"
#include "hw/misc/gnw_h7b0_stub_log.h"

static void gnw_h7b0_ospi_reset(DeviceState *dev)
{
    GnwH7B0OspiState *s = GNW_H7B0_OSPI(dev);

    for (int i = 0; i < (GNW_H7B0_OSPI_SIZE / 4); i++) {
        s->regs[i] = get_ospi_reset_value(i * 4);
    }
    memset(s->logged_unimp, 0, sizeof(s->logged_unimp));
    s->pending_instr = 0;
    s->pending_addr = 0;
    s->dr_pos = 0;
    s->wel = false;
}

/*
 * Byte generator for SFDP reads: a minimal one-parameter-header SFDP
 * table (signature + basic flash parameter table) whose only field
 * OSPI_GetFlashSizeSfdp() (gw_flash.c) actually reads is the density
 * dword at basic-table offset 4, from which it derives flash size.
 * `off` is an absolute byte offset into this synthetic SFDP address
 * space, matching how real SFDP reads are addressed (CMD_SFDP's
 * address phase carries the offset).
 */
static uint8_t ospi_sfdp_byte(GnwH7B0OspiState *s, uint32_t off)
{
    static const char sig[4] = "SFDP";
    uint32_t density_bits;

    if (off < 4) {
        return sig[off];
    }
    switch (off) {
    case 4: return 0x06; /* SFDP minor revision */
    case 5: return 0x01; /* SFDP major revision */
    case 6: return 0x00; /* NPH: 1 parameter header (0-based) */
    case 7: return 0xFF; /* unused */
    case 8: return 0x00; /* basic table JEDEC ID LSB (0xFF00) */
    case 9: return 0x06; /* basic table minor revision */
    case 10: return 0x01; /* basic table major revision */
    case 11: return 0x09; /* basic table length, in DWORDs */
    case 12: return OSPI_SFDP_TABLE_PTR & 0xFF;
    case 13: return (OSPI_SFDP_TABLE_PTR >> 8) & 0xFF;
    case 14: return (OSPI_SFDP_TABLE_PTR >> 16) & 0xFF;
    case 15: return 0xFF; /* basic table JEDEC ID MSB */
    default:
        break;
    }

    if (off >= OSPI_SFDP_TABLE_PTR && off < OSPI_SFDP_TABLE_PTR + 9 * 4) {
        uint32_t rel = off - OSPI_SFDP_TABLE_PTR;

        /* Basic Flash Parameter Table DWORD 2 = density, in bits - 1. */
        density_bits = (uint32_t)(s->flash_size * 8 - 1);
        switch (rel) {
        case 4: return density_bits & 0xFF;
        case 5: return (density_bits >> 8) & 0xFF;
        case 6: return (density_bits >> 16) & 0xFF;
        case 7: return (density_bits >> 24) & 0xFF;
        default:
            return 0;
        }
    }

    return 0;
}

/* Byte generator for the current indirect-read command's data phase. */
static uint8_t ospi_gen_read_byte(GnwH7B0OspiState *s, uint32_t pos)
{
    switch (s->pending_instr) {
    case OSPI_CMD_RDID:
        switch (pos) {
        case 0: return OSPI_FLASH_JEDEC_MANUF;
        case 1: return OSPI_FLASH_JEDEC_TYPE;
        case 2: return OSPI_FLASH_JEDEC_DENS;
        default: return 0;
        }
    case OSPI_CMD_RDSR:
        return OSPI_FLASH_SR_QE | (s->wel ? OSPI_FLASH_SR_WEL : 0);
    case OSPI_CMD_RDCR:
        return 0;
    case OSPI_CMD_SFDP:
        return ospi_sfdp_byte(s, s->pending_addr + pos);
    case OSPI_CMD_READ: {
        uint64_t off = s->pending_addr + pos;

        if (s->backing && off < s->flash_size) {
            return s->backing[off];
        }
        return 0xFF;
    }
    default:
        qemu_log_mask(LOG_UNIMP,
                       "%s: no real data-phase model for instruction 0x%02x, "
                       "returning zero\n", __func__, s->pending_instr);
        return 0;
    }
}

/*
 * Expected CCR ADSIZE per address-phase command, per gw_flash.c's
 * cmds_quad_32b_mx[] (this device's synthetic identity -- see
 * gnw_h7b0_ospi.h). Erase/PP/READ are the "32b" 4-byte-address quad
 * commands the table name implies, but SFDP is *not*: cmds_quad_32b_mx's
 * CMD_SFDP entry is still ADDR_SIZE_24B (SFDP addressing is small and
 * conventionally 24-bit even on 32-bit-address flash). Commands not
 * listed here (RDID/RDSR/RDCR/WREN/RSTEN/RST/CE) have no address phase
 * at all (addr_lines == LINES_0 in gw_flash.c), so ADSIZE is meaningless
 * for them and they're intentionally absent.
 */
static bool ospi_expected_adsize(uint8_t instr, uint32_t *adsize)
{
    switch (instr) {
    case OSPI_CMD_SFDP:
        *adsize = OSPI_ADSIZE_24B;
        return true;
    case OSPI_CMD_SE:
    case OSPI_CMD_BE32K:
    case OSPI_CMD_BE64K:
    case OSPI_CMD_PP:
    case OSPI_CMD_READ:
        *adsize = OSPI_ADSIZE_32B;
        return true;
    default:
        return false;
    }
}

/* Erase `size` bytes at `addr` in the XIP backing store, if wired up. */
static void ospi_backing_erase(GnwH7B0OspiState *s, uint32_t addr,
                                uint64_t size)
{
    if (!s->backing || addr >= s->flash_size) {
        return;
    }
    if (size > s->flash_size - addr) {
        size = s->flash_size - addr;
    }
    memset(s->backing + addr, 0xFF, size);
}

/*
 * Side effects of commands that have no data phase at all (WREN/
 * RSTEN/RST/write-enable-gated erase & program), applied the moment
 * the command is triggered -- since indirect-write commands with a
 * data phase still see their DR bytes silently dropped (no backing
 * store unification with the memory-mapped XIP region yet, see
 * gnw_h7b0_ospi.h), it's simplest to just clear WEL here too for any
 * instruction that real hardware would treat as consuming it.
 */
static void ospi_cmd_triggered(GnwH7B0OspiState *s)
{
    switch (s->pending_instr) {
    case OSPI_CMD_WREN:
        s->wel = true;
        return;
    case OSPI_CMD_RDID:
    case OSPI_CMD_RDSR:
    case OSPI_CMD_RDCR:
    case OSPI_CMD_SFDP:
    case OSPI_CMD_RSTEN:
    case OSPI_CMD_RST:
        return;
    case OSPI_CMD_CE:
        ospi_backing_erase(s, 0, s->flash_size);
        s->wel = false;
        return;
    default:
        /* Erase/program/WRSR-class commands consume WEL. */
        s->wel = false;
        return;
    }
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

    if (addr == GNW_H7B0_OSPI_DR) {
        /*
         * HAL_OSPI_Receive() reads DR one byte at a time (see
         * gnw_h7b0_ospi.h); real hardware auto-increments through the
         * FIFO/data-phase per access regardless of the access size
         * used, so just always hand back the next byte here too.
         *
         * DLR holds NbData-1 (see stm32h7xx_hal_ospi.c's "DLR =
         * NbData - 1U" in both HAL_OSPI_Command() and the DMA path),
         * so the transfer covers exactly DLR+1 bytes; well-behaved
         * firmware never reads past that (it polls TCF/checks
         * XferCount, both already satisfied by the AR/IR write
         * handler below setting TCF instantly). Bound dr_pos anyway
         * so a guest that ignores status and keeps reading DR past
         * its own configured length gets a flat 0xFF (idle-bus/
         * erased-flash filler) instead of the byte generator quietly
         * running off the end of whatever pending_instr's data phase
         * actually means (e.g. re-reading stale RDID/SFDP bytes).
         */
        uint32_t dlr = s->regs[GNW_H7B0_OSPI_DLR >> 2];

        if (s->dr_pos > dlr) {
            return 0xFF;
        }
        uint8_t b = ospi_gen_read_byte(s, s->dr_pos);
        s->dr_pos++;
        return b;
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

    uint32_t mask = get_ospi_write_mask(addr);
    value = (s->regs[addr >> 2] & ~mask) | (value & mask);

    switch (addr) {
    case GNW_H7B0_OSPI_IR:
    case GNW_H7B0_OSPI_AR:
        /*
         * Real hardware starts the transaction once the command is
         * fully configured -- IR alone for HAL_OSPI_Command()'s
         * no-data / no-address path, or AR/IR (re-written by
         * HAL_OSPI_Receive()/HAL_OSPI_Transmit() themselves, after
         * HAL_OSPI_Command() already configured but didn't start the
         * transfer) for the data phase, per whichever of AR/IR
         * corresponds to the command's address mode. Latch the
         * instruction/address for the data-phase byte generator
         * below, apply any no-data-phase side effects (WREN/etc)
         * immediately, and instantly report the command as complete
         * (TCF+FTF set, BUSY clear) so every polling loop -- the
         * command's own, and each subsequent per-byte DR access --
         * succeeds right away.
         */
        s->regs[addr >> 2] = value;
        if (addr == GNW_H7B0_OSPI_IR) {
            s->pending_instr = value & 0xFF;
        }
        if (addr == GNW_H7B0_OSPI_AR) {
            s->pending_addr = value;
            /*
             * pending_addr is used as a plain integer offset below
             * regardless of address width (AR is a real 32-bit register
             * on actual hardware too -- ADSIZE only shapes how many
             * bytes get clocked out over the wire, not AR's numeric
             * contents), so there's no width-dependent decode needed
             * here. Still, verify firmware actually configured the
             * ADSIZE this device's synthetic identity is documented to
             * use, so a future flash-detection change picking a
             * different command table (e.g. 24-bit addressing) becomes
             * a visible log instead of a silent misbehavior.
             */
            uint32_t expected_adsize, actual_adsize;

            if (ospi_expected_adsize(s->pending_instr, &expected_adsize)) {
                actual_adsize = (s->regs[GNW_H7B0_OSPI_CCR >> 2]
                                  & OSPI_CCR_ADSIZE_MASK)
                                 >> OSPI_CCR_ADSIZE_SHIFT;
                if (actual_adsize != expected_adsize) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "%s: instruction 0x%02x configured CCR "
                                  "ADSIZE=%u, expected %u for this "
                                  "device's synthetic flash identity -- "
                                  "address-phase decoding may be wrong\n",
                                  __func__, s->pending_instr,
                                  actual_adsize, expected_adsize);
                }
            }
            /*
             * Sector/block erase have an address phase but no data
             * phase -- real hardware erases the moment the address is
             * latched, so do the same here (chip erase has no address
             * phase at all; that's handled in ospi_cmd_triggered()
             * on the IR-only trigger instead).
             */
            switch (s->pending_instr) {
            case OSPI_CMD_SE:
                ospi_backing_erase(s, s->pending_addr, 4 * 1024);
                break;
            case OSPI_CMD_BE32K:
                ospi_backing_erase(s, s->pending_addr, 32 * 1024);
                break;
            case OSPI_CMD_BE64K:
                ospi_backing_erase(s, s->pending_addr, 64 * 1024);
                break;
            default:
                break;
            }
        }
        s->dr_pos = 0;
        ospi_cmd_triggered(s);
        s->regs[GNW_H7B0_OSPI_SR >> 2] |= OSPI_SR_TCF | OSPI_SR_FTF;
        s->regs[GNW_H7B0_OSPI_SR >> 2] &= ~OSPI_SR_BUSY;
        return;
    case GNW_H7B0_OSPI_DR:
        /*
         * HAL_OSPI_Transmit() writes DR one byte at a time. For page
         * program, persist each byte to the XIP backing store at
         * pending_addr+dr_pos (mirrors ospi_gen_read_byte()'s
         * addressing for indirect reads); any other write-class
         * command (e.g. WRSR) still has its byte dropped, since only
         * PP is modeled with a real data-phase effect.
         */
        if (s->pending_instr == OSPI_CMD_PP && s->backing) {
            uint64_t off = s->pending_addr + s->dr_pos;

            if (off < s->flash_size) {
                s->backing[off] = (uint8_t)value;
            }
        }
        s->dr_pos++;
        return;
    case GNW_H7B0_OSPI_CCR:
        s->regs[addr >> 2] = value;
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
        gnw_h7b0_stub_log_unimp_ratelimited(s->logged_unimp, __func__, addr);
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

void gnw_h7b0_ospi_set_backing(GnwH7B0OspiState *s, void *backing)
{
    s->backing = backing;
}

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
        VMSTATE_UINT8(pending_instr, GnwH7B0OspiState),
        VMSTATE_UINT32(pending_addr, GnwH7B0OspiState),
        VMSTATE_UINT32(dr_pos, GnwH7B0OspiState),
        VMSTATE_BOOL(wel, GnwH7B0OspiState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property gnw_h7b0_ospi_properties[] = {
    DEFINE_PROP_UINT64("flash-size", GnwH7B0OspiState, flash_size,
                        OSPI_FLASH_DEFAULT_SIZE),
};

static void gnw_h7b0_ospi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_ospi;
    device_class_set_legacy_reset(dc, gnw_h7b0_ospi_reset);
    device_class_set_props(dc, gnw_h7b0_ospi_properties);
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
