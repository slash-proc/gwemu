/*
 * QEMU model of the STM32H7B0 MDMA (master DMA) controller.
 *
 * Was a plain create_unimplemented_device() stub -- fine for firmware that
 * never touches it, but stm32h7b0-diag's mdma_m2m case (HAL_MDMA_Start() +
 * HAL_MDMA_PollForTransfer(), MDMA_REQUEST_SW, single-block SW-triggered
 * memory-to-memory transfer) polls CISR.CTCIF forever since an
 * unimplemented-device stub never sets it, and even if it somehow did,
 * nothing would have actually copied the guest memory -- same underlying
 * gap as DMA1/DMA2's mem2mem streams before their own fix (see
 * gnw_h7b0_dma.c's gnw_h7b0_dma_do_m2m_copy()).
 *
 * Only real (16-channel) register layout + a synchronous SW-request
 * transfer is modeled: HAL's HAL_MDMA_Start() writes CCR.EN=1, then (for
 * MDMA_REQUEST_SW) ORs in CCR.SWRQ=1 in a second write. On that SWRQ 0->1
 * edge (with EN already set), perform the whole configured transfer
 * (CSAR->CDAR, CBNDTR.BNDT bytes, SINC/DINC + SSIZE/DSIZE from CTCR)
 * immediately and set every completion flag HAL_MDMA_PollForTransfer()
 * might poll for (CTCIF/BTIF/BRTIF/TCIF), clearing EN/SWRQ back to match
 * real hardware's "flag channel ready when read low" semantics. Only the
 * common equal-SSIZE/DSIZE case is implemented (the only one any known
 * firmware here uses); anything else logs unimplemented rather than
 * silently doing the wrong thing. Register-request-triggered and
 * linked-list transfers are not modeled -- no known firmware here uses
 * them.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "exec/cpu-common.h"
#include "hw/misc/gnw_h7b0_mdma.h"

#define MDMA_CHANNEL_BASE      0x40
#define MDMA_CHANNEL_STRIDE    0x40
#define MDMA_CHANNEL_COUNT     16

#define MDMA_CISR_OFF   0x00
#define MDMA_CIFCR_OFF  0x04
#define MDMA_CCR_OFF    0x0c
#define MDMA_CTCR_OFF   0x10
#define MDMA_CBNDTR_OFF 0x14
#define MDMA_CSAR_OFF   0x18
#define MDMA_CDAR_OFF   0x1c

#define MDMA_CCR_EN     (1U << 0)
#define MDMA_CCR_SWRQ   (1U << 16)

#define MDMA_CISR_CTCIF (1U << 1)
#define MDMA_CISR_BRTIF (1U << 2)
#define MDMA_CISR_BTIF  (1U << 3)
#define MDMA_CISR_TCIF  (1U << 4)

#define MDMA_CTCR_SINC_SHIFT   0
#define MDMA_CTCR_SINC_MASK    (0x3U << MDMA_CTCR_SINC_SHIFT)
#define MDMA_CTCR_DINC_SHIFT   2
#define MDMA_CTCR_DINC_MASK    (0x3U << MDMA_CTCR_DINC_SHIFT)
#define MDMA_CTCR_SSIZE_SHIFT  4
#define MDMA_CTCR_SSIZE_MASK   (0x3U << MDMA_CTCR_SSIZE_SHIFT)
#define MDMA_CTCR_DSIZE_SHIFT  6
#define MDMA_CTCR_DSIZE_MASK   (0x3U << MDMA_CTCR_DSIZE_SHIFT)
#define MDMA_CTCR_SINCOS_SHIFT 8
#define MDMA_CTCR_SINCOS_MASK  (0x3U << MDMA_CTCR_SINCOS_SHIFT)
#define MDMA_CTCR_DINCOS_SHIFT 10
#define MDMA_CTCR_DINCOS_MASK  (0x3U << MDMA_CTCR_DINCOS_SHIFT)

#define MDMA_CBNDTR_BNDT_MASK 0x1FFFFU

static void gnw_h7b0_mdma_do_transfer(GnwH7B0MdmaState *s, int ch)
{
    hwaddr base = MDMA_CHANNEL_BASE + (hwaddr)ch * MDMA_CHANNEL_STRIDE;
    uint32_t ctcr = s->regs[(base + MDMA_CTCR_OFF) >> 2];
    uint32_t bndt = s->regs[(base + MDMA_CBNDTR_OFF) >> 2] & MDMA_CBNDTR_BNDT_MASK;
    uint32_t csar = s->regs[(base + MDMA_CSAR_OFF) >> 2];
    uint32_t cdar = s->regs[(base + MDMA_CDAR_OFF) >> 2];
    uint32_t ssize = 1U << ((ctcr & MDMA_CTCR_SSIZE_MASK) >> MDMA_CTCR_SSIZE_SHIFT);
    uint32_t dsize = 1U << ((ctcr & MDMA_CTCR_DSIZE_MASK) >> MDMA_CTCR_DSIZE_SHIFT);
    bool sinc = ((ctcr & MDMA_CTCR_SINC_MASK) >> MDMA_CTCR_SINC_SHIFT) != 0;
    bool dinc = ((ctcr & MDMA_CTCR_DINC_MASK) >> MDMA_CTCR_DINC_SHIFT) != 0;
    uint32_t sstep = 1U << ((ctcr & MDMA_CTCR_SINCOS_MASK) >> MDMA_CTCR_SINCOS_SHIFT);
    uint32_t dstep = 1U << ((ctcr & MDMA_CTCR_DINCOS_MASK) >> MDMA_CTCR_DINCOS_SHIFT);

    if (ssize != dsize) {
        qemu_log_mask(LOG_UNIMP,
                      "gnw-h7b0-mdma: channel %d SSIZE(%u) != DSIZE(%u) "
                      "not implemented\n", ch, ssize, dsize);
        return;
    }

    for (uint32_t off = 0; off < bndt; off += ssize) {
        uint8_t buf[8];
        hwaddr src = csar + (sinc ? (hwaddr)(off / ssize) * sstep : 0);
        hwaddr dst = cdar + (dinc ? (hwaddr)(off / ssize) * dstep : 0);

        cpu_physical_memory_read(src, buf, ssize);
        cpu_physical_memory_write(dst, buf, dsize);
    }
}

static void gnw_h7b0_mdma_reset(DeviceState *dev)
{
    GnwH7B0MdmaState *s = GNW_H7B0_MDMA(dev);
    memset(s->regs, 0, sizeof(s->regs));
}

static uint64_t gnw_h7b0_mdma_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0MdmaState *s = GNW_H7B0_MDMA(opaque);
    if (addr >= GNW_H7B0_MDMA_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return 0;
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_mdma_write(void *opaque, hwaddr addr, uint64_t val64, unsigned int size)
{
    GnwH7B0MdmaState *s = GNW_H7B0_MDMA(opaque);
    if (addr >= GNW_H7B0_MDMA_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return;
    }

    if (addr >= MDMA_CHANNEL_BASE) {
        hwaddr rel = addr - MDMA_CHANNEL_BASE;
        int ch = (int)(rel / MDMA_CHANNEL_STRIDE);
        hwaddr ch_off = rel % MDMA_CHANNEL_STRIDE;

        if (ch < MDMA_CHANNEL_COUNT && ch_off == MDMA_CIFCR_OFF) {
            /* Write-1-to-clear against CISR (identical bit layout). */
            hwaddr cisr_off = MDMA_CHANNEL_BASE +
                               (hwaddr)ch * MDMA_CHANNEL_STRIDE + MDMA_CISR_OFF;
            s->regs[cisr_off >> 2] &= ~(uint32_t)val64;
            s->regs[addr >> 2] = (uint32_t)val64;
            return;
        }

        if (ch < MDMA_CHANNEL_COUNT && ch_off == MDMA_CCR_OFF) {
            uint32_t old_val = s->regs[addr >> 2];
            uint32_t new_val = (uint32_t)val64;
            s->regs[addr >> 2] = new_val;

            bool old_swrq = (old_val & MDMA_CCR_SWRQ) != 0;
            bool new_swrq = (new_val & MDMA_CCR_SWRQ) != 0;
            bool en = (new_val & MDMA_CCR_EN) != 0;

            if (en && new_swrq && !old_swrq) {
                gnw_h7b0_mdma_do_transfer(s, ch);

                hwaddr cisr_off = MDMA_CHANNEL_BASE +
                                   (hwaddr)ch * MDMA_CHANNEL_STRIDE + MDMA_CISR_OFF;
                s->regs[cisr_off >> 2] |= MDMA_CISR_CTCIF | MDMA_CISR_BTIF |
                                           MDMA_CISR_BRTIF | MDMA_CISR_TCIF;
                s->regs[addr >> 2] &= ~(MDMA_CCR_EN | MDMA_CCR_SWRQ);
            }
            return;
        }
    }

    s->regs[addr >> 2] = (uint32_t)val64;
}

static const MemoryRegionOps gnw_h7b0_mdma_ops = {
    .read = gnw_h7b0_mdma_read,
    .write = gnw_h7b0_mdma_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void gnw_h7b0_mdma_init(Object *obj)
{
    GnwH7B0MdmaState *s = GNW_H7B0_MDMA(obj);
    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_mdma_ops, s,
                           TYPE_GNW_H7B0_MDMA, GNW_H7B0_MDMA_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void gnw_h7b0_mdma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, gnw_h7b0_mdma_reset);
}

static const TypeInfo gnw_h7b0_mdma_info = {
    .name          = TYPE_GNW_H7B0_MDMA,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0MdmaState),
    .instance_init = gnw_h7b0_mdma_init,
    .class_init    = gnw_h7b0_mdma_class_init,
};

static void gnw_h7b0_mdma_register_types(void)
{
    type_register_static(&gnw_h7b0_mdma_info);
}

type_init(gnw_h7b0_mdma_register_types)
