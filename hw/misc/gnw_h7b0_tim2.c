/* Auto-generated stub for TIM2, extended with a real counter for CR1.CEN */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/misc/gnw_h7b0_tim2.h"
#include "hw/misc/gnw_h7b0_regs_tim2.h"

/*
 * TIM2-TIM14's real kernel clock, not a guess: game-and-watch-retro-go-sd's
 * main.c SystemClock_Config sets AHBCLKDivider=RCC_HCLK_DIV1 (HCLK=SYSCLK,
 * no division) and APB1CLKDivider=RCC_APB1_DIV2 (PCLK1=HCLK/2) -- but
 * STM32H7's timer kernel clock uses the "x2" rule whenever its APBx
 * prescaler isn't /1: timer_clk = 2 x PCLK1 when APB1 prescaler > 1. That
 * doubling exactly cancels the /2, so TIM2's real kernel clock equals HCLK,
 * which (AHB_DIV1) equals SYSCLK.
 *
 * The real, LIVE value now comes from gnw_h7b0_rcc_get_hclk_hz() (see
 * gnw_h7b0_tim2_period_ns()) once gnw_h7b0_tim2_set_rcc() has wired an RCC
 * pointer in -- RCC decodes it from actual PLL1/SYSCLK register state, so
 * it tracks retro-go's runtime CPU overclock feature (NORMAL~280MHz/
 * BOOST1~312MHz/BOOST2~340MHz, switchable at runtime), rather than being
 * frozen at whichever level happened to be sampled once.
 *
 * The 340512000 constant below is now only a defensive fallback for when
 * RCC isn't wired (e.g. a standalone unit test) or returns 0 -- it's the
 * BOOST2-level SYSCLK value previously reverse-engineered from a live
 * SystemCoreClock read (see gnw_h7b0.c's SYSCLK_FRQ comment), kept as the
 * least-wrong single guess since it matches the already-validated ARMv7M
 * clock constant. Before this live wiring, this file's own guess had
 * predated that finding and silently mismatched it (a flat 200MHz), and
 * even after matching it, remained correct only at that one OC level --
 * any firmware game-speed pacing derived from a TIM2-TIM14 instance (not
 * just audio-DMA-paced code) would run at the wrong real-time rate at
 * NORMAL/BOOST1 or mid-switch otherwise.
 */
#define GNW_H7B0_TIM2_APPROX_CLK_HZ 340512000ULL
/* Clamp so a firmware-chosen PSC/ARR can't freeze the emulator for a
 * real-world eternity (e.g. ARR=0xffffffff at a slow prescaler) or
 * spin the host CPU rescheduling a timer for an ~instant period. */
#define GNW_H7B0_TIM2_MIN_PERIOD_NS 1000ULL
#define GNW_H7B0_TIM2_MAX_PERIOD_NS (2 * NANOSECONDS_PER_SECOND)

static int gnw_h7b0_tim2_instance_index(hwaddr addr)
{
    hwaddr instance_base = addr - (addr % 0x400);
    unsigned int idx = instance_base / 0x400;

    return idx < GNW_H7B0_TIM2_BLOCK_INSTANCE_COUNT ? (int)idx : -1;
}

static uint64_t gnw_h7b0_tim2_period_ns(GnwH7B0Tim2State *s, int idx)
{
    hwaddr base = (hwaddr)idx * 0x400;
    uint32_t psc = s->regs[(base + GNW_H7B0_TIM2_PSC_OFFSET) >> 2];
    uint32_t arr = s->regs[(base + GNW_H7B0_TIM2_ARR_OFFSET) >> 2];
    uint64_t clk_hz = GNW_H7B0_TIM2_APPROX_CLK_HZ;
    uint64_t period_ns;

    if (s->rcc) {
        uint32_t live_hz = gnw_h7b0_rcc_get_hclk_hz(s->rcc);
        if (live_hz != 0) {
            clk_hz = live_hz;
        }
    }

    period_ns = (uint64_t)(arr + 1) * (psc + 1)
                * NANOSECONDS_PER_SECOND / clk_hz;

    period_ns = MAX(period_ns, GNW_H7B0_TIM2_MIN_PERIOD_NS);
    period_ns = MIN(period_ns, GNW_H7B0_TIM2_MAX_PERIOD_NS);
    return period_ns;
}

static void gnw_h7b0_tim2_start_counting(GnwH7B0Tim2State *s, int idx)
{
    timer_mod(s->count_timer[idx],
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              gnw_h7b0_tim2_period_ns(s, idx));
}

static void gnw_h7b0_tim2_timer_tick(void *opaque)
{
    GnwH7B0Tim2TimerCtx *ctx = opaque;
    GnwH7B0Tim2State *s = ctx->s;
    int idx = ctx->idx;
    hwaddr base = (hwaddr)idx * 0x400;
    uint32_t cr1 = s->regs[(base + GNW_H7B0_TIM2_CR1_OFFSET) >> 2];

    s->regs[(base + GNW_H7B0_TIM2_SR_OFFSET) >> 2] |= 0x1u; /* UIF */

    if (cr1 & (1u << 3)) {
        /* OPM (one-pulse mode): counter stops itself after the update
         * event, same as real hardware. */
        s->regs[(base + GNW_H7B0_TIM2_CR1_OFFSET) >> 2] = cr1 & ~0x1u;
        return;
    }

    if (cr1 & 0x1u) {
        gnw_h7b0_tim2_start_counting(s, idx);
    }
}

void gnw_h7b0_tim2_set_rcc(GnwH7B0Tim2State *s, GnwH7B0RccState *rcc)
{
    s->rcc = rcc;
}

static void gnw_h7b0_tim2_reset(DeviceState *dev)
{
    GnwH7B0Tim2State *s = GNW_H7B0_TIM2(dev);
    for (int i = 0; i < (GNW_H7B0_TIM2_SIZE / 4); i++) {
        s->regs[i] = get_tim2_reset_value(i * 4);
    }
    for (int i = 0; i < GNW_H7B0_TIM2_BLOCK_INSTANCE_COUNT; i++) {
        timer_del(s->count_timer[i]);
    }
}

static uint64_t gnw_h7b0_tim2_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0Tim2State *s = GNW_H7B0_TIM2(opaque);
    if (addr >= GNW_H7B0_TIM2_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return 0;
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_tim2_write(void *opaque, hwaddr addr, uint64_t val64, unsigned int size)
{
    GnwH7B0Tim2State *s = GNW_H7B0_TIM2(opaque);
    if (addr >= GNW_H7B0_TIM2_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return;
    }
    uint32_t mask = get_tim2_write_mask(addr);
    uint32_t old_value = s->regs[addr >> 2];
    s->regs[addr >> 2] = (old_value & ~mask) | ((uint32_t)val64 & mask);

    /*
     * This device models the whole TIM2-TIM14 block (see soc.h's
     * TIM2_BLOCK_BASE_ADDRESS/SIZE) as one contiguous region, but is
     * otherwise a plain shadow with no side effects -- fine for most
     * firmware, but a real hardware-timer busy-wait idiom (write EGR.UG=1
     * to force an update event, then spin on SR.UIF until hardware sets
     * it) never terminates here, since nothing ever sets UIF. Found via a
     * silent black-screen hang tracing a patched stock-firmware image
     * with no debug symbols (disassembly-only: r0=0x40000C00 == TIM5,
     * spinning on SR bit0 after an EGR.UG write). TIM2/3/4/5/6/7 share a
     * uniform 0x400-per-instance layout with EGR at local offset 0x14 and
     * SR at local offset 0x10 (general-purpose/basic timer register map);
     * apply the EGR.UG -> SR.UIF side effect generically at that stride so
     * it covers every timer in the block, not just whichever one this was
     * found on. (TIM12/13/14 don't sit on that same 0x400 grid within
     * this block; not covered by this generic rule -- fix those the same
     * way if a similar hang is found on one of them.)
     */
    if ((addr & 0x3ffu) == GNW_H7B0_TIM2_EGR_OFFSET && (val64 & 0x1u)) {
        uint32_t sr_addr = (addr & ~0x3ffu) + GNW_H7B0_TIM2_SR_OFFSET;
        s->regs[sr_addr >> 2] |= 0x1u; /* UIF */
    }

    /*
     * CR1.CEN (bit 0): real hardware starts counting from CNT toward
     * ARR (at PSC's rate) and sets SR.UIF on overflow, repeating
     * unless one-pulse mode (CR1.OPM, bit 3) is set. Previously a
     * plain shadow -- any firmware busy-waiting on SR.UIF after
     * enabling CEN (rather than forcing an EGR.UG update, the only
     * path with a side effect) would hang forever. Approximated via
     * gnw_h7b0_tim2_start_counting()'s QEMUTimer instead of instant
     * completion, so timing-dependent pacing (e.g. a real per-frame
     * delay) gets a plausible real duration rather than either
     * hanging or running unbounded-fast.
     */
    if ((addr & 0x3ffu) == GNW_H7B0_TIM2_CR1_OFFSET) {
        int idx = gnw_h7b0_tim2_instance_index(addr);
        uint32_t new_cr1 = s->regs[addr >> 2];

        if (idx >= 0) {
            if ((new_cr1 & 0x1u) && !(old_value & 0x1u)) {
                gnw_h7b0_tim2_start_counting(s, idx);
            } else if (!(new_cr1 & 0x1u)) {
                timer_del(s->count_timer[idx]);
            }
        }
    }
}

static const MemoryRegionOps gnw_h7b0_tim2_ops = {
    .read = gnw_h7b0_tim2_read,
    .write = gnw_h7b0_tim2_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void gnw_h7b0_tim2_init(Object *obj)
{
    GnwH7B0Tim2State *s = GNW_H7B0_TIM2(obj);
    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_tim2_ops, s, TYPE_GNW_H7B0_TIM2, GNW_H7B0_TIM2_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);

    for (int i = 0; i < GNW_H7B0_TIM2_BLOCK_INSTANCE_COUNT; i++) {
        s->timer_ctx[i].s = s;
        s->timer_ctx[i].idx = i;
        s->count_timer[i] = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                          gnw_h7b0_tim2_timer_tick,
                                          &s->timer_ctx[i]);
    }
}

static const VMStateDescription vmstate_gnw_h7b0_tim2 = {
    .name = TYPE_GNW_H7B0_TIM2,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0Tim2State, GNW_H7B0_TIM2_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_tim2_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &vmstate_gnw_h7b0_tim2;
    device_class_set_legacy_reset(dc, gnw_h7b0_tim2_reset);
}

static const TypeInfo gnw_h7b0_tim2_info = {
    .name          = TYPE_GNW_H7B0_TIM2,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0Tim2State),
    .instance_init = gnw_h7b0_tim2_init,
    .class_init    = gnw_h7b0_tim2_class_init,
};

static void gnw_h7b0_tim2_register_types(void)
{
    type_register_static(&gnw_h7b0_tim2_info);
}
type_init(gnw_h7b0_tim2_register_types)
