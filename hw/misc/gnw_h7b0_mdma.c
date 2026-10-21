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
 * The default model remains the synchronous SW-request transfer described
 * below.  An explicit timing mode can instead consume a versioned external
 * service profile and commit completed service units over virtual time.  No
 * hardware profile is supplied here: the built-in profile is a hidden qtest
 * seam and is not a hardware latency or throughput claim.
 *
 * HAL's HAL_MDMA_Start() writes CCR.EN=1, then (for
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
#include "hw/misc/gnw_h7b0_mdma_large_candidate.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "migration/vmstate.h"
#include "exec/cpu-common.h"
#include "exec/gnw-cycle-model.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/misc/gnw_h7b0_mdma.h"

#define MDMA_CHANNEL_BASE      0x40
#define MDMA_CHANNEL_STRIDE    0x40
#define MDMA_CHANNEL_COUNT     GNW_H7B0_MDMA_CHANNEL_COUNT

#define MDMA_GISR0_OFF  0x00
#define MDMA_CISR_OFF   0x00
#define MDMA_CIFCR_OFF  0x04
#define MDMA_CESR_OFF   0x08
#define MDMA_CCR_OFF    0x0c
#define MDMA_CTCR_OFF   0x10
#define MDMA_CBNDTR_OFF 0x14
#define MDMA_CSAR_OFF   0x18
#define MDMA_CDAR_OFF   0x1c
#define MDMA_CTBR_OFF   0x28

#define MDMA_CCR_EN     (1U << 0)
#define MDMA_CCR_TEIE   (1U << 1)
#define MDMA_CCR_CTCIE  (1U << 2)
#define MDMA_CCR_BRTIE  (1U << 3)
#define MDMA_CCR_BTIE   (1U << 4)
#define MDMA_CCR_TCIE   (1U << 5)
#define MDMA_CCR_SWRQ   (1U << 16)

#define MDMA_CISR_TEIF  (1U << 0)
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
#define MDMA_CTCR_TLEN_SHIFT   18
#define MDMA_CTCR_TLEN_MASK    (0x7fU << MDMA_CTCR_TLEN_SHIFT)
#define MDMA_CTCR_TRGM_SHIFT   28
#define MDMA_CTCR_TRGM_MASK    (0x3U << MDMA_CTCR_TRGM_SHIFT)
#define MDMA_CTCR_TRGM_BUFFER  0
#define MDMA_CTCR_TRGM_BLOCK   (0x1U << MDMA_CTCR_TRGM_SHIFT)
#define MDMA_CTCR_TRGM_FULL    (0x3U << MDMA_CTCR_TRGM_SHIFT)
#define MDMA_CTCR_SWRM         (1U << 30)
#define MDMA_CTCR_BWM          (1U << 31)
#define MDMA_CTCR_BUS_V6_GEOMETRY \
    ((0x2U << MDMA_CTCR_SINC_SHIFT) | \
     (0x2U << MDMA_CTCR_DINC_SHIFT) | \
     (0x2U << MDMA_CTCR_SSIZE_SHIFT) | \
     (0x2U << MDMA_CTCR_DSIZE_SHIFT) | \
     (0x2U << MDMA_CTCR_SINCOS_SHIFT) | \
     (0x2U << MDMA_CTCR_DINCOS_SHIFT))
#define MDMA_CTCR_BUS_V6_GEOMETRY_MASK \
    (MDMA_CTCR_SINC_MASK | MDMA_CTCR_DINC_MASK | \
     MDMA_CTCR_SSIZE_MASK | MDMA_CTCR_DSIZE_MASK | \
     MDMA_CTCR_SINCOS_MASK | MDMA_CTCR_DINCOS_MASK)

#define MDMA_CBNDTR_BNDT_MASK 0x1FFFFU
#define MDMA_CISR_CRQA  (1U << 16)
#define MDMA_CISR_IRQ_FLAGS (MDMA_CISR_TEIF | MDMA_CISR_CTCIF | \
                             MDMA_CISR_BRTIF | MDMA_CISR_BTIF | \
                             MDMA_CISR_TCIF)
#define MDMA_CCR_IRQ_ENABLES (MDMA_CCR_TEIE | MDMA_CCR_CTCIE | \
                              MDMA_CCR_BRTIE | MDMA_CCR_BTIE | \
                              MDMA_CCR_TCIE)
#define MDMA_CESR_TEA_MASK 0x7fU
#define MDMA_CESR_TED      (1U << 7)
#define MDMA_CESR_ASE      (1U << 10)
#define MDMA_CESR_BSE      (1U << 11)
#define MDMA_TEST_PROFILE_ID UINT32_C(0x4d445401)
#define DMA_ENGINE_R1_RECEIPT_SHA256 \
    "a4cbac646ba40c21f0e345f5f83124cac638e5c42dcfbc782ec54559bddfeb8e"
#define MDMA_AXI1_V1_PROFILE_ID UINT32_C(0x4d444101)
#define MDMA_DMA_ENGINE_R1_PROFILE_ID UINT32_C(0x4d445201)
#define MDMA_AXI1_V1_CHANNEL 15
#define MDMA_AXI1_V1_BYTES UINT32_C(24576)
#define MDMA_AXI1_V1_SOURCE UINT32_C(0x240d8000)
#define MDMA_AXI1_V1_DESTINATION UINT32_C(0x240e8000)
#define MDMA_SETTLE_BUDGET 64
#define AXI_SRAM_BASE UINT64_C(0x24000000)
#define AXI_SRAM_END  UINT64_C(0x24100000)
#define AXI_SRAM1_END UINT64_C(0x24040000)
#define AXI_SRAM2_END UINT64_C(0x240a0000)

static const GnwH7B0MdmaTimingProfile mdma_large_profile = {
    .identity = MDMA_DMA_ENGINE_R1_PROFILE_ID,
    .eligible = true,
    .setup_cycles = 1,
    .cycles_per_byte_num = GNW_MDMA_LARGE_CANDIDATE_SLOPE_NUM,
    .cycles_per_byte_den = GNW_MDMA_LARGE_CANDIDATE_DEN,
    .service_bytes = GNW_MDMA_LARGE_CANDIDATE_SERVICE_BYTES,
    .setup_num = GNW_MDMA_LARGE_CANDIDATE_SETUP_NUM,
    .reference_hz = GNW_MDMA_LARGE_CANDIDATE_REFERENCE_HZ,
    .required_clock_hz = GNW_MDMA_LARGE_CANDIDATE_REFERENCE_HZ,
};

static const GnwH7B0MdmaTimingProfile *
gnw_h7b0_mdma_channel_profile(GnwH7B0MdmaState *s, int ch)
{
    return s->large_profile[ch] ? &mdma_large_profile : &s->profile;
}

static hwaddr gnw_h7b0_mdma_ch_base(int ch)
{
    return MDMA_CHANNEL_BASE + (hwaddr)ch * MDMA_CHANNEL_STRIDE;
}

/*
 * GISR0.GIFx and the single MDMA interrupt output are combinational: a
 * channel contributes when at least one of CISR[4:0] is pending and the
 * corresponding CCR interrupt enable (one bit higher) is set.  CRQA is a
 * status bit only and never contributes to the interrupt.
 */
static void gnw_h7b0_mdma_update_irq(GnwH7B0MdmaState *s)
{
    uint32_t gisr = 0;

    for (int ch = 0; ch < MDMA_CHANNEL_COUNT; ch++) {
        hwaddr base = gnw_h7b0_mdma_ch_base(ch);
        uint32_t flags = s->regs[(base + MDMA_CISR_OFF) >> 2] &
                         MDMA_CISR_IRQ_FLAGS;
        uint32_t enables = s->regs[(base + MDMA_CCR_OFF) >> 2] &
                           MDMA_CCR_IRQ_ENABLES;

        if (flags & (enables >> 1)) {
            gisr |= 1U << ch;
        }
    }
    s->regs[MDMA_GISR0_OFF >> 2] = gisr;
    qemu_set_irq(s->irq, gisr != 0);
}

static void gnw_h7b0_mdma_set_error(GnwH7B0MdmaState *s, int ch,
                                     uint32_t cesr)
{
    hwaddr base = gnw_h7b0_mdma_ch_base(ch);

    s->active[ch] = false;
    s->external_paced[ch] = false;
    s->large_profile[ch] = false;
    timer_del(s->timer[ch]);
    s->regs[(base + MDMA_CESR_OFF) >> 2] = cesr;
    s->regs[(base + MDMA_CISR_OFF) >> 2] &= ~MDMA_CISR_CRQA;
    s->regs[(base + MDMA_CISR_OFF) >> 2] |= MDMA_CISR_TEIF;
    s->regs[(base + MDMA_CCR_OFF) >> 2] &= ~(MDMA_CCR_EN | MDMA_CCR_SWRQ);
    gnw_h7b0_mdma_update_irq(s);
}

/* Return an RM0455-defined configuration error before any data is visible. */
static uint32_t gnw_h7b0_mdma_config_error(GnwH7B0MdmaState *s, int ch)
{
    hwaddr base = gnw_h7b0_mdma_ch_base(ch);
    uint32_t ctcr = s->regs[(base + MDMA_CTCR_OFF) >> 2];
    uint32_t bndt = s->regs[(base + MDMA_CBNDTR_OFF) >> 2] &
                    MDMA_CBNDTR_BNDT_MASK;
    uint32_t csar = s->regs[(base + MDMA_CSAR_OFF) >> 2];
    uint32_t cdar = s->regs[(base + MDMA_CDAR_OFF) >> 2];
    uint32_t ssize = 1U << ((ctcr & MDMA_CTCR_SSIZE_MASK) >>
                            MDMA_CTCR_SSIZE_SHIFT);
    uint32_t dsize = 1U << ((ctcr & MDMA_CTCR_DSIZE_MASK) >>
                            MDMA_CTCR_DSIZE_SHIFT);

    if (csar & (ssize - 1)) {
        return MDMA_CESR_ASE | (csar & MDMA_CESR_TEA_MASK);
    }
    if (cdar & (dsize - 1)) {
        return MDMA_CESR_ASE | MDMA_CESR_TED |
               (cdar & MDMA_CESR_TEA_MASK);
    }
    if (bndt % ssize) {
        return MDMA_CESR_BSE;
    }
    if (bndt % dsize) {
        return MDMA_CESR_BSE | MDMA_CESR_TED;
    }
    return 0;
}

static int64_t gnw_h7b0_mdma_rebase_time(int64_t value, int64_t delta)
{
    if (delta > 0 && value > INT64_MAX - delta) {
        return INT64_MAX;
    }
    if (delta < 0 && value < INT64_MIN - delta) {
        return INT64_MIN;
    }
    return value + delta;
}

static void gnw_h7b0_mdma_arm(GnwH7B0MdmaState *s, int ch);

static bool gnw_h7b0_mdma_phase_need(uint32_t setup_cycles,
                                      uint32_t bytes,
                                      const GnwH7B0MdmaTimingProfile *profile,
                                      uint64_t *need)
{
    unsigned __int128 value =
        (setup_cycles && profile->setup_num ? profile->setup_num :
         (unsigned __int128)setup_cycles * profile->cycles_per_byte_den) +
        (unsigned __int128)bytes * profile->cycles_per_byte_num;

    if (!value || value > UINT64_MAX) {
        return false;
    }
    *need = value;
    return true;
}

static void gnw_h7b0_mdma_phase_add(GnwH7B0MdmaState *s, int ch,
                                     uint64_t elapsed)
{
    const GnwH7B0MdmaTimingProfile *profile =
        gnw_h7b0_mdma_channel_profile(s, ch);
    uint32_t rate = profile->reference_hz ?: s->clock_hz;
    unsigned __int128 scaled = (unsigned __int128)elapsed * rate *
                               profile->cycles_per_byte_den +
                               s->phase_subcycle[ch];
    unsigned __int128 whole = scaled / NANOSECONDS_PER_SECOND;

    s->phase_subcycle[ch] = scaled % NANOSECONDS_PER_SECOND;
    s->phase_progress[ch] = whole > UINT64_MAX - s->phase_progress[ch] ?
                            UINT64_MAX : s->phase_progress[ch] + whole;
}

static void gnw_h7b0_mdma_finish(GnwH7B0MdmaState *s, int ch)
{
    hwaddr base = gnw_h7b0_mdma_ch_base(ch);

    s->regs[(base + MDMA_CISR_OFF) >> 2] &= ~MDMA_CISR_CRQA;
    s->regs[(base + MDMA_CISR_OFF) >> 2] |= MDMA_CISR_CTCIF |
        MDMA_CISR_BTIF | MDMA_CISR_BRTIF | MDMA_CISR_TCIF;
    s->regs[(base + MDMA_CCR_OFF) >> 2] &= ~(MDMA_CCR_EN | MDMA_CCR_SWRQ);
    s->active[ch] = false;
    s->external_paced[ch] = false;
    s->large_profile[ch] = false;
    timer_del(s->timer[ch]);
    gnw_h7b0_mdma_update_irq(s);
}

static void gnw_h7b0_mdma_consume_bndt(GnwH7B0MdmaState *s, int ch,
                                        uint32_t bytes)
{
    hwaddr off = gnw_h7b0_mdma_ch_base(ch) + MDMA_CBNDTR_OFF;
    uint32_t value = s->regs[off >> 2];
    uint32_t remaining = value & MDMA_CBNDTR_BNDT_MASK;

    remaining = bytes >= remaining ? 0 : remaining - bytes;
    s->regs[off >> 2] = (value & ~MDMA_CBNDTR_BNDT_MASK) | remaining;
}

static void gnw_h7b0_mdma_complete_phase(GnwH7B0MdmaState *s, int ch)
{
    const GnwH7B0MdmaTimingProfile *profile =
        gnw_h7b0_mdma_channel_profile(s, ch);
    uint32_t bytes = s->phase_bytes[ch];
    g_autofree uint8_t *buf = g_malloc(bytes);

    cpu_physical_memory_read(s->source[ch], buf, bytes);
    cpu_physical_memory_write(s->destination[ch], buf, bytes);
    gnw_h7b0_mdma_consume_bndt(s, ch, bytes);
    s->regs[(gnw_h7b0_mdma_ch_base(ch) + MDMA_CISR_OFF) >> 2] |=
        MDMA_CISR_TCIF;
    gnw_h7b0_mdma_update_irq(s);
    s->source[ch] += bytes;
    s->destination[ch] += bytes;
    s->remaining[ch] -= bytes;
    s->first_phase[ch] = false;
    if (s->external_paced[ch]) {
        hwaddr base = gnw_h7b0_mdma_ch_base(ch);

        s->regs[(base + MDMA_CSAR_OFF) >> 2] = s->source[ch];
        s->regs[(base + MDMA_CDAR_OFF) >> 2] = s->destination[ch];
        s->active[ch] = false;
        s->external_paced[ch] = false;
        timer_del(s->timer[ch]);
        s->regs[(base + MDMA_CISR_OFF) >> 2] &= ~MDMA_CISR_CRQA;
        if (!s->remaining[ch]) {
            s->regs[(base + MDMA_CISR_OFF) >> 2] |= MDMA_CISR_CTCIF |
                MDMA_CISR_BTIF | MDMA_CISR_BRTIF;
            s->regs[(base + MDMA_CCR_OFF) >> 2] &= ~MDMA_CCR_EN;
        }
        gnw_h7b0_mdma_update_irq(s);
        return;
    }
    if (!s->remaining[ch]) {
        gnw_h7b0_mdma_finish(s, ch);
        return;
    }
    s->phase_bytes[ch] = MIN(s->remaining[ch], profile->service_bytes);
    g_assert(gnw_h7b0_mdma_phase_need(0, s->phase_bytes[ch],
                                      profile, &s->phase_need[ch]));
}

static bool gnw_h7b0_mdma_settle(GnwH7B0MdmaState *s, int ch, int64_t now)
{
    const GnwH7B0MdmaTimingProfile *profile =
        gnw_h7b0_mdma_channel_profile(s, ch);
    unsigned events = 0;

    if (!s->active[ch] || now < s->last_update_ns[ch]) {
        return true;
    }
    if (!s->clock_known || !s->clock_hz) {
        s->last_update_ns[ch] = now;
        return true;
    }
    while (s->active[ch]) {
        if (s->phase_progress[ch] >= s->phase_need[ch]) {
            s->phase_progress[ch] -= s->phase_need[ch];
            gnw_h7b0_mdma_complete_phase(s, ch);
            if (++events == MDMA_SETTLE_BUDGET) {
                return false;
            }
            continue;
        }
        uint64_t elapsed = now - s->last_update_ns[ch];
        uint64_t need = s->phase_need[ch] - s->phase_progress[ch];
        unsigned __int128 numerator =
            (unsigned __int128)need * NANOSECONDS_PER_SECOND -
            s->phase_subcycle[ch];
        unsigned __int128 denominator =
            (unsigned __int128)(profile->reference_hz ?: s->clock_hz) *
            profile->cycles_per_byte_den;
        unsigned __int128 delta128 =
            (numerator + denominator - 1) / denominator;
        uint64_t delta = delta128 > INT64_MAX ? INT64_MAX : delta128;

        if (elapsed < delta) {
            gnw_h7b0_mdma_phase_add(s, ch, elapsed);
            s->last_update_ns[ch] = now;
            return true;
        }
        gnw_h7b0_mdma_phase_add(s, ch, delta);
        s->last_update_ns[ch] += delta;
    }
    return true;
}

static void gnw_h7b0_mdma_arm(GnwH7B0MdmaState *s, int ch)
{
    const GnwH7B0MdmaTimingProfile *profile =
        gnw_h7b0_mdma_channel_profile(s, ch);
    if (!s->active[ch] || !s->clock_known || !s->clock_hz) {
        timer_del(s->timer[ch]);
        return;
    }
    if (s->phase_progress[ch] >= s->phase_need[ch]) {
        timer_mod(s->timer[ch], qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        return;
    }
    uint64_t need = s->phase_need[ch] - s->phase_progress[ch];
    unsigned __int128 numerator =
        (unsigned __int128)need * NANOSECONDS_PER_SECOND -
        s->phase_subcycle[ch];
    unsigned __int128 denominator =
        (unsigned __int128)(profile->reference_hz ?: s->clock_hz) *
        profile->cycles_per_byte_den;
    unsigned __int128 delay = (numerator + denominator - 1) / denominator;
    int64_t deadline = delay > INT64_MAX - s->last_update_ns[ch] ?
                       INT64_MAX : s->last_update_ns[ch] + (int64_t)delay;

    timer_mod(s->timer[ch], deadline);
}

static void gnw_h7b0_mdma_tick(void *opaque)
{
    GnwH7B0MdmaChannelCtx *ctx = opaque;
    GnwH7B0MdmaState *s = ctx->s;

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!gnw_h7b0_mdma_settle(s, ctx->channel, now)) {
        timer_mod(s->timer[ctx->channel], now);
    } else {
        gnw_h7b0_mdma_arm(s, ctx->channel);
    }
}

/*
 * Read-only mechanism-test view of the currently serviced AXI-SRAM ranges.
 * The virtual clock services expired timers before each one-instruction TB;
 * do not settle here, since completing DMA memory writes from a CPU memory
 * callback would make observation itself mutate the bus master.
 */
static unsigned gnw_h7b0_axi_sram_subdivision(uint64_t address,
                                               uint64_t size)
{
    uint64_t end;

    if (!size || address < AXI_SRAM_BASE || address >= AXI_SRAM_END ||
        size > AXI_SRAM_END - address) {
        return 0;
    }
    end = address + size;
    if (end <= AXI_SRAM1_END) {
        return 1;
    }
    if (address >= AXI_SRAM1_END && end <= AXI_SRAM2_END) {
        return 2;
    }
    if (address >= AXI_SRAM2_END) {
        return 3;
    }
    return 0;
}

static void gnw_h7b0_mdma_v37_bwm_record_phase_load(GnwH7B0MdmaState *s,
                                                     unsigned ch,
                                                     int64_t now)
{
    const GnwH7B0MdmaTimingProfile *profile =
        gnw_h7b0_mdma_channel_profile(s, ch);
    uint32_t rate = profile->reference_hz ?: s->clock_hz;
    uint64_t denominator = profile->cycles_per_byte_den;
    unsigned __int128 total, progress;
    unsigned bucket;

    if (!rate || !denominator || !s->phase_need[ch]) {
        return;
    }
    total = (unsigned __int128)s->phase_need[ch] * NANOSECONDS_PER_SECOND;
    progress = (unsigned __int128)s->phase_progress[ch] *
               NANOSECONDS_PER_SECOND + s->phase_subcycle[ch];
    if (now > s->last_update_ns[ch]) {
        progress += (unsigned __int128)(now - s->last_update_ns[ch]) * rate *
                    denominator;
    }
    if (progress > total) {
        progress = total;
    }
    bucket = (unsigned)(progress * 4 / total);
    s->v37_bwm_phase_loads[MIN(bucket, 3u)]++;
}

uint32_t gnw_h7b0_mdma_contends_for_range(void *opaque, uint64_t address,
                                          unsigned size)
{
    GnwH7B0MdmaState *s = opaque;
    unsigned victim_subdivision =
        gnw_h7b0_axi_sram_subdivision(address, size);

    /* A gated or unresolved engine cannot own the AXI bus.  Timed channels
     * may remain logically active while their clock is paused, so `active`
     * alone is not sufficient evidence of contention. */
    if (!victim_subdivision || !s->clock_known ||
        !s->clock_gate_enabled || !s->clock_hz) {
        return 0;
    }
    for (int ch = 0; ch < MDMA_CHANNEL_COUNT; ch++) {
        unsigned source_subdivision, destination_subdivision;

        if (!s->active[ch] || !s->remaining[ch]) {
            continue;
        }
        source_subdivision = gnw_h7b0_axi_sram_subdivision(
            s->source[ch], s->remaining[ch]);
        destination_subdivision = gnw_h7b0_axi_sram_subdivision(
            s->destination[ch], s->remaining[ch]);

        /* The retained cross-subdivision control measured no effect, while
         * the accepted signal has victim, source and destination all in the
         * documented AXISRAM3 subdivision.  Do not turn "some AXI master is
         * active" into a chip-wide contention claim.  A range crossing a
         * subdivision boundary is outside both measured topologies and
         * therefore remains uncharged.
         */
        if (source_subdivision == victim_subdivision &&
            destination_subdivision == victim_subdivision) {
            return GNW_M7_BUS_MASTER_MDMA;
        }
    }
    return 0;
}

uint32_t gnw_h7b0_mdma_large_contends_for_range(void *opaque,
                                                uint64_t address,
                                                unsigned size)
{
    GnwH7B0MdmaState *s = opaque;
    unsigned victim_subdivision =
        gnw_h7b0_axi_sram_subdivision(address, size);
    unsigned source_subdivision, destination_subdivision;
    unsigned ch = GNW_MDMA_LARGE_CANDIDATE_CHANNEL;

    if (!victim_subdivision || !s->clock_known ||
        !s->clock_gate_enabled || !s->clock_hz ||
        !s->active[ch] || !s->large_profile[ch] || !s->remaining[ch]) {
        return 0;
    }
    source_subdivision = gnw_h7b0_axi_sram_subdivision(
        s->source[ch], s->remaining[ch]);
    destination_subdivision = gnw_h7b0_axi_sram_subdivision(
        s->destination[ch], s->remaining[ch]);
    return source_subdivision == victim_subdivision &&
           destination_subdivision == victim_subdivision ?
           GNW_M7_BUS_MASTER_MDMA_LARGE : 0;
}

/* Comparison-only signal for the frozen v37 synthetic BWM request. This is
 * intentionally tied to the transient test profile and channel 15's active,
 * externally paced 128-byte buffer request. It is not a general MDMA model. */
uint32_t gnw_h7b0_mdma_v37_bwm_contends_for_range(void *opaque,
                                                   uint64_t address,
                                                   unsigned size,
                                                   int64_t guest_time_ns)
{
    GnwH7B0MdmaState *s = opaque;
    const unsigned ch = 15;
    const hwaddr base = gnw_h7b0_mdma_ch_base(ch);
    uint32_t ctcr = s->regs[(base + MDMA_CTCR_OFF) >> 2];
    unsigned victim = gnw_h7b0_axi_sram_subdivision(address, size);
    unsigned source, destination;

    if (!s->test_profile || !s->bus_v6_request_path || !victim ||
        !s->timing_mode || !s->clock_known || !s->clock_gate_enabled ||
        !s->clock_hz || !s->active[ch] || !s->external_paced[ch] ||
        !s->remaining[ch] || !s->phase_bytes[ch] ||
        !(ctcr & MDMA_CTCR_BWM) ||
        (ctcr & MDMA_CTCR_TRGM_MASK) != MDMA_CTCR_TRGM_BUFFER ||
        ((ctcr & MDMA_CTCR_TLEN_MASK) >> MDMA_CTCR_TLEN_SHIFT) + 1 != 128) {
        return 0;
    }
    source = gnw_h7b0_axi_sram_subdivision(s->source[ch], s->remaining[ch]);
    destination = gnw_h7b0_axi_sram_subdivision(s->destination[ch],
                                                 s->remaining[ch]);
    if (source == victim && destination == victim) {
        gnw_h7b0_mdma_v37_bwm_record_phase_load(s, ch, guest_time_ns);
        return GNW_M7_BUS_MASTER_MDMA_BWM_COMPARE;
    }
    return 0;
}

void gnw_h7b0_mdma_set_timing_profile(GnwH7B0MdmaState *s,
                                       const GnwH7B0MdmaTimingProfile *profile)
{
    g_assert(!DEVICE(s)->realized);
    s->profile = profile ? *profile : (GnwH7B0MdmaTimingProfile) { 0 };
    s->migration_profile_identity = s->profile.identity;
}

bool gnw_h7b0_mdma_bus_v6_request(GnwH7B0MdmaState *s, uint32_t request)
{
    const unsigned ch = 15;
    hwaddr base = gnw_h7b0_mdma_ch_base(ch);
    uint32_t ctcr = s->regs[(base + MDMA_CTCR_OFF) >> 2];
    uint32_t remaining = s->regs[(base + MDMA_CBNDTR_OFF) >> 2] &
                         MDMA_CBNDTR_BNDT_MASK;
    uint32_t bytes = MIN(remaining, 128u);
    g_autofree uint8_t *buffer = NULL;
    uint64_t phase_need;

    if (!s->bus_v6_request_path || request != 7 ||
        !(s->regs[(base + MDMA_CCR_OFF) >> 2] & MDMA_CCR_EN) ||
        (s->regs[(base + MDMA_CTBR_OFF) >> 2] & 0x3fu) != request ||
        (ctcr & MDMA_CTCR_TRGM_MASK) != MDMA_CTCR_TRGM_BUFFER ||
        (ctcr & MDMA_CTCR_BUS_V6_GEOMETRY_MASK) !=
            MDMA_CTCR_BUS_V6_GEOMETRY ||
        ((ctcr & MDMA_CTCR_TLEN_MASK) >> MDMA_CTCR_TLEN_SHIFT) + 1 != 128 ||
        !(ctcr & MDMA_CTCR_BWM) ||
        !remaining || s->active[ch]) {
        return false;
    }
    if (gnw_h7b0_mdma_config_error(s, ch)) {
        gnw_h7b0_mdma_set_error(s, ch, gnw_h7b0_mdma_config_error(s, ch));
        return false;
    }
    /* The hidden profile exercises the real paced-service lifecycle without
     * assigning an unmeasured production width to BUFFER requests.  The
     * coordinated bus-v11 FIT must supply and authorize that production
     * profile before this gate can be broadened. */
    if (s->timing_mode && s->profile.identity == MDMA_TEST_PROFILE_ID &&
        s->profile.eligible && s->clock_known && s->clock_gate_enabled &&
        s->clock_hz &&
        gnw_h7b0_mdma_phase_need(s->profile.setup_cycles, bytes,
                                 &s->profile, &phase_need)) {
        s->active[ch] = true;
        s->external_paced[ch] = true;
        s->first_phase[ch] = true;
        s->large_profile[ch] = false;
        s->source[ch] = s->regs[(base + MDMA_CSAR_OFF) >> 2];
        s->destination[ch] = s->regs[(base + MDMA_CDAR_OFF) >> 2];
        s->remaining[ch] = remaining;
        s->phase_bytes[ch] = bytes;
        s->phase_need[ch] = phase_need;
        s->phase_progress[ch] = 0;
        s->phase_subcycle[ch] = 0;
        s->last_update_ns[ch] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        s->regs[(base + MDMA_CISR_OFF) >> 2] |= MDMA_CISR_CRQA;
        gnw_h7b0_mdma_arm(s, ch);
        gnw_h7b0_mdma_update_irq(s);
        return true;
    }
    buffer = g_malloc(bytes);
    s->regs[(base + MDMA_CISR_OFF) >> 2] |= MDMA_CISR_CRQA;
    cpu_physical_memory_read(s->regs[(base + MDMA_CSAR_OFF) >> 2],
                             buffer, bytes);
    cpu_physical_memory_write(s->regs[(base + MDMA_CDAR_OFF) >> 2],
                              buffer, bytes);
    s->regs[(base + MDMA_CSAR_OFF) >> 2] += bytes;
    s->regs[(base + MDMA_CDAR_OFF) >> 2] += bytes;
    gnw_h7b0_mdma_consume_bndt(s, ch, bytes);
    /* BUFFER requests complete one TLEN unit.  TCIF is the per-buffer
     * completion flag (HAL's MDMA_FLAG_BFTC); BTIF is reserved for the end
     * of the whole block.  This synchronous fallback has finished the
     * accepted request before returning, so CRQA must clear even while the
     * channel remains enabled for the next external request. */
    s->regs[(base + MDMA_CISR_OFF) >> 2] |= MDMA_CISR_TCIF;
    s->regs[(base + MDMA_CISR_OFF) >> 2] &= ~MDMA_CISR_CRQA;
    if (bytes == remaining) {
        s->regs[(base + MDMA_CISR_OFF) >> 2] |= MDMA_CISR_CTCIF |
                                                 MDMA_CISR_BTIF |
                                                 MDMA_CISR_BRTIF |
                                                 MDMA_CISR_TCIF;
        s->regs[(base + MDMA_CCR_OFF) >> 2] &= ~MDMA_CCR_EN;
    }
    gnw_h7b0_mdma_update_irq(s);
    return true;
}

void gnw_h7b0_mdma_set_clock_info(GnwH7B0MdmaState *s, bool known,
                                   uint32_t reference_hz,
                                   bool gate_enabled)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    for (int ch = 0; ch < MDMA_CHANNEL_COUNT; ch++) {
        while (!gnw_h7b0_mdma_settle(s, ch, now)) {
            /* BNDT bounds the number of visible service-unit commits. */
        }
    }
    /* A known gated clock pauses the engine; it does not invalidate the
     * calibrated rate.  Reject only an unresolved clock or a different rate
     * while the clock is actually driving the engine.  On re-enable the
     * clock-disabled settle path rebases last_update_ns[] before arming, so
     * no service accrues during the gated interval. */
    if (s->profile.required_clock_hz &&
        (!known || (gate_enabled &&
                    reference_hz != s->profile.required_clock_hz))) {
        for (int ch = 0; ch < MDMA_CHANNEL_COUNT; ch++) {
            if (s->active[ch]) {
                s->unsupported_count++;
                gnw_h7b0_mdma_set_error(s, ch, 0);
            }
        }
    }
    if (!known) {
        for (int ch = 0; ch < MDMA_CHANNEL_COUNT; ch++) {
            if (s->active[ch]) {
                s->unsupported_count++;
                qemu_log_mask(LOG_UNIMP,
                              "gnw-h7b0-mdma: active timed channel %d lost "
                              "known engine clock; phase paused\n", ch);
            }
        }
    }
    s->clock_known = known;
    s->clock_gate_enabled = gate_enabled;
    s->clock_hz = known && gate_enabled ? reference_hz : 0;
    for (int ch = 0; ch < MDMA_CHANNEL_COUNT; ch++) {
        gnw_h7b0_mdma_arm(s, ch);
    }
}

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

    /*
     * Common case: both source and destination genuinely increment
     * contiguously (step == element size), i.e. a real word-contiguous
     * block copy -- exactly what every known firmware transfer here
     * uses. That's just one contiguous byte range in, one contiguous
     * byte range out; do it as a single bulk cpu_physical_memory_read()/
     * write() pair instead of one call pair per element. Found via
     * stm32h7b0-diag's mdma_m2m.md report: the old per-word loop paid
     * QEMU's real AddressSpace-dispatch/RCU overhead per 4-byte element
     * (1024 call pairs for a 4KB word-sized transfer), which -- being
     * real host CPU time -- leaked into the guest's wall-clock-tied
     * CYCCNT as ~11ms of apparent firmware time for a transfer that
     * completes in zero elapsed guest instructions either way; a ~21x
     * QEMU-vs-hardware gap driven entirely by this dispatch overhead,
     * not anything guest-observable. Bulk-copying doesn't change any
     * guest-visible behavior (same bytes end up in the same place,
     * same completion flags), it just does it with 1 call pair instead
     * of up to 32768 (CBNDTR.BNDT's 17-bit field allows up to 128KB).
     * Any less-common pattern (fixed source/dest, or a stride that
     * doesn't match the element size -- true striped/gather-scatter
     * access) still falls through to the exact per-element loop this
     * replaces, unchanged.
     */
    if (sinc && sstep == ssize && dinc && dstep == dsize) {
        g_autofree uint8_t *buf = g_malloc(bndt);
        cpu_physical_memory_read(csar, buf, bndt);
        cpu_physical_memory_write(cdar, buf, bndt);
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

#define GNW_AXI_SRAM_BASE UINT32_C(0x24000000)
#define GNW_AXI_SRAM_BYTES UINT32_C(0x00100000)

static bool gnw_h7b0_mdma_axi_range(uint32_t address, uint32_t bytes)
{
    uint64_t end = (uint64_t)address + bytes;

    return bytes && address >= GNW_AXI_SRAM_BASE &&
           end <= (uint64_t)GNW_AXI_SRAM_BASE + GNW_AXI_SRAM_BYTES;
}

typedef enum GnwTimedStart {
    GNW_TIMED_START_DISABLED,
    GNW_TIMED_START_STARTED,
    GNW_TIMED_START_REJECTED,
} GnwTimedStart;

static GnwTimedStart gnw_h7b0_mdma_start_timed(GnwH7B0MdmaState *s, int ch)
{
    hwaddr base = gnw_h7b0_mdma_ch_base(ch);
    uint32_t ctcr = s->regs[(base + MDMA_CTCR_OFF) >> 2];
    uint32_t bndt = s->regs[(base + MDMA_CBNDTR_OFF) >> 2] &
                    MDMA_CBNDTR_BNDT_MASK;
    uint32_t ssize = 1U << ((ctcr & MDMA_CTCR_SSIZE_MASK) >>
                            MDMA_CTCR_SSIZE_SHIFT);
    uint32_t dsize = 1U << ((ctcr & MDMA_CTCR_DSIZE_MASK) >>
                            MDMA_CTCR_DSIZE_SHIFT);
    bool sinc = ((ctcr & MDMA_CTCR_SINC_MASK) >> MDMA_CTCR_SINC_SHIFT) != 0;
    bool dinc = ((ctcr & MDMA_CTCR_DINC_MASK) >> MDMA_CTCR_DINC_SHIFT) != 0;
    uint32_t sstep = 1U << ((ctcr & MDMA_CTCR_SINCOS_MASK) >>
                            MDMA_CTCR_SINCOS_SHIFT);
    uint32_t dstep = 1U << ((ctcr & MDMA_CTCR_DINCOS_MASK) >>
                            MDMA_CTCR_DINCOS_SHIFT);
    uint32_t tlen = ((ctcr & MDMA_CTCR_TLEN_MASK) >>
                     MDMA_CTCR_TLEN_SHIFT) + 1;
    uint32_t trigger_mode = ctcr & MDMA_CTCR_TRGM_MASK;
    const GnwH7B0MdmaTimingProfile *profile = &s->profile;
    bool large_profile = false;
    uint64_t first_need;

    if (!s->timing_mode) {
        return GNW_TIMED_START_DISABLED;
    }
    if (!s->profile.identity) {
        return GNW_TIMED_START_DISABLED;
    }

    /* The calibrated profile is evidence-backed only for the frozen
     * independent holdout geometry.  Reject another geometry before making
     * data visible instead of silently completing it at zero guest time. */
    if (s->profile.identity == MDMA_AXI1_V1_PROFILE_ID &&
        (ch != MDMA_AXI1_V1_CHANNEL || bndt != MDMA_AXI1_V1_BYTES ||
         ssize != 1 || dsize != 1 || tlen != 128 ||
         trigger_mode != MDMA_CTCR_TRGM_FULL ||
         s->regs[(base + MDMA_CSAR_OFF) >> 2] != MDMA_AXI1_V1_SOURCE ||
         s->regs[(base + MDMA_CDAR_OFF) >> 2] != MDMA_AXI1_V1_DESTINATION)) {
        s->unsupported_count++;
        qemu_log_mask(LOG_UNIMP,
                      "gnw-h7b0-mdma: AXI1 v1 profile geometry mismatch; "
                      "transfer rejected before data became visible\n");
        gnw_h7b0_mdma_set_error(s, ch, 0);
        return GNW_TIMED_START_REJECTED;
    }

    if (s->profile.identity == MDMA_DMA_ENGINE_R1_PROFILE_ID &&
        GNW_MDMA_LARGE_CANDIDATE_RUNTIME_AUTHORIZED &&
        gnw_mdma_large_candidate_scope(ch,
            s->regs[(base + MDMA_CSAR_OFF) >> 2],
            s->regs[(base + MDMA_CDAR_OFF) >> 2], bndt, ssize, tlen,
            sinc && sstep == ssize, dinc && dstep == dsize,
            trigger_mode == MDMA_CTCR_TRGM_FULL,
            ctcr & MDMA_CTCR_BWM, s->clock_hz)) {
        profile = &mdma_large_profile;
        large_profile = true;
    }

    if (!profile->eligible ||
        !profile->cycles_per_byte_num ||
        !profile->cycles_per_byte_den || !profile->service_bytes ||
        !s->clock_known || !bndt || ssize != dsize || !sinc || !dinc ||
        sstep != ssize || dstep != dsize ||
        tlen != MIN(bndt, profile->service_bytes) ||
        tlen % ssize || !(ctcr & MDMA_CTCR_SWRM) ||
        (trigger_mode != MDMA_CTCR_TRGM_BLOCK &&
         trigger_mode != MDMA_CTCR_TRGM_FULL)) {
        if (s->profile.identity == MDMA_DMA_ENGINE_R1_PROFILE_ID) {
            return GNW_TIMED_START_DISABLED;
        }
        s->unsupported_count++;
        qemu_log_mask(LOG_UNIMP,
                      "gnw-h7b0-mdma: timed SW transfer unsupported; "
                      "transfer rejected before data became visible\n");
        gnw_h7b0_mdma_set_error(s, ch, 0);
        return GNW_TIMED_START_REJECTED;
    }
    if (s->profile.identity == MDMA_DMA_ENGINE_R1_PROFILE_ID &&
        !large_profile &&
        (ch != 0 || ssize != 4 || dsize != 4 || bndt < 16 || bndt > 4096 ||
         (ctcr & MDMA_CTCR_BWM) ||
         !gnw_h7b0_mdma_axi_range(
             s->regs[(base + MDMA_CSAR_OFF) >> 2], bndt) ||
         !gnw_h7b0_mdma_axi_range(
             s->regs[(base + MDMA_CDAR_OFF) >> 2], bndt) ||
         ((uint64_t)s->regs[(base + MDMA_CSAR_OFF) >> 2] <
              (uint64_t)s->regs[(base + MDMA_CDAR_OFF) >> 2] + bndt &&
          (uint64_t)s->regs[(base + MDMA_CDAR_OFF) >> 2] <
              (uint64_t)s->regs[(base + MDMA_CSAR_OFF) >> 2] + bndt))) {
        return GNW_TIMED_START_DISABLED;
    }
    if (profile->required_clock_hz &&
        s->clock_hz != profile->required_clock_hz) {
        if (s->profile.identity == MDMA_DMA_ENGINE_R1_PROFILE_ID) {
            return GNW_TIMED_START_DISABLED;
        }
        return gnw_h7b0_mdma_set_error(s, ch, 0), GNW_TIMED_START_REJECTED;
    }
    if (!gnw_h7b0_mdma_phase_need(profile->setup_cycles,
                                   MIN(bndt, tlen),
                                   profile, &first_need)) {
        s->unsupported_count++;
        qemu_log_mask(LOG_UNIMP,
                      "gnw-h7b0-mdma: timed profile phase overflows; "
                      "transfer rejected before data became visible\n");
        gnw_h7b0_mdma_set_error(s, ch, 0);
        return GNW_TIMED_START_REJECTED;
    }
    s->active[ch] = true;
    s->external_paced[ch] = false;
    s->first_phase[ch] = true;
    s->large_profile[ch] = large_profile;
    s->source[ch] = s->regs[(base + MDMA_CSAR_OFF) >> 2];
    s->destination[ch] = s->regs[(base + MDMA_CDAR_OFF) >> 2];
    s->remaining[ch] = bndt;
    s->phase_bytes[ch] = MIN(bndt, profile->service_bytes);
    s->phase_need[ch] = first_need;
    s->phase_progress[ch] = 0;
    s->phase_subcycle[ch] = 0;
    s->last_update_ns[ch] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->regs[(base + MDMA_CISR_OFF) >> 2] |= MDMA_CISR_CRQA;
    gnw_h7b0_mdma_arm(s, ch);
    return GNW_TIMED_START_STARTED;
}

static void gnw_h7b0_mdma_reset(DeviceState *dev)
{
    GnwH7B0MdmaState *s = GNW_H7B0_MDMA(dev);
    memset(s->regs, 0, sizeof(s->regs));
    for (int ch = 0; ch < MDMA_CHANNEL_COUNT; ch++) {
        timer_del(s->timer[ch]);
        s->active[ch] = false;
        s->external_paced[ch] = false;
        s->first_phase[ch] = false;
        s->large_profile[ch] = false;
        s->remaining[ch] = 0;
        s->phase_progress[ch] = 0;
        s->phase_subcycle[ch] = 0;
    }
    memset(s->v37_bwm_phase_loads, 0, sizeof(s->v37_bwm_phase_loads));
    qemu_set_irq(s->irq, 0);
}

static uint64_t gnw_h7b0_mdma_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0MdmaState *s = GNW_H7B0_MDMA(opaque);
    if (addr >= GNW_H7B0_MDMA_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return 0;
    }
    if (addr == MDMA_GISR0_OFF) {
        gnw_h7b0_mdma_update_irq(s);
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
    if (addr == MDMA_GISR0_OFF) {
        /* GISR0 is read-only and derived from channel status/enables. */
        return;
    }

    if (addr >= MDMA_CHANNEL_BASE) {
        hwaddr rel = addr - MDMA_CHANNEL_BASE;
        int ch = (int)(rel / MDMA_CHANNEL_STRIDE);
        hwaddr ch_off = rel % MDMA_CHANNEL_STRIDE;

        if (ch < MDMA_CHANNEL_COUNT &&
            (ch_off == MDMA_CISR_OFF || ch_off == MDMA_CESR_OFF)) {
            /* Hardware-owned status registers are read-only. */
            return;
        }

        if (ch < MDMA_CHANNEL_COUNT && ch_off == MDMA_CIFCR_OFF) {
            /* Write-1-to-clear against CISR (identical bit layout). */
            hwaddr cisr_off = MDMA_CHANNEL_BASE +
                               (hwaddr)ch * MDMA_CHANNEL_STRIDE + MDMA_CISR_OFF;
            uint32_t clear = (uint32_t)val64 & MDMA_CISR_IRQ_FLAGS;

            s->regs[cisr_off >> 2] &= ~clear;
            if (clear & MDMA_CISR_TEIF) {
                s->regs[(gnw_h7b0_mdma_ch_base(ch) +
                         MDMA_CESR_OFF) >> 2] = 0;
            }
            s->regs[addr >> 2] = (uint32_t)val64;
            gnw_h7b0_mdma_update_irq(s);
            return;
        }

        if (ch < MDMA_CHANNEL_COUNT && ch_off == MDMA_CCR_OFF) {
            uint32_t old_val = s->regs[addr >> 2];
            uint32_t new_val = (uint32_t)val64;
            int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

            if (s->active[ch]) {
                while (!gnw_h7b0_mdma_settle(s, ch, now)) {
                    /* Control writes observe all units due by now. */
                }
                if (!s->active[ch]) {
                    /* A stale SWRQ copied from CCR cannot start a second
                     * descriptor after settlement completed the first. */
                    s->regs[addr >> 2] = new_val &
                        ~(MDMA_CCR_EN | MDMA_CCR_SWRQ);
                    gnw_h7b0_mdma_update_irq(s);
                    return;
                }
                if (!(new_val & MDMA_CCR_EN)) {
                    s->active[ch] = false;
                    s->external_paced[ch] = false;
                    s->first_phase[ch] = false;
                    s->large_profile[ch] = false;
                    timer_del(s->timer[ch]);
                    s->regs[(gnw_h7b0_mdma_ch_base(ch) +
                             MDMA_CISR_OFF) >> 2] &= ~MDMA_CISR_CRQA;
                    s->regs[(gnw_h7b0_mdma_ch_base(ch) +
                             MDMA_CISR_OFF) >> 2] |= MDMA_CISR_CTCIF;
                    s->regs[addr >> 2] = new_val & ~MDMA_CCR_SWRQ;
                    gnw_h7b0_mdma_update_irq(s);
                    return;
                }
                /* The active descriptor is latched.  IRQ-enable writes may
                 * not change how it was triggered: externally paced BUFFER
                 * service has SWRQ clear, while a timed software request has
                 * it set until completion. */
                s->regs[addr >> 2] = (new_val | MDMA_CCR_EN) &
                    ~MDMA_CCR_SWRQ;
                s->regs[addr >> 2] |= old_val & MDMA_CCR_SWRQ;
                gnw_h7b0_mdma_update_irq(s);
                return;
            }

            s->regs[addr >> 2] = new_val;

            bool old_swrq = (old_val & MDMA_CCR_SWRQ) != 0;
            bool new_swrq = (new_val & MDMA_CCR_SWRQ) != 0;
            bool en = (new_val & MDMA_CCR_EN) != 0;

            if (en && new_swrq && !old_swrq &&
                (s->regs[(gnw_h7b0_mdma_ch_base(ch) +
                          MDMA_CTCR_OFF) >> 2] & MDMA_CTCR_SWRM)) {
                uint32_t error = gnw_h7b0_mdma_config_error(s, ch);

                if (error) {
                    gnw_h7b0_mdma_set_error(s, ch, error);
                    return;
                }
                GnwTimedStart timed = gnw_h7b0_mdma_start_timed(s, ch);

                if (timed != GNW_TIMED_START_DISABLED) {
                    gnw_h7b0_mdma_update_irq(s);
                    return;
                }
                gnw_h7b0_mdma_do_transfer(s, ch);
                gnw_h7b0_mdma_consume_bndt(s, ch,
                    s->regs[(gnw_h7b0_mdma_ch_base(ch) +
                             MDMA_CBNDTR_OFF) >> 2] & MDMA_CBNDTR_BNDT_MASK);

                hwaddr cisr_off = MDMA_CHANNEL_BASE +
                                   (hwaddr)ch * MDMA_CHANNEL_STRIDE + MDMA_CISR_OFF;
                s->regs[cisr_off >> 2] |= MDMA_CISR_CTCIF | MDMA_CISR_BTIF |
                                           MDMA_CISR_BRTIF | MDMA_CISR_TCIF;
                s->regs[addr >> 2] &= ~(MDMA_CCR_EN | MDMA_CCR_SWRQ);
            }
            gnw_h7b0_mdma_update_irq(s);
            return;
        }

        if (ch < MDMA_CHANNEL_COUNT && s->active[ch] &&
            (ch_off == MDMA_CTCR_OFF || ch_off == MDMA_CBNDTR_OFF ||
             ch_off == MDMA_CSAR_OFF || ch_off == MDMA_CDAR_OFF)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "gnw-h7b0-mdma: channel %d descriptor write at "
                          "offset 0x%" HWADDR_PRIx " ignored while active\n",
                          ch, ch_off);
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
    static const char * const phase_property_names[] = {
        "x-v37-bwm-phase-loads-0-25",
        "x-v37-bwm-phase-loads-25-50",
        "x-v37-bwm-phase-loads-50-75",
        "x-v37-bwm-phase-loads-75-100",
    };
    s->test_clock_known = true;
    s->test_clock_enabled = true;
    s->test_clock_hz = 100000000;
    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_mdma_ops, s,
                           TYPE_GNW_H7B0_MDMA, GNW_H7B0_MDMA_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    for (unsigned i = 0; i < ARRAY_SIZE(phase_property_names); i++) {
        object_property_add_uint64_ptr(obj, phase_property_names[i],
                                       &s->v37_bwm_phase_loads[i],
                                       OBJ_PROP_FLAG_READ);
    }
    for (int ch = 0; ch < MDMA_CHANNEL_COUNT; ch++) {
        s->ctx[ch].s = s;
        s->ctx[ch].channel = ch;
        s->timer[ch] = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    gnw_h7b0_mdma_tick, &s->ctx[ch]);
    }
}

static int gnw_h7b0_mdma_pre_save(void *opaque)
{
    GnwH7B0MdmaState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    for (int ch = 0; ch < MDMA_CHANNEL_COUNT; ch++) {
        while (!gnw_h7b0_mdma_settle(s, ch, now)) {
            /* Drain all old-rate work before serializing one timestamp. */
        }
    }
    s->migration_timing_mode = s->timing_mode;
    s->migration_profile_identity = s->profile.identity;
    s->migration_clock_ns = now;
    return 0;
}

static bool gnw_h7b0_mdma_post_load(void *opaque, int version_id, Error **errp)
{
    GnwH7B0MdmaState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t delta;

    if (version_id >= 2 && s->migration_timing_mode != s->timing_mode) {
        error_setg(errp, "MDMA timing mode differs between migration endpoints");
        return false;
    }
    if (version_id < 2) {
        if (s->timing_mode || s->profile.identity) {
            error_setg(errp, "MDMA v1 migration lacks timed engine state");
            return false;
        }
        gnw_h7b0_mdma_update_irq(s);
        return true;
    }
    if (s->migration_clock_ns < 0) {
        error_setg(errp, "MDMA migration has invalid virtual clock epoch");
        return false;
    }
    if ((!s->clock_known || !s->clock_gate_enabled) && s->clock_hz) {
        error_setg(errp, "MDMA migration has inconsistent engine clock state");
        return false;
    }
    delta = now - s->migration_clock_ns;
    for (int ch = 0; ch < MDMA_CHANNEL_COUNT; ch++) {
        const GnwH7B0MdmaTimingProfile *profile;
        uint64_t expected_need = 0;
        hwaddr cbndtr_off = gnw_h7b0_mdma_ch_base(ch) + MDMA_CBNDTR_OFF;

        if (version_id < 4) {
            s->large_profile[ch] = false;
        }
        if (version_id < 5) {
            s->external_paced[ch] = false;
        }
        profile = gnw_h7b0_mdma_channel_profile(s, ch);
        if (s->large_profile[ch] &&
            (s->profile.identity != MDMA_DMA_ENGINE_R1_PROFILE_ID ||
             !GNW_MDMA_LARGE_CANDIDATE_RUNTIME_AUTHORIZED)) {
            error_setg(errp, "MDMA channel %d has unavailable large profile",
                       ch);
            return false;
        }

        /* v2 timed snapshots predate guest-visible BNDT progress.  Their
         * serialized remaining[] is authoritative; normalize the stale
         * register before service resumes. */
        if (version_id == 2 && s->active[ch]) {
            if (s->remaining[ch] > MDMA_CBNDTR_BNDT_MASK) {
                error_setg(errp, "MDMA channel %d has corrupt v2 remaining bytes",
                           ch);
                return false;
            }
            s->regs[cbndtr_off >> 2] =
                (s->regs[cbndtr_off >> 2] & ~MDMA_CBNDTR_BNDT_MASK) |
                s->remaining[ch];
        }

        if (s->active[ch] &&
            !gnw_h7b0_mdma_phase_need(s->first_phase[ch] ?
                                      profile->setup_cycles : 0,
                                      s->phase_bytes[ch], profile,
                                      &expected_need)) {
            error_setg(errp, "MDMA channel %d has overflowing timed state",
                       ch);
            return false;
        }
        if (s->active[ch] && (!profile->eligible || !s->remaining[ch] ||
            s->remaining[ch] > MDMA_CBNDTR_BNDT_MASK ||
            !s->phase_bytes[ch] || s->phase_bytes[ch] > s->remaining[ch] ||
            (version_id >= 3 &&
             (s->regs[cbndtr_off >> 2] & MDMA_CBNDTR_BNDT_MASK) !=
             s->remaining[ch]) ||
            /* A paced BUFFER request is one indivisible TLEN transaction.
             * Its 128-byte phase may be larger than the profile's internal
             * software-transfer service quantum. */
            (!s->external_paced[ch] &&
             s->phase_bytes[ch] > profile->service_bytes) ||
            s->phase_need[ch] != expected_need ||
            s->phase_progress[ch] >= s->phase_need[ch] ||
            s->phase_subcycle[ch] >= NANOSECONDS_PER_SECOND ||
            (uint64_t)s->source[ch] + s->remaining[ch] >
                (UINT64_C(1) << 32) ||
            (uint64_t)s->destination[ch] + s->remaining[ch] >
                (UINT64_C(1) << 32) || s->last_update_ns[ch] < 0 ||
            s->last_update_ns[ch] > s->migration_clock_ns)) {
            error_setg(errp, "MDMA channel %d has corrupt timed state", ch);
            return false;
        }
        if (s->external_paced[ch] &&
            (!s->active[ch] || ch != 15 ||
             s->profile.identity != MDMA_TEST_PROFILE_ID ||
             s->phase_bytes[ch] != 128)) {
            error_setg(errp, "MDMA channel %d has corrupt paced state", ch);
            return false;
        }
        if (s->active[ch]) {
            s->last_update_ns[ch] = gnw_h7b0_mdma_rebase_time(
                s->last_update_ns[ch], delta);
        }
        gnw_h7b0_mdma_arm(s, ch);
    }
    gnw_h7b0_mdma_update_irq(s);
    return true;
}

static const VMStateDescription vmstate_gnw_h7b0_mdma = {
    .name = TYPE_GNW_H7B0_MDMA,
    .version_id = 5,
    .minimum_version_id = 1,
    .pre_save = gnw_h7b0_mdma_pre_save,
    .post_load_errp = gnw_h7b0_mdma_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0MdmaState, GNW_H7B0_MDMA_SIZE / 4),
        VMSTATE_BOOL_V(migration_timing_mode, GnwH7B0MdmaState, 2),
        VMSTATE_UINT32_EQUAL_V(migration_profile_identity,
                               GnwH7B0MdmaState, 2,
                               "MDMA timing profile differs"),
        VMSTATE_BOOL_V(clock_known, GnwH7B0MdmaState, 2),
        VMSTATE_BOOL_V(clock_gate_enabled, GnwH7B0MdmaState, 2),
        VMSTATE_UINT32_V(clock_hz, GnwH7B0MdmaState, 2),
        VMSTATE_INT64_V(migration_clock_ns, GnwH7B0MdmaState, 2),
        VMSTATE_UINT64_V(unsupported_count, GnwH7B0MdmaState, 2),
        VMSTATE_BOOL_ARRAY_V(active, GnwH7B0MdmaState, MDMA_CHANNEL_COUNT, 2),
        VMSTATE_BOOL_ARRAY_V(external_paced, GnwH7B0MdmaState,
                             MDMA_CHANNEL_COUNT, 5),
        VMSTATE_BOOL_ARRAY_V(first_phase, GnwH7B0MdmaState,
                             MDMA_CHANNEL_COUNT, 2),
        VMSTATE_BOOL_ARRAY_V(large_profile, GnwH7B0MdmaState,
                             MDMA_CHANNEL_COUNT, 4),
        VMSTATE_UINT32_ARRAY_V(source, GnwH7B0MdmaState, MDMA_CHANNEL_COUNT, 2),
        VMSTATE_UINT32_ARRAY_V(destination, GnwH7B0MdmaState, MDMA_CHANNEL_COUNT, 2),
        VMSTATE_UINT32_ARRAY_V(remaining, GnwH7B0MdmaState, MDMA_CHANNEL_COUNT, 2),
        VMSTATE_UINT32_ARRAY_V(phase_bytes, GnwH7B0MdmaState, MDMA_CHANNEL_COUNT, 2),
        VMSTATE_UINT64_ARRAY_V(phase_need, GnwH7B0MdmaState, MDMA_CHANNEL_COUNT, 2),
        VMSTATE_UINT64_ARRAY_V(phase_progress, GnwH7B0MdmaState, MDMA_CHANNEL_COUNT, 2),
        VMSTATE_UINT32_ARRAY_V(phase_subcycle, GnwH7B0MdmaState, MDMA_CHANNEL_COUNT, 2),
        VMSTATE_INT64_ARRAY_V(last_update_ns, GnwH7B0MdmaState, MDMA_CHANNEL_COUNT, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_mdma_test_clock_set(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    const Property *prop = opaque;
    uint32_t *value = object_field_prop_ptr(obj, prop);

    if (!visit_type_uint32(v, name, value, errp)) {
        return;
    }
    GnwH7B0MdmaState *s = GNW_H7B0_MDMA(obj);
    gnw_h7b0_mdma_set_clock_info(s, s->test_clock_known,
                                 s->test_clock_hz, s->test_clock_enabled);
}

static const PropertyInfo gnw_h7b0_mdma_test_clock_prop = {
    .type = "uint32",
    .set = gnw_h7b0_mdma_test_clock_set,
    .realized_set_allowed = true,
};

static void gnw_h7b0_mdma_test_known_set(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    const Property *prop = opaque;
    bool *value = object_field_prop_ptr(obj, prop);

    if (!visit_type_bool(v, name, value, errp)) {
        return;
    }
    GnwH7B0MdmaState *s = GNW_H7B0_MDMA(obj);
    gnw_h7b0_mdma_set_clock_info(s, s->test_clock_known,
                                 s->test_clock_hz, s->test_clock_enabled);
}

static const PropertyInfo gnw_h7b0_mdma_test_known_prop = {
    .type = "bool",
    .set = gnw_h7b0_mdma_test_known_set,
    .realized_set_allowed = true,
};

static void gnw_h7b0_mdma_test_enabled_set(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    const Property *prop = opaque;
    bool *value = object_field_prop_ptr(obj, prop);

    if (!visit_type_bool(v, name, value, errp)) {
        return;
    }
    GnwH7B0MdmaState *s = GNW_H7B0_MDMA(obj);
    gnw_h7b0_mdma_set_clock_info(s, s->test_clock_known,
                                 s->test_clock_hz, s->test_clock_enabled);
}

static const PropertyInfo gnw_h7b0_mdma_test_enabled_prop = {
    .type = "bool",
    .set = gnw_h7b0_mdma_test_enabled_set,
    .realized_set_allowed = true,
};

static void gnw_h7b0_mdma_realize(DeviceState *dev, Error **errp)
{
    GnwH7B0MdmaState *s = GNW_H7B0_MDMA(dev);

    if ((s->test_profile ? 1 : 0) + (s->mdma_axi1_v1_profile ? 1 : 0) +
        (s->dma_engine_successor_profile ? 1 : 0) > 1) {
        error_setg(errp, "x-test-profile and x-mdma-axi1-v1-profile are "
                   "mutually exclusive");
        return;
    }
    if ((s->mdma_axi1_v1_profile || s->dma_engine_successor_profile) &&
        !s->timing_mode) {
        error_setg(errp, "evidence timing profile requires timing-mode=on");
        return;
    }
    if (s->dma_engine_successor_profile &&
        (!DMA_ENGINE_R1_RECEIPT_SHA256[0] ||
         !s->dma_engine_successor_receipt_sha256 ||
         strcmp(s->dma_engine_successor_receipt_sha256,
                DMA_ENGINE_R1_RECEIPT_SHA256))) {
        error_setg(errp, "DMA engine successor profile requires the exact reviewed replay receipt");
        return;
    }
    if (!s->dma_engine_successor_profile &&
        s->dma_engine_successor_receipt_sha256) {
        error_setg(errp, "MDMA DMA engine replay receipt requires its profile");
        return;
    }
    if (s->test_profile) {
        GnwH7B0MdmaTimingProfile profile = {
            .identity = MDMA_TEST_PROFILE_ID,
            .eligible = true,
            .setup_cycles = 4,
            .cycles_per_byte_num = 1,
            .cycles_per_byte_den = 1,
            .service_bytes = 16,
            .required_clock_hz = s->test_required_clock_hz,
        };
        gnw_h7b0_mdma_set_timing_profile(s, &profile);
        gnw_h7b0_mdma_set_clock_info(s, s->test_clock_known,
                                     s->test_clock_hz, s->test_clock_enabled);
    } else if (s->mdma_axi1_v1_profile) {
        GnwH7B0MdmaTimingProfile profile = {
            .identity = MDMA_AXI1_V1_PROFILE_ID,
            .eligible = true,
            /* The two-boot baseline was 99,544 cycles for 24,576 bytes. */
            .setup_cycles = 1240,
            .cycles_per_byte_num = 4,
            .cycles_per_byte_den = 1,
            .service_bytes = 128,
        };
        gnw_h7b0_mdma_set_timing_profile(s, &profile);
    } else if (s->dma_engine_successor_profile) {
        GnwH7B0MdmaTimingProfile profile = {
            .identity = MDMA_DMA_ENGINE_R1_PROFILE_ID,
            .eligible = true,
            /* Nonzero selects setup_num on the first service phase only. */
            .setup_cycles = 1,
            .setup_num = UINT64_C(679048), /* 4993/67 on denominator 9112 */
            .cycles_per_byte_num = 9567,
            .cycles_per_byte_den = 9112,
            .service_bytes = 128,
            .reference_hz = 280000000,
            .required_clock_hz = 280000000,
        };
        gnw_h7b0_mdma_set_timing_profile(s, &profile);
    }
}

static const Property gnw_h7b0_mdma_properties[] = {
    DEFINE_PROP_BOOL("timing-mode", GnwH7B0MdmaState, timing_mode, false),
    DEFINE_PROP_BOOL("x-test-profile", GnwH7B0MdmaState, test_profile, false),
    DEFINE_PROP_BOOL("x-mdma-axi1-v1-profile", GnwH7B0MdmaState,
                     mdma_axi1_v1_profile, false),
    DEFINE_PROP_BOOL("x-dma-engine-successor-r1-profile", GnwH7B0MdmaState,
                     dma_engine_successor_profile, false),
    DEFINE_PROP_BOOL("x-bus-v6-request-path", GnwH7B0MdmaState,
                     bus_v6_request_path, true),
    DEFINE_PROP_STRING("x-dma-engine-successor-r1-receipt-sha256",
                       GnwH7B0MdmaState, dma_engine_successor_receipt_sha256),
    DEFINE_PROP("x-test-clock-known", GnwH7B0MdmaState, test_clock_known,
                gnw_h7b0_mdma_test_known_prop, bool),
    DEFINE_PROP("x-test-clock-enabled", GnwH7B0MdmaState,
                test_clock_enabled, gnw_h7b0_mdma_test_enabled_prop, bool),
    DEFINE_PROP("x-test-clock-hz", GnwH7B0MdmaState, test_clock_hz,
                gnw_h7b0_mdma_test_clock_prop, uint32_t),
    DEFINE_PROP_UINT32("x-test-required-clock-hz", GnwH7B0MdmaState,
                       test_required_clock_hz, 0),
};

static void gnw_h7b0_mdma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &vmstate_gnw_h7b0_mdma;
    dc->realize = gnw_h7b0_mdma_realize;
    device_class_set_props(dc, gnw_h7b0_mdma_properties);
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
