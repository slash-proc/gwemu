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
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/misc/gnw_h7b0_spi.h"
#include "hw/misc/gnw_h7b0_regs_spi.h"
#include "hw/misc/gnw_h7b0_stub_log.h"
#include "hw/sd/sd.h"
#include "system/blockdev.h"

/*
 * TXDR completion used to be deferred by a small QEMUTimer-paced delay
 * here, meant to fix retro-go-sd's user_diskio_spi.c busy-wait loops
 * (SD_ReadyWait()/SD_RxDataBlock()/SD_SendCmd()) spinning through far
 * more iterations per wall-clock timeout window than real hardware could
 * (confirmed via live profiling: ~23,000 SPI1 register writes/sec during
 * ordinary SD-card gameplay reads, since instant completion meant each
 * "byte" cost only a few emulated instructions). That approach caused a
 * real regression -- a full guest hang, confirmed via live PC-sampling
 * (frozen in HAL_Delay, forward progress only resumed once completion
 * was reverted to synchronous) -- almost certainly because retro-go-sd's
 * poll loop here is tight enough that TCG never yields back to QEMU's
 * main loop to service the pending completion timer, so the guest spins
 * forever waiting on a flag that can only ever be set from a timer
 * callback it's preventing from running. Reverted to synchronous
 * completion (matching this file's original, working behavior) --
 * correctness/not-hanging trumps the CPU-usage optimization. If revisited,
 * needs a pacing strategy that doesn't require the guest CPU to yield
 * control on every single byte (e.g. block-level pacing instead of
 * per-byte, or a mechanism proven not to starve under a tight poll loop).
 */
static void gnw_h7b0_spi_reset(DeviceState *dev)
{
    GnwH7B0SpiState *s = GNW_H7B0_SPI(dev);

    for (int i = 0; i < (GNW_H7B0_SPI_SIZE / 4); i++) {
        s->regs[i] = get_spi_reset_value(i * 4);
    }
    memset(s->logged_unimp, 0, sizeof(s->logged_unimp));
    /*
     * TXP ("Tx-Packet space available") reflects TxFIFO occupancy on
     * real hardware, which is trivially available even before the
     * peripheral is enabled -- it is NOT gated on CR1.SPE. Found via
     * retro-go-sd's bare-metal user_diskio_spi.c: it polls SR.TXP
     * *before* HAL_SPI_Transmit() (the actual SPE-enabling call) ever
     * runs, so a TXP-only-set-after-SPE model left it spinning
     * forever on the very first byte. Set once at reset and never
     * cleared -- see the CR1 handler below.
     */
    s->regs[GNW_H7B0_SPI_SR >> 2] = SPI_SR_TXP;
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

    uint32_t mask = get_spi_write_mask(addr);
    value = (s->regs[addr >> 2] & ~mask) | (value & mask);

    switch (addr) {
    case GNW_H7B0_SPI_CR1:
        s->regs[addr >> 2] = value;
        if (!(value & SPI_CR1_SPE)) {
            /*
             * TXP is left alone (see reset's comment -- it's not
             * SPE-gated on real hardware); EOT and RXP are
             * disable-gated, matching real hardware clearing status
             * on disable.
             */
            s->regs[GNW_H7B0_SPI_SR >> 2] &= ~(SPI_SR_EOT | SPI_SR_RXP);
        }
        return;
    case GNW_H7B0_SPI_TXDR:
        /*
         * Real hardware only sets EOT once the whole configured
         * transfer finishes; approximated here as "every write instantly
         * completes the transfer" since nothing currently tracks a
         * configured byte count -- see this file's top-of-file comment
         * for why a deferred (paced) completion was tried and reverted.
         */
        s->regs[addr >> 2] = value;
        if (s->ssi) {
            /* Real SSI byte exchange with the attached virtual SD card. */
            uint32_t rx = ssi_transfer(s->ssi, value & 0xff);
            s->regs[GNW_H7B0_SPI_RXDR >> 2] = rx;
            /*
             * RXP must be set whenever a real byte lands in RXDR --
             * HAL_SPI_TransmitReceive() polls RXP (not EOT) before
             * reading each received byte. See gnw_h7b0_spi.h's file
             * comment for the "No SD Card found" bug this caused.
             */
            s->regs[GNW_H7B0_SPI_SR >> 2] |= SPI_SR_RXP;
        }
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
        gnw_h7b0_stub_log_unimp_ratelimited(s->logged_unimp, __func__, addr);
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

static void gnw_h7b0_spi_realize(DeviceState *dev, Error **errp)
{
    GnwH7B0SpiState *s = GNW_H7B0_SPI(dev);

    if (!s->sd_card) {
        return;
    }

    /*
     * Real SD-over-SPI card, backed by whatever -drive if=sd,... the
     * user passes (none => card model with no image => reads as
     * "ejected", same as before this device existed). See
     * gnw_h7b0_spi.h for why chip-select isn't wired up.
     */
    s->ssi = ssi_create_bus(dev, "ssi");
    DeviceState *ssi_sd_dev = ssi_create_peripheral(s->ssi, "ssi-sd");

    DriveInfo *dinfo = drive_get(IF_SD, 0, 0);
    BlockBackend *blk = dinfo ? blk_by_legacy_dinfo(dinfo) : NULL;
    DeviceState *card_dev = qdev_new(TYPE_SD_CARD_SPI);
    qdev_prop_set_drive_err(card_dev, "drive", blk, &error_fatal);
    qdev_realize_and_unref(card_dev, qdev_get_child_bus(ssi_sd_dev, "sd-bus"),
                            &error_fatal);
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

static Property gnw_h7b0_spi_properties[] = {
    DEFINE_PROP_BOOL("sd-card", GnwH7B0SpiState, sd_card, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void gnw_h7b0_spi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_spi;
    dc->realize = gnw_h7b0_spi_realize;
    device_class_set_legacy_reset(dc, gnw_h7b0_spi_reset);
    device_class_set_props(dc, gnw_h7b0_spi_properties);
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
