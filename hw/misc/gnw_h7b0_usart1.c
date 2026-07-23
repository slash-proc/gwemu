/*
 * STM32H7B0 USART1 -- minimal transmit-only model.
 *
 * Scope: a register shadow plus two pieces of real behaviour, and nothing
 * else.
 *
 *  - ISR reads always report the transmitter idle/ready flags (TXE/TXFNF, TC,
 *    TXFE) and the enable-acknowledge flags (TEACK/REACK). A real UART clears
 *    TXE while a character is shifting out; modelling that would require a
 *    baud-rate timer for no benefit here, since the only consumer is a
 *    blocking putc that just wants "ready".
 *  - TDR writes are pushed to a chardev, so guest printf output shows up on
 *    QEMU's serial console (-serial stdio / -serial file:...).
 *
 * Receive is not modelled at all: RXNE never sets, RDR always reads 0.
 *
 * Why this exists rather than staying a create_unimplemented_device(): the
 * retro-go porting toolkit's test firmware (used by the gnw-doom homebrew
 * port) makes USART1 its printf console and hangs at the first character in
 * `while (!(USART1->ISR & USART_ISR_TXE));` -- reads of a log-only stub
 * return 0, so TXE never sets. That is exactly the "promote it out of the
 * not-modeled list" trigger docs/peripheral-coverage.md describes.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/misc/gnw_h7b0_usart1.h"

#define USART_ISR   0x1c
#define USART_ICR   0x20
#define USART_RDR   0x24
#define USART_TDR   0x28

/* IDLE | TC | TXE(TXFNF) | TEACK | REACK | TXFE */
#define USART_ISR_STATIC_FLAGS  (0x10 | 0x40 | 0x80 | (1u << 21) | \
                                 (1u << 22) | (1u << 23))

static void gnw_h7b0_usart1_reset(DeviceState *dev)
{
    GnwH7B0Usart1State *s = GNW_H7B0_USART1(dev);
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[USART_ISR >> 2] = USART_ISR_STATIC_FLAGS;
}

static uint64_t gnw_h7b0_usart1_read(void *opaque, hwaddr addr,
                                     unsigned int size)
{
    GnwH7B0Usart1State *s = GNW_H7B0_USART1(opaque);

    if (addr >= GNW_H7B0_USART1_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }
    if (addr == USART_ISR) {
        return s->regs[addr >> 2] | USART_ISR_STATIC_FLAGS;
    }
    if (addr == USART_RDR) {
        return 0;
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_usart1_write(void *opaque, hwaddr addr, uint64_t val64,
                                  unsigned int size)
{
    GnwH7B0Usart1State *s = GNW_H7B0_USART1(opaque);
    uint32_t val = (uint32_t)val64;

    if (addr >= GNW_H7B0_USART1_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    switch (addr) {
    case USART_TDR: {
        uint8_t ch = val & 0xff;
        /* Blocking-safe: the guest already believes the byte is gone. */
        qemu_chr_fe_write_all(&s->chr, &ch, 1);
        break;
    }
    case USART_ICR:
        s->regs[USART_ISR >> 2] &= ~val;
        break;
    case USART_ISR:
        /* Read-only on real hardware. */
        break;
    default:
        s->regs[addr >> 2] = val;
        break;
    }
}

static const MemoryRegionOps gnw_h7b0_usart1_ops = {
    .read = gnw_h7b0_usart1_read,
    .write = gnw_h7b0_usart1_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void gnw_h7b0_usart1_init(Object *obj)
{
    GnwH7B0Usart1State *s = GNW_H7B0_USART1(obj);

    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_usart1_ops, s,
                          TYPE_GNW_H7B0_USART1, GNW_H7B0_USART1_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const VMStateDescription vmstate_gnw_h7b0_usart1 = {
    .name = TYPE_GNW_H7B0_USART1,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0Usart1State,
                             GNW_H7B0_USART1_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static const Property gnw_h7b0_usart1_properties[] = {
    DEFINE_PROP_CHR("chardev", GnwH7B0Usart1State, chr),
};

static void gnw_h7b0_usart1_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_usart1;
    device_class_set_props(dc, gnw_h7b0_usart1_properties);
    device_class_set_legacy_reset(dc, gnw_h7b0_usart1_reset);
}

static const TypeInfo gnw_h7b0_usart1_info = {
    .name          = TYPE_GNW_H7B0_USART1,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0Usart1State),
    .instance_init = gnw_h7b0_usart1_init,
    .class_init    = gnw_h7b0_usart1_class_init,
};

static void gnw_h7b0_usart1_register_types(void)
{
    type_register_static(&gnw_h7b0_usart1_info);
}
type_init(gnw_h7b0_usart1_register_types)
