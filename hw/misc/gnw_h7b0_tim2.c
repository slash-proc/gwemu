/* Auto-generated stub for TIM2, extended with a real counter for CR1.CEN */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/misc/gnw_h7b0_tim2.h"
#include "hw/misc/gnw_h7b0_regs_tim2.h"
#include "hw/misc/gnw_env.h"

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
        uint32_t live_hz = gnw_h7b0_rcc_get_timer_ker_hz(s->rcc);
        if (live_hz != 0) {
            clk_hz = live_hz;
        }
    }

    /*
     * Widen BEFORE incrementing. `arr` is uint32_t, so a plain (arr + 1)
     * is evaluated in 32-bit and wraps to 0 for the single most important
     * case there is: ARR = 0xffffffff, which is both the reset value and
     * exactly how firmware sets up a free-running 32-bit microsecond time
     * base (real example, stock Mario: CR1=1, PSC=0x15, ARR=0xffffffff).
     * The old (uint64_t)(arr + 1) cast the *already-wrapped* result, making
     * period_ns 0, which the MIN clamp below then turned into 1us -- a
     * ~277-second update event modelled as a 1MHz one. The timer re-armed
     * itself as fast as the main loop could dispatch it (~130-180k times a
     * second, measured), and that flood of virtual-clock expiries, not any
     * per-instruction emulation cost, was what saturated QEMU's main loop.
     *
     * The product needs a 128-bit intermediate too: (2^32) * 22 * 1e9
     * overflows uint64_t, so muldiv64() rather than a bare multiply.
     */
    period_ns = muldiv64(((uint64_t)arr + 1) * ((uint64_t)psc + 1),
                         NANOSECONDS_PER_SECOND, clk_hz);

    period_ns = MAX(period_ns, GNW_H7B0_TIM2_MIN_PERIOD_NS);
    period_ns = MIN(period_ns, GNW_H7B0_TIM2_MAX_PERIOD_NS);
    return period_ns;
}

/*
 * Counter tick rate for this instance: kernel clock after the prescaler.
 * Same clock source gnw_h7b0_tim2_period_ns() uses, kept separate so the
 * CNT extrapolation below doesn't have to re-derive the update period.
 */
static uint64_t gnw_h7b0_tim2_tick_hz(GnwH7B0Tim2State *s, int idx)
{
    hwaddr base = (hwaddr)idx * 0x400;
    uint32_t psc = s->regs[(base + GNW_H7B0_TIM2_PSC_OFFSET) >> 2];
    uint64_t clk_hz = GNW_H7B0_TIM2_APPROX_CLK_HZ;

    if (s->rcc) {
        uint32_t live_hz = gnw_h7b0_rcc_get_timer_ker_hz(s->rcc);
        if (live_hz != 0) {
            clk_hz = live_hz;
        }
    }
    return clk_hz / ((uint64_t)psc + 1);
}

/*
 * CNT as it would read *right now*: cnt_base plus however many ticks have
 * elapsed since cnt_ref_ns, wrapped at ARR+1. While CR1.CEN is clear the
 * counter is frozen, so cnt_base is the answer verbatim.
 */
static uint32_t gnw_h7b0_tim2_live_cnt(GnwH7B0Tim2State *s, int idx)
{
    hwaddr base = (hwaddr)idx * 0x400;
    uint32_t cr1 = s->regs[(base + GNW_H7B0_TIM2_CR1_OFFSET) >> 2];
    uint32_t arr = s->regs[(base + GNW_H7B0_TIM2_ARR_OFFSET) >> 2];
    uint64_t modulus = (uint64_t)arr + 1;
    uint64_t now, elapsed_ns, ticks;

    if (!(cr1 & 0x1u) || modulus == 0) {
        return s->cnt_base[idx];
    }

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (now <= s->cnt_ref_ns[idx]) {
        return s->cnt_base[idx];
    }
    elapsed_ns = now - s->cnt_ref_ns[idx];

    /* ticks = elapsed_ns * tick_hz / 1e9, in 128-bit to avoid overflow. */
    ticks = muldiv64(elapsed_ns, gnw_h7b0_tim2_tick_hz(s, idx),
                     NANOSECONDS_PER_SECOND);

    return (uint32_t)(((uint64_t)s->cnt_base[idx] + ticks) % modulus);
}

/*
 * Pin CNT's extrapolation to "now". Must be called BEFORE changing
 * anything the extrapolation depends on (PSC, ARR, CEN, CNT itself),
 * so the elapsed time so far is accounted against the old parameters.
 */
static void gnw_h7b0_tim2_latch_cnt(GnwH7B0Tim2State *s, int idx)
{
    s->cnt_base[idx] = gnw_h7b0_tim2_live_cnt(s, idx);
    s->cnt_ref_ns[idx] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void gnw_h7b0_tim2_start_counting(GnwH7B0Tim2State *s, int idx)
{
    timer_mod(s->count_timer[idx],
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              gnw_h7b0_tim2_period_ns(s, idx));
}

/* Env-gated (GNW_TIMER_LATE): per-instance update-expiry counter,
 * reported once per virtual second. The 2026-07-22 perf bug was exactly
 * an expiry flood here; this makes any recurrence measurable at a
 * glance instead of needing a host profiler. */
static void gnw_h7b0_tim2_count_expiry(int idx)
{
    static int enabled = -1;
    static int64_t window_start;
    static uint32_t counts[GNW_H7B0_TIM2_BLOCK_INSTANCE_COUNT];

    if (enabled < 0) {
        enabled = gnw_env_enabled("GNW_TIMER_LATE");
    }
    if (!enabled) {
        return;
    }
    counts[idx]++;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (now - window_start >= NANOSECONDS_PER_SECOND) {
        if (window_start) {
            fprintf(stderr, "TIMEXP");
            for (int i = 0; i < GNW_H7B0_TIM2_BLOCK_INSTANCE_COUNT; i++) {
                if (counts[i]) {
                    fprintf(stderr, " tim[%d]=%u/s", i, counts[i]);
                }
            }
            fprintf(stderr, "\n");
        }
        memset(counts, 0, sizeof(counts));
        window_start = now;
    }
}

static void gnw_h7b0_tim2_timer_tick(void *opaque)
{
    GnwH7B0Tim2TimerCtx *ctx = opaque;
    GnwH7B0Tim2State *s = ctx->s;
    int idx = ctx->idx;

    gnw_h7b0_tim2_count_expiry(idx);
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
        s->regs[i] = get_tim2_reset_value((i * 4) & 0x3ffu);
    }
    for (int i = 0; i < GNW_H7B0_TIM2_BLOCK_INSTANCE_COUNT; i++) {
        timer_del(s->count_timer[i]);
        s->cnt_base[i] = 0;
        s->cnt_ref_ns[i] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
}

static uint64_t gnw_h7b0_tim2_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0Tim2State *s = GNW_H7B0_TIM2(opaque);
    if (addr >= GNW_H7B0_TIM2_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return 0;
    }

    /*
     * CNT must be derived from elapsed time, not read back as a shadow.
     * Firmware uses a free-running instance (ARR=0xffffffff) as its
     * microsecond time base and polls this register hundreds of times a
     * second; a shadow answers "no time has passed" every time. See
     * gnw_h7b0_tim2.h's cnt_base/cnt_ref_ns comment for the measured
     * audio symptom this caused.
     */
    if ((addr & 0x3ffu) == GNW_H7B0_TIM2_CNT_OFFSET) {
        int idx = gnw_h7b0_tim2_instance_index(addr);
        if (idx >= 0) {
            uint32_t v = gnw_h7b0_tim2_live_cnt(s, idx);
            /* Cached: firmware polls CNT at busy-wait rates, and
             * msvcrt's getenv() is a locked linear scan -- calling it
             * per-read measurably slowed the whole guest on Windows. */
            static int trace_env = -1;
            if (trace_env < 0) {
                trace_env = gnw_env_enabled("GNW_AUDIO_TRACE");
            }
            if (trace_env) {
                fprintf(stderr, "TR %d %" PRId64 " %u\n", idx,
                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), v);
            }
            return v;
        }
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
    /*
     * get_tim2_write_mask()/get_tim2_reset_value() (auto-generated from
     * a single TIM2 instance's own register layout) only recognize
     * offsets within the first 0x400 window -- BUG FIX (2026-07-16):
     * using the raw block-wide `addr` here meant every instance past
     * TIM2 itself (TIM3-TIM7, offsets 0x400+) hit those functions'
     * default case (mask 0) for literally every register, silently
     * turning every write to TIM3-TIM7 (CR1, PSC, ARR, SR, ...) into a
     * no-op -- found via stm32h7b0-diag's timer_tim6_update case: with
     * CR1.CEN never actually taking effect, gnw_h7b0_tim2_start_counting()
     * never ran, and (more subtly) firmware's own `TIM6->SR = 0` clear
     * -- meant to clear the UIF that the EGR.UG side effect below had
     * just set -- was *also* a no-op, so the busy-wait loop saw UIF
     * already set and returned almost instantly. Fold to the
     * per-instance local offset before consulting either table, same as
     * the EGR/CR1 side-effect logic below already correctly does.
     */
    uint32_t local_addr = addr & 0x3ffu;
    uint32_t mask = get_tim2_write_mask(local_addr);
    uint32_t old_value = s->regs[addr >> 2];
    int cnt_idx = gnw_h7b0_tim2_instance_index(addr);

    /*
     * Anything that changes how CNT extrapolates (its own value, the
     * prescaler, or the wrap point) has to settle the elapsed time so far
     * against the OLD parameters first -- hence latching before the store
     * below, not after.
     */
    if (cnt_idx >= 0 && (local_addr == GNW_H7B0_TIM2_CNT_OFFSET ||
                         local_addr == GNW_H7B0_TIM2_PSC_OFFSET ||
                         local_addr == GNW_H7B0_TIM2_ARR_OFFSET)) {
        gnw_h7b0_tim2_latch_cnt(s, cnt_idx);
    }

    s->regs[addr >> 2] = (old_value & ~mask) | ((uint32_t)val64 & mask);

    /* A write to CNT sets the counter outright. */
    if (cnt_idx >= 0 && local_addr == GNW_H7B0_TIM2_CNT_OFFSET) {
        s->cnt_base[cnt_idx] = s->regs[addr >> 2];
        s->cnt_ref_ns[cnt_idx] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }

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
        /* UG re-initializes the counter (and reloads the prescaler) on
         * real hardware, so restart the extrapolation from zero. */
        if (cnt_idx >= 0) {
            s->cnt_base[cnt_idx] = 0;
            s->cnt_ref_ns[cnt_idx] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
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
                /* Counting resumes from the frozen value, as of now. */
                s->cnt_ref_ns[idx] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                gnw_h7b0_tim2_start_counting(s, idx);
            } else if (!(new_cr1 & 0x1u)) {
                if (old_value & 0x1u) {
                    /* Freeze CNT where it had got to. */
                    gnw_h7b0_tim2_latch_cnt(s, idx);
                }
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
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0Tim2State, GNW_H7B0_TIM2_SIZE / 4),
        /* CNT is derived, so its reference point has to travel with the
         * snapshot or the counter jumps on restore. */
        VMSTATE_UINT64_ARRAY(cnt_ref_ns, GnwH7B0Tim2State,
                             GNW_H7B0_TIM2_BLOCK_INSTANCE_COUNT),
        VMSTATE_UINT32_ARRAY(cnt_base, GnwH7B0Tim2State,
                             GNW_H7B0_TIM2_BLOCK_INSTANCE_COUNT),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_tim2_class_init(ObjectClass *klass, const void *data)
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
