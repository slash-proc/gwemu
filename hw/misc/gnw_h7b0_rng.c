/*
 * QEMU model of the STM32H7B0 RNG (true random number generator).
 *
 * Was a plain create_unimplemented_device() stub -- fine for firmware that
 * never touches it, but stm32h7b0-diag's crypto_rng_sanity/
 * crypto_rng_seed_error cases (raw CMSIS register access, no vendored
 * HAL RNG driver in this project) poll RNG_SR.DRDY forever since an
 * unimplemented-device stub never sets it, and even if it somehow did,
 * RNG_DR always reading 0 would fail crypto_rng_sanity's "not all
 * identical / not a constant stride" liveness check anyway.
 *
 * Only the 3 registers (CR/SR/DR) any known firmware here touches are
 * modeled, and only synchronously/instantaneously -- real hardware takes
 * some seed-generation cycles after RNGEN is set before DRDY first goes
 * high, and again some cycles between successive draws, neither of which
 * any known firmware here depends on (both diag cases use
 * iteration-bounded poll loops that tolerate immediate readiness fine).
 * CONDRST (conditioning reset, used by crypto_rng_seed_error's recovery-
 * sequence liveness check) is accepted but has no observable effect here
 * beyond DRDY staying set -- this model never generates the CECS/SECS
 * clock-/seed-error conditions CONDRST exists to recover from, so there
 * is nothing for it to actually reset.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/misc/gnw_h7b0_rng.h"

#define RNG_CR_OFFSET 0x00
#define RNG_SR_OFFSET 0x04
#define RNG_DR_OFFSET 0x08

#define RNG_CR_RNGEN (1U << 2)
#define RNG_CR_CONDRST (1U << 30)

#define RNG_SR_DRDY (1U << 0)
#define RNG_SR_CECS (1U << 1)
#define RNG_SR_SECS (1U << 2)
#define RNG_SR_CEIS (1U << 5)
#define RNG_SR_SEIS (1U << 6)
/* Write-1-to-clear bits within SR; DRDY/CECS/SECS are read-only status. */
#define RNG_SR_W1C_MASK (RNG_SR_CEIS | RNG_SR_SEIS)

static void gnw_h7b0_rng_reset(DeviceState *dev)
{
    GnwH7B0RngState *s = GNW_H7B0_RNG(dev);
    s->cr = 0;
    s->sr = 0;
}

static uint64_t gnw_h7b0_rng_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0RngState *s = GNW_H7B0_RNG(opaque);

    switch (addr) {
    case RNG_CR_OFFSET:
        return s->cr;
    case RNG_SR_OFFSET:
        return s->sr;
    case RNG_DR_OFFSET:
        /* Real hardware clears DRDY until the next word is ready; this
         * synchronous model has the next word ready immediately as long
         * as RNGEN is still on. */
        return (s->cr & RNG_CR_RNGEN) ? g_random_int() : 0;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return 0;
    }
}

static void gnw_h7b0_rng_write(void *opaque, hwaddr addr, uint64_t val64, unsigned int size)
{
    GnwH7B0RngState *s = GNW_H7B0_RNG(opaque);
    uint32_t val = (uint32_t)val64;

    switch (addr) {
    case RNG_CR_OFFSET:
        s->cr = val & ~RNG_CR_CONDRST; /* CONDRST self-clears (pulse). */
        s->sr = (s->cr & RNG_CR_RNGEN) ? RNG_SR_DRDY : 0;
        break;
    case RNG_SR_OFFSET:
        s->sr &= ~(val & RNG_SR_W1C_MASK);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        break;
    }
}

static const MemoryRegionOps gnw_h7b0_rng_ops = {
    .read = gnw_h7b0_rng_read,
    .write = gnw_h7b0_rng_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void gnw_h7b0_rng_init(Object *obj)
{
    GnwH7B0RngState *s = GNW_H7B0_RNG(obj);
    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_rng_ops, s,
                           TYPE_GNW_H7B0_RNG, GNW_H7B0_RNG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void gnw_h7b0_rng_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, gnw_h7b0_rng_reset);
}

static const TypeInfo gnw_h7b0_rng_info = {
    .name          = TYPE_GNW_H7B0_RNG,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0RngState),
    .instance_init = gnw_h7b0_rng_init,
    .class_init    = gnw_h7b0_rng_class_init,
};

static void gnw_h7b0_rng_register_types(void)
{
    type_register_static(&gnw_h7b0_rng_info);
}

type_init(gnw_h7b0_rng_register_types)
