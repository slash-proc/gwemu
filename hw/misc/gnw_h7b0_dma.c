/*
 * Auto-generated stub for DMA, extended with real per-stream transfer
 * completion.
 *
 * Was a plain read/write shadow with no side effects at all -- fine
 * for firmware that only pokes DMA registers without ever waiting on
 * completion, but game-and-watch-retro-go-sd's audio playback
 * (Core/Src/gw_audio.c) starts a circular-mode DMA1 Stream0 transfer
 * (SAI1 Tx) and then busy-waits on `dma_counter`, which is only
 * incremented by HAL_SAI_TxHalfCpltCallback/HAL_SAI_TxCpltCallback --
 * themselves only ever called from the DMA stream's real half/full
 * transfer-complete interrupt. With no completion modeled at all, that
 * wait never ends. Found via retro-go-bank1-flash.bin (a flash-only,
 * no-SD build) freezing solid on trying to start a game, right after a
 * burst of OSPI reads (loading the game/core) and RCC writes (turning
 * on audio-clock peripherals) -- confirmed via a real single-step
 * trace, not just async PC sampling (see the project's
 * feedback_pc_sample_before_screenshot memory) landing on the exact
 * same ~35-instruction busy-wait spinning on two RAM words that never
 * changed (both stuck at 0).
 *
 * Approximated as: on a stream's CR.EN 0->1 edge, schedule a
 * QEMUTimer for a "half transfer" delay derived from NDTR assuming a
 * flat 48kHz item rate (matching gw_audio.h's AUDIO_SAMPLE_RATE --
 * this device has no way to know which peripheral a stream's DMAMUX
 * request line actually points at, so it can't distinguish "this is
 * SAI1 audio" from any other DMA user; 48kHz is a reasonable universal
 * stand-in and is clamped to a sane range either way), set the
 * matching HTIF flag and fire the stream's IRQ if HTIE is set; on the
 * next tick set TCIF and fire if TCIE is set. In circular mode
 * (CR.CIRC) the stream then restarts automatically (real hardware
 * behavior for continuous audio-style DMA); otherwise CR.EN is
 * cleared, matching a real one-shot transfer completing.
 *
 * Originally a fixed 2ms delay regardless of NDTR -- unblocked the
 * hang above, but retro-go-bank1-flash.bin's game then ran at ~240fps
 * instead of ~60fps: retro-go paces emulated game speed off audio DMA
 * completion (AUDIO_BUFFER_LENGTH=1077 samples/48kHz ~= 22.4ms per
 * half, matching one real video frame per the "GWENESIS_AUDIO_BUFFER_
 * LENGTH_PAL" comment in gw_audio.h), and 2ms is ~11x faster than
 * that real 22.4ms -- same order of magnitude as the observed ~4x
 * speedup. Deriving the delay from NDTR fixes this for the actual
 * audio case without needing to model DMAMUX request routing.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "migration/vmstate.h"
#include "exec/cpu-common.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/gnw_h7b0_dma.h"
#include "hw/misc/gnw_h7b0_regs_dma.h"
#include "hw/misc/gnw_env.h"

/* getenv() is a locked linear scan on Windows (msvcrt) -- never call it
 * per-event in emulation-hot paths; resolve once and cache. */
static bool gnw_timer_late_enabled(void)
{
    static int v = -1;
    if (v < 0) {
        v = gnw_env_enabled("GNW_TIMER_LATE");
    }
    return v;
}

/*
 * GNW_DMA_TRACE: one DMA stream's event trace, for diagnosing the "IRQ storm
 * after quitting a retro-go game" class of bug and app-specific DMA cadence.
 * GNW_DMA_TRACE_STREAM selects the global stream index (0..15) and defaults
 * to DMA1 Stream0, preserving the original SAI1-audio trace behavior.
 *   =1  once-a-second summary counts only (safe to leave on)
 *   =2  one line per event (only for offline headless capture -- this
 *       project has repeatedly perturbed its own measurements with
 *       per-event stderr; never default it on)
 */
int gnw_dma_trace_level(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("GNW_DMA_TRACE");
        v = (e && *e && *e != '0') ? atoi(e) : 0;
    }
    return v;
}

static int gnw_dma_trace_stream(void)
{
    static int stream = -1;

    if (stream < 0) {
        const char *e = getenv("GNW_DMA_TRACE_STREAM");
        char *end = NULL;
        long parsed = e && *e ? strtol(e, &end, 0) : 0;

        /* Preserve the historical Stream0 default.  Invalid selectors are
         * ignored rather than changing guest behavior for a diagnostic
         * environment variable. */
        stream = end && !*end && parsed >= 0 &&
                 parsed < GNW_H7B0_DMA_STREAM_COUNT ? parsed : 0;
    }
    return stream;
}

static struct {
    unsigned set_ht, set_tc, clr_ht, clr_tc, cr_writes, irq_hi, irq_lo;
    unsigned have_actfn;   /* traced stream had a request predicate bound */
    int64_t t0;
} gnw_dma_trace_acc;

void gnw_dma_trace_event(const char *what, const char *fmt, ...)
{
    if (gnw_dma_trace_level() < 2) {
        return;
    }
    va_list ap;
    fprintf(stderr, "DMATRACE %12" PRId64 " %-6s ",
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), what);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void gnw_dma_trace_tick_summary(void)
{
    if (gnw_dma_trace_level() < 1) {
        return;
    }
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (now - gnw_dma_trace_acc.t0 < 1000000000LL) {
        return;
    }
    gnw_dma_trace_acc.t0 = now;
    fprintf(stderr, "DMASUM s%d setHT=%u setTC=%u clrHT=%u clrTC=%u "
            "crw=%u irq+=%u irq-=%u actfn=%u\n",
            gnw_dma_trace_stream(),
            gnw_dma_trace_acc.set_ht, gnw_dma_trace_acc.set_tc,
            gnw_dma_trace_acc.clr_ht, gnw_dma_trace_acc.clr_tc,
            gnw_dma_trace_acc.cr_writes, gnw_dma_trace_acc.irq_hi,
            gnw_dma_trace_acc.irq_lo, gnw_dma_trace_acc.have_actfn);
    memset(&gnw_dma_trace_acc, 0, sizeof(gnw_dma_trace_acc));
    gnw_dma_trace_acc.t0 = now;
}

#define GNW_H7B0_DMA_CTRL_SIZE      0x400
#define GNW_H7B0_DMA_STREAM_STRIDE  0x18
#define GNW_H7B0_DMA_S0CR_OFFSET    GNW_H7B0_DMA1_S0CR_OFFSET

#define DMA_SxCR_EN    (1U << 0)
#define DMA_SxCR_HTIE  (1U << 3)
#define DMA_SxCR_TCIE  (1U << 4)
#define DMA_SxCR_DIR         (0x3U << 6)
#define DMA_SxCR_DIR_M2M     (0x2U << 6)
#define DMA_SxCR_CIRC  (1U << 8)
#define DMA_SxCR_PINC  (1U << 9)
#define DMA_SxCR_MINC  (1U << 10)
#define DMA_SxCR_PSIZE_SHIFT 11
#define DMA_SxCR_PSIZE       (0x3U << DMA_SxCR_PSIZE_SHIFT)
#define DMA_SxCR_MSIZE_SHIFT 13
#define DMA_SxCR_MSIZE       (0x3U << DMA_SxCR_MSIZE_SHIFT)
#define DMA_SxCR_DBM   (1U << 18)
#define DMA_SxCR_CT    (1U << 19)

#define GNW_H7B0_DMA_ASSUMED_ITEM_RATE_HZ 48000ULL
#define GNW_H7B0_DMA_MIN_HALF_DELAY_NS (1 * SCALE_MS)
#define GNW_H7B0_DMA_MAX_HALF_DELAY_NS (500 * SCALE_MS)
#define GNW_H7B0_DMA_S0NDTR_OFFSET GNW_H7B0_DMA1_S0NDTR_OFFSET
#define GNW_H7B0_DMA_PHASE_DENOM UINT64_C(1000000000)
#define GNW_H7B0_DMA_LIVE_RATE_IDENTITY UINT32_C(0x444d4103)
#define GNW_H7B0_DMA_TEST_PROVIDER_IDENTITY UINT32_C(0x54455301)
#define GNW_H7B0_DMA_SETTLE_EVENT_BUDGET 64
#define GNW_H7B0_DMA_TEST_REQUEST_DEFAULT 125

static bool gnw_h7b0_dma_exact_settle(GnwH7B0DmaState *s, int stream,
                                       int64_t now);
static void gnw_h7b0_dma_exact_arm(GnwH7B0DmaState *s, int stream);
static void gnw_h7b0_dma_cache_request_state(GnwH7B0DmaState *s,
                                              int stream, int request);

/* Bit position within the owning ISR word (LISR for local streams 0-3,
 * HISR for local streams 4-7) of that stream's flag group's base --
 * standard STM32 DMA "6 bits per stream, 4-bit gap after every 2
 * streams" layout. TCIF is base+5, HTIF is base+4. */
static int gnw_h7b0_dma_isr_bit_base(int local_stream)
{
    static const int base[4] = { 0, 6, 16, 22 };
    return base[local_stream % 4];
}

static void gnw_h7b0_dma_update_irq(GnwH7B0DmaState *s, int stream)
{
    int ctrl = stream / GNW_H7B0_DMA_STREAMS_PER_CTRL;
    int local = stream % GNW_H7B0_DMA_STREAMS_PER_CTRL;
    hwaddr ctrl_base = (hwaddr)ctrl * GNW_H7B0_DMA_CTRL_SIZE;
    hwaddr isr_off = ctrl_base + ((local < 4) ? GNW_H7B0_DMA1_LISR_OFFSET
                                               : GNW_H7B0_DMA1_HISR_OFFSET);
    hwaddr cr_off = ctrl_base + GNW_H7B0_DMA_S0CR_OFFSET +
                     (hwaddr)local * GNW_H7B0_DMA_STREAM_STRIDE;
    int bit_base = gnw_h7b0_dma_isr_bit_base(local);
    uint32_t isr = s->regs[isr_off >> 2];
    uint32_t cr = s->regs[cr_off >> 2];
    bool htif = (isr >> (bit_base + 4)) & 1;
    bool tcif = (isr >> (bit_base + 5)) & 1;
    bool pending = (htif && (cr & DMA_SxCR_HTIE)) ||
                   (tcif && (cr & DMA_SxCR_TCIE));

    if (stream == gnw_dma_trace_stream() && gnw_dma_trace_level()) {
        static int last = -1;
        if (last != (int)pending) {
            last = pending;
            if (pending) {
                gnw_dma_trace_acc.irq_hi++;
            } else {
                gnw_dma_trace_acc.irq_lo++;
            }
            gnw_dma_trace_event("IRQ", "%s isr=0x%08x cr=0x%08x",
                                pending ? "ASSERT" : "deassert", isr, cr);
        }
    }

    qemu_set_irq(s->irq[stream], pending);
}

static void gnw_h7b0_dma_set_isr_bit(GnwH7B0DmaState *s, int stream,
                                     bool half)
{
    int ctrl = stream / GNW_H7B0_DMA_STREAMS_PER_CTRL;
    int local = stream % GNW_H7B0_DMA_STREAMS_PER_CTRL;
    hwaddr ctrl_base = (hwaddr)ctrl * GNW_H7B0_DMA_CTRL_SIZE;
    hwaddr isr_off = ctrl_base + ((local < 4) ? GNW_H7B0_DMA1_LISR_OFFSET
                                               : GNW_H7B0_DMA1_HISR_OFFSET);
    int bit_base = gnw_h7b0_dma_isr_bit_base(local);
    uint32_t bit = 1U << (bit_base + (half ? 4 : 5));

    if (stream == gnw_dma_trace_stream() && gnw_dma_trace_level()) {
        if (half) {
            gnw_dma_trace_acc.set_ht++;
        } else {
            gnw_dma_trace_acc.set_tc++;
        }
        gnw_dma_trace_event("SET", "%s", half ? "HTIF" : "TCIF");
    }

    s->regs[isr_off >> 2] |= bit;
    gnw_h7b0_dma_update_irq(s, stream);

    if (s->stream_notifier[stream]) {
        /* Stream register block layout: CR, NDTR, PAR, M0AR at +0x0,
         * +0x4, +0x8, +0xc from the stream's CR offset (see S0CR/
         * S0NDTR/S0PAR/S0M0AR offsets in the regs header). */
        hwaddr stream_base = ctrl_base + GNW_H7B0_DMA_S0CR_OFFSET +
                              (hwaddr)local * GNW_H7B0_DMA_STREAM_STRIDE;
        uint32_t cr = s->regs[stream_base >> 2];
        uint32_t ndtr = s->regs[(stream_base + 0x4) >> 2];
        /* In double-buffer mode the transfer in flight streams from
         * M1AR (+0x10) when CT is set, M0AR (+0xc) otherwise. */
        hwaddr mar_off = ((cr & DMA_SxCR_DBM) && (cr & DMA_SxCR_CT))
                          ? 0x10 : 0xc;
        uint32_t m0ar = s->regs[(stream_base + mar_off) >> 2];
        s->stream_notifier[stream](s->stream_notifier_opaque[stream], half,
                                    m0ar, ndtr);
    }
}

void gnw_h7b0_dma_set_stream_notifier(GnwH7B0DmaState *s, int stream,
                                       GnwH7B0DmaStreamNotifier cb,
                                       void *opaque)
{
    assert(stream >= 0 && stream < GNW_H7B0_DMA_STREAM_COUNT);
    s->stream_notifier[stream] = cb;
    s->stream_notifier_opaque[stream] = opaque;
}

void gnw_h7b0_dma_set_stream_rate_fn(GnwH7B0DmaState *s, int stream,
                                      GnwH7B0DmaStreamRateFn fn,
                                      void *opaque)
{
    assert(stream >= 0 && stream < GNW_H7B0_DMA_STREAM_COUNT);
    s->stream_rate_fn[stream] = fn;
    s->stream_rate_fn_opaque[stream] = opaque;
}

/* DMAMUX1's CxCR shadow block starts at the third 0x400 block (see the
 * header's layout comment); one CR per channel, DMAREQ_ID in bits 6:0. */
#define GNW_H7B0_DMAMUX_BLOCK_OFFSET 0x800
#define GNW_H7B0_DMAMUX_REQ_ID_MASK  0x7F

/*
 * Re-derive which stream each registered request-ID peripheral is bound
 * to from the current DMAMUX1 routing, and (re)attach the notifier/rate
 * callbacks there. Called on registration and on every DMAMUX CxCR
 * write, so the binding tracks firmware's actual routing.
 *
 * Iterates every live registration in req_reg[] -- this used to handle
 * only a single registration, which meant a second peripheral
 * registering (e.g. HASH, request 78) silently stomped the first's
 * (SAI1, request 87) fields, and SAI1 re-registering later (it does so
 * on every enable/disable) would then stomp back over HASH's, leaving
 * whichever one registered last as the only one that actually worked.
 */
static void gnw_h7b0_dma_rebind_request(GnwH7B0DmaState *s)
{
    for (int slot = 0; slot < GNW_H7B0_DMA_REQ_REG_COUNT; slot++) {
        GnwH7B0DmaReqReg *reg = &s->req_reg[slot];
        int stream = -1;

        if (reg->req_id < 0 || !reg->notifier) {
            continue;
        }

        for (int ch = 0; ch < GNW_H7B0_DMA_STREAM_COUNT; ch++) {
            uint32_t ccr = s->regs[(GNW_H7B0_DMAMUX_BLOCK_OFFSET
                                     + 4 * ch) >> 2];
            if ((int)(ccr & GNW_H7B0_DMAMUX_REQ_ID_MASK) == reg->req_id) {
                stream = ch;
                break;
            }
        }

        if (stream == reg->bound_stream) {
            continue;
        }
        if (reg->bound_stream >= 0) {
            if (s->stream_exact_timing[reg->bound_stream]) {
                gnw_h7b0_dma_exact_settle(
                    s, reg->bound_stream,
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
                s->stream_rate_state[reg->bound_stream] =
                    GNW_H7B0_DMA_RATE_STOPPED;
                s->stream_rate_num[reg->bound_stream] = 0;
                s->stream_rate_den[reg->bound_stream] = 0;
                s->stream_request_active[reg->bound_stream] = false;
                s->stream_transition_count[reg->bound_stream] = 0;
                s->stream_transition_head[reg->bound_stream] = 0;
                s->stream_transition_overflow[reg->bound_stream] = false;
                timer_del(s->stream_timer[reg->bound_stream]);
            }
            gnw_h7b0_dma_set_stream_notifier(s, reg->bound_stream, NULL, NULL);
            gnw_h7b0_dma_set_stream_rate_fn(s, reg->bound_stream, NULL, NULL);
            s->stream_low_latency[reg->bound_stream] = false;
            s->stream_active_fn[reg->bound_stream] = NULL;
            s->stream_active_fn_opaque[reg->bound_stream] = NULL;
            s->stream_state_fn[reg->bound_stream] = NULL;
            s->stream_state_fn_opaque[reg->bound_stream] = NULL;
        }
        if (stream >= 0) {
            gnw_h7b0_dma_set_stream_notifier(s, stream, reg->notifier,
                                              reg->notifier_opaque);
            gnw_h7b0_dma_set_stream_rate_fn(s, stream, reg->rate_fn,
                                             reg->rate_opaque);
            s->stream_low_latency[stream] = reg->low_latency;
            s->stream_active_fn[stream] = reg->active_fn;
            s->stream_active_fn_opaque[stream] = reg->active_opaque;
            s->stream_state_fn[stream] = reg->state_fn;
            s->stream_state_fn_opaque[stream] = reg->state_opaque;
            if (s->stream_exact_timing[stream] && reg->state_fn &&
                !s->suppress_rebind_state_refresh) {
                gnw_h7b0_dma_cache_request_state(s, stream, reg->req_id);
                gnw_h7b0_dma_exact_arm(s, stream);
            }
        }
        if (gnw_dma_trace_level() &&
            (stream == gnw_dma_trace_stream() ||
             reg->bound_stream == gnw_dma_trace_stream())) {
            gnw_dma_trace_event("BIND", "req %d: stream %d -> %d",
                                reg->req_id, reg->bound_stream, stream);
        }
        reg->bound_stream = stream;
    }
}

void gnw_h7b0_dma_set_request_notifier(GnwH7B0DmaState *s, int request,
                                        GnwH7B0DmaStreamNotifier cb,
                                        void *cb_opaque,
                                        GnwH7B0DmaStreamRateFn rate_fn,
                                        void *rate_opaque,
                                        bool low_latency)
{
    GnwH7B0DmaReqReg *reg = NULL;

    /* Reuse an existing slot for this request id if one's already
     * registered (re-registration, e.g. SAI1's enable/disable-time
     * calls, or a NULL-cb clear); otherwise claim the first free slot. */
    for (int slot = 0; slot < GNW_H7B0_DMA_REQ_REG_COUNT; slot++) {
        if (s->req_reg[slot].req_id == request) {
            reg = &s->req_reg[slot];
            break;
        }
    }
    if (!reg) {
        for (int slot = 0; slot < GNW_H7B0_DMA_REQ_REG_COUNT; slot++) {
            if (s->req_reg[slot].req_id < 0) {
                reg = &s->req_reg[slot];
                break;
            }
        }
    }
    if (!reg) {
        qemu_log_mask(LOG_GUEST_ERROR,
                       "gnw-h7b0-dma: no free request-notifier slot for "
                       "request %d (bump GNW_H7B0_DMA_REQ_REG_COUNT)\n",
                       request);
        return;
    }

    if (cb == NULL) {
        /* Clearing: fully free the slot so it doesn't shadow a future
         * registration for the same request id. */
        if (reg->bound_stream >= 0) {
            int old_stream = reg->bound_stream;

            if (s->stream_exact_timing[old_stream]) {
                gnw_h7b0_dma_exact_settle(
                    s, old_stream, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
                s->stream_rate_state[old_stream] =
                    GNW_H7B0_DMA_RATE_STOPPED;
                s->stream_rate_num[old_stream] = 0;
                s->stream_rate_den[old_stream] = 0;
                s->stream_request_active[old_stream] = false;
                s->stream_transition_count[old_stream] = 0;
                s->stream_transition_head[old_stream] = 0;
                s->stream_transition_overflow[old_stream] = false;
                timer_del(s->stream_timer[old_stream]);
            }
            gnw_h7b0_dma_set_stream_notifier(s, reg->bound_stream, NULL, NULL);
            gnw_h7b0_dma_set_stream_rate_fn(s, reg->bound_stream, NULL, NULL);
            s->stream_low_latency[reg->bound_stream] = false;
            s->stream_active_fn[reg->bound_stream] = NULL;
            s->stream_active_fn_opaque[reg->bound_stream] = NULL;
            s->stream_state_fn[reg->bound_stream] = NULL;
            s->stream_state_fn_opaque[reg->bound_stream] = NULL;
        }
        reg->active_fn = NULL;
        reg->active_opaque = NULL;
        reg->state_fn = NULL;
        reg->state_opaque = NULL;
        reg->req_id = -1;
        reg->notifier = NULL;
        reg->notifier_opaque = NULL;
        reg->rate_fn = NULL;
        reg->rate_opaque = NULL;
        reg->low_latency = false;
        reg->bound_stream = -1;
        return;
    }

    reg->req_id = request;
    reg->notifier = cb;
    reg->notifier_opaque = cb_opaque;
    reg->rate_fn = rate_fn;
    reg->rate_opaque = rate_opaque;
    reg->low_latency = low_latency;
    gnw_h7b0_dma_rebind_request(s);
}

bool gnw_h7b0_dma_get_request_stream_regs(GnwH7B0DmaState *s, int request,
                                           uint32_t *m0ar, uint32_t *ndtr)
{
    for (int slot = 0; slot < GNW_H7B0_DMA_REQ_REG_COUNT; slot++) {
        const GnwH7B0DmaReqReg *reg = &s->req_reg[slot];
        int stream = reg->bound_stream;
        int ctrl, local;
        hwaddr stream_base;
        uint32_t cr;
        hwaddr mar_off;

        if (reg->req_id != request || stream < 0) {
            continue;
        }

        ctrl = stream / GNW_H7B0_DMA_STREAMS_PER_CTRL;
        local = stream % GNW_H7B0_DMA_STREAMS_PER_CTRL;
        stream_base = (hwaddr)ctrl * GNW_H7B0_DMA_CTRL_SIZE +
                       GNW_H7B0_DMA_S0CR_OFFSET +
                       (hwaddr)local * GNW_H7B0_DMA_STREAM_STRIDE;
        cr = s->regs[stream_base >> 2];
        if (!(cr & DMA_SxCR_EN)) {
            return false;
        }
        /* Same M0AR/M1AR selection as gnw_h7b0_dma_set_isr_bit(). */
        mar_off = ((cr & DMA_SxCR_DBM) && (cr & DMA_SxCR_CT)) ? 0x10 : 0xc;
        *m0ar = s->regs[(stream_base + mar_off) >> 2];
        *ndtr = s->regs[(stream_base + 0x4) >> 2];
        return true;
    }
    return false;
}

void gnw_h7b0_dma_set_request_active_fn(GnwH7B0DmaState *s, int request,
                                         GnwH7B0DmaStreamActiveFn fn,
                                         void *opaque)
{
    for (int slot = 0; slot < GNW_H7B0_DMA_REQ_REG_COUNT; slot++) {
        if (s->req_reg[slot].req_id == request) {
            s->req_reg[slot].active_fn = fn;
            s->req_reg[slot].active_opaque = opaque;
            if (s->req_reg[slot].bound_stream >= 0) {
                s->stream_active_fn[s->req_reg[slot].bound_stream] = fn;
                s->stream_active_fn_opaque[s->req_reg[slot].bound_stream] =
                    opaque;
            }
            return;
        }
    }
}

static GnwH7B0DmaRequestState
gnw_h7b0_dma_validate_request_state(GnwH7B0DmaRequestState state,
                                     int request)
{
    bool valid = state.provider_identity != 0;

    switch (state.rate_state) {
    case GNW_H7B0_DMA_RATE_UNKNOWN:
    case GNW_H7B0_DMA_RATE_STOPPED:
        valid &= state.item_rate_num == 0 && state.item_rate_den == 0;
        break;
    case GNW_H7B0_DMA_RATE_RUNNING:
        valid &= state.item_rate_num != 0 && state.item_rate_den != 0;
        break;
    default:
        valid = false;
        break;
    }
    if (!valid) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gnw-h7b0-dma: request %d returned invalid typed "
                      "rate state %u/%u/%u identity 0x%08x; using unknown\n",
                      request, state.rate_state, state.item_rate_num,
                      state.item_rate_den,
                      state.provider_identity);
        state.rate_state = GNW_H7B0_DMA_RATE_UNKNOWN;
        state.item_rate_num = 0;
        state.item_rate_den = 0;
        state.provider_identity = 0;
    }
    return state;
}

static void gnw_h7b0_dma_cache_request_state(GnwH7B0DmaState *s,
                                              int stream, int request)
{
    GnwH7B0DmaRequestState state = {
        .rate_state = GNW_H7B0_DMA_RATE_UNKNOWN,
        .request_active = true,
    };

    if (s->stream_state_fn[stream]) {
        state = gnw_h7b0_dma_validate_request_state(
            s->stream_state_fn[stream](s->stream_state_fn_opaque[stream]),
            request);
    }
    s->stream_rate_state[stream] = state.rate_state;
    s->stream_rate_num[stream] = state.item_rate_num;
    s->stream_rate_den[stream] = state.item_rate_den;
    s->stream_request_active[stream] = state.request_active;
    s->stream_provider_identity[stream] = state.provider_identity;
}

static unsigned gnw_h7b0_dma_transition_slot(GnwH7B0DmaState *s,
                                              int stream, unsigned offset)
{
    return stream * GNW_H7B0_DMA_TRANSITION_QUEUE_DEPTH +
        ((s->stream_transition_head[stream] + offset) %
         GNW_H7B0_DMA_TRANSITION_QUEUE_DEPTH);
}

static bool gnw_h7b0_dma_enqueue_request_state(
    GnwH7B0DmaState *s, int stream, GnwH7B0DmaRequestState state,
    int64_t transition_ns)
{
    unsigned count = s->stream_transition_count[stream];
    unsigned slot;

    if (count) {
        slot = gnw_h7b0_dma_transition_slot(s, stream, count - 1);
        if (s->transition_ns[slot] == transition_ns) {
            /* Multiple provider register writes at one virtual timestamp
             * have no intervening cadence; keep only the final snapshot. */
            goto store;
        }
        if (s->transition_ns[slot] > transition_ns) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "gnw-h7b0-dma: stream %d provider transition "
                          "timestamps moved backwards\n", stream);
            return false;
        }
    }
    if (count == GNW_H7B0_DMA_TRANSITION_QUEUE_DEPTH) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gnw-h7b0-dma: stream %d provider transition queue "
                      "overflow; exact cadence paused\n", stream);
        s->stream_request_active[stream] = false;
        s->stream_transition_overflow[stream] = true;
        timer_del(s->stream_timer[stream]);
        return false;
    }
    slot = gnw_h7b0_dma_transition_slot(s, stream, count);
    s->stream_transition_count[stream] = count + 1;

store:
    s->transition_ns[slot] = transition_ns;
    s->pending_rate_state[slot] = state.rate_state;
    s->pending_rate_num[slot] = state.item_rate_num;
    s->pending_rate_den[slot] = state.item_rate_den;
    s->pending_request_active[slot] = state.request_active;
    s->pending_provider_identity[slot] = state.provider_identity;
    return true;
}

static int64_t gnw_h7b0_dma_first_transition_ns(GnwH7B0DmaState *s,
                                                 int stream)
{
    unsigned slot = gnw_h7b0_dma_transition_slot(s, stream, 0);

    g_assert(s->stream_transition_count[stream]);
    return s->transition_ns[slot];
}

static void gnw_h7b0_dma_apply_first_request_state(GnwH7B0DmaState *s,
                                                    int stream)
{
    unsigned slot = gnw_h7b0_dma_transition_slot(s, stream, 0);

    g_assert(s->stream_transition_count[stream]);
    s->stream_rate_state[stream] = s->pending_rate_state[slot];
    s->stream_rate_num[stream] = s->pending_rate_num[slot];
    s->stream_rate_den[stream] = s->pending_rate_den[slot];
    s->stream_request_active[stream] = s->pending_request_active[slot];
    s->stream_provider_identity[stream] = s->pending_provider_identity[slot];
    s->stream_transition_head[stream] =
        (s->stream_transition_head[stream] + 1) %
        GNW_H7B0_DMA_TRANSITION_QUEUE_DEPTH;
    s->stream_transition_count[stream]--;
}

void gnw_h7b0_dma_set_request_state_fn(GnwH7B0DmaState *s, int request,
                                        GnwH7B0DmaRequestStateFn fn,
                                        void *opaque)
{
    for (int slot = 0; slot < GNW_H7B0_DMA_REQ_REG_COUNT; slot++) {
        GnwH7B0DmaReqReg *reg = &s->req_reg[slot];

        if (reg->req_id != request) {
            continue;
        }
        reg->state_fn = fn;
        reg->state_opaque = opaque;
        if (reg->bound_stream >= 0) {
            int stream = reg->bound_stream;

            s->stream_state_fn[stream] = fn;
            s->stream_state_fn_opaque[stream] = opaque;
            if (s->stream_exact_timing[stream]) {
                gnw_h7b0_dma_request_state_changed(s, request);
            }
        }
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "gnw-h7b0-dma: typed state registered before request %d "
                  "notifier\n", request);
}

void gnw_h7b0_dma_request_state_changed(GnwH7B0DmaState *s, int request)
{
    for (int slot = 0; slot < GNW_H7B0_DMA_REQ_REG_COUNT; slot++) {
        GnwH7B0DmaReqReg *reg = &s->req_reg[slot];
        int stream = reg->bound_stream;

        if (reg->req_id != request || stream < 0 || !reg->state_fn) {
            continue;
        }
        if (s->stream_exact_timing[stream]) {
            int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            GnwH7B0DmaRequestState state =
                gnw_h7b0_dma_validate_request_state(
                    reg->state_fn(reg->state_opaque), request);

            /* Repeated writes at one virtual timestamp are one transition:
             * only the provider's final state can govern later time.  An
             * older outstanding transition is normally drained by its
             * already-due timer before virtual time can advance. */
            if (s->stream_transition_count[stream]) {
                gnw_h7b0_dma_enqueue_request_state(s, stream, state, now);
            } else if (gnw_h7b0_dma_exact_settle(s, stream, now)) {
                s->stream_rate_state[stream] = state.rate_state;
                s->stream_rate_num[stream] = state.item_rate_num;
                s->stream_rate_den[stream] = state.item_rate_den;
                s->stream_request_active[stream] = state.request_active;
                s->stream_provider_identity[stream] =
                    state.provider_identity;
            } else {
                gnw_h7b0_dma_enqueue_request_state(s, stream, state, now);
            }
            if (s->stream_exact_timing[stream]) {
                gnw_h7b0_dma_exact_arm(s, stream);
            }
        } else {
            gnw_h7b0_dma_cache_request_state(s, stream, request);
        }
        return;
    }
}

static uint64_t gnw_h7b0_dma_half_delay_ns(GnwH7B0DmaState *s, int stream)
{
    /* See gnw_h7b0_dma_set_request_notifier()'s low_latency parameter:
     * the flat-rate/1ms-floor pacing model below exists for perceptual
     * realism (audio), not correctness, and is actively wrong for a
     * bulk one-shot consumer like HASH_IN whose real transfer time is
     * microseconds -- skip straight to a near-instant, unclamped delay
     * for those. */
    if (s->stream_low_latency[stream]) {
        return 1; /* 1ns: effectively immediate, but still a real timer
                    * fire (not synchronous), preserving normal
                    * half-then-full notification ordering. */
    }

    /* Memory-to-memory streams (CR.DIR == M2M) have no DMAMUX request id
     * at all (DMA_REQUEST_MEM2MEM) for a peripheral to register a
     * low_latency/rate_fn notifier against, so they always fell through
     * to the 48kHz-audio-pacing model below -- wrong by ~1000x for bulk
     * M2M, whose real hardware transfer time is low-single-digit
     * microseconds, not tens of milliseconds (found via
     * stm32h7b0-diag's dma_m2m.md report: a 4KB/1024-item M2M transfer
     * was taking ~21ms simulated per one-shot copy under the audio
     * fallback, a ~395x QEMU-vs-hardware gap on case_dma1_m2m/
     * case_dma2_m2m). Unlike the request-id-keyed low_latency
     * registrations above, M2M mode is a per-stream CR bit firmware sets
     * directly -- read it straight from the stream's own CR, no
     * DMAMUX/notifier plumbing needed. */
    {
        int ctrl = stream / GNW_H7B0_DMA_STREAMS_PER_CTRL;
        int local = stream % GNW_H7B0_DMA_STREAMS_PER_CTRL;
        hwaddr cr_off = (hwaddr)ctrl * GNW_H7B0_DMA_CTRL_SIZE +
                         GNW_H7B0_DMA_S0CR_OFFSET +
                         (hwaddr)local * GNW_H7B0_DMA_STREAM_STRIDE;
        uint32_t cr = s->regs[cr_off >> 2];
        if ((cr & DMA_SxCR_DIR) == DMA_SxCR_DIR_M2M) {
            return 1;
        }
    }

    int ctrl = stream / GNW_H7B0_DMA_STREAMS_PER_CTRL;
    int local = stream % GNW_H7B0_DMA_STREAMS_PER_CTRL;
    hwaddr ndtr_off = (hwaddr)ctrl * GNW_H7B0_DMA_CTRL_SIZE +
                       GNW_H7B0_DMA_S0NDTR_OFFSET +
                       (hwaddr)local * GNW_H7B0_DMA_STREAM_STRIDE;
    uint32_t ndtr = s->regs[ndtr_off >> 2];
    uint32_t item_rate = GNW_H7B0_DMA_ASSUMED_ITEM_RATE_HZ;
    bool peripheral_rate = false;

    if (s->stream_rate_fn[stream]) {
        uint32_t real_rate = s->stream_rate_fn[stream](s->stream_rate_fn_opaque[stream]);
        if (real_rate != 0) {
            item_rate = real_rate;
            peripheral_rate = true;
        }
    }

    uint32_t items = ndtr / 2;

    if (s->timing_mode && peripheral_rate && !s->stream_half_pending[stream]) {
        items = ndtr - items;
    }
    uint64_t half_delay_ns = (uint64_t)items * NANOSECONDS_PER_SECOND /
                              item_rate;

    if (stream == gnw_dma_trace_stream()) {
        gnw_dma_trace_event("PACE", "ndtr=%u rate=%u", ndtr, item_rate);
    }

    /* A known peripheral cadence must not inherit host presentation clamps.
     * Unknown request rates and bulk transfers remain the existing fallback;
     * this does not assign an unmeasured bus-transfer latency. */
    if (s->timing_mode && peripheral_rate) {
        return MAX(half_delay_ns, 1);
    }
    half_delay_ns = MAX(half_delay_ns, GNW_H7B0_DMA_MIN_HALF_DELAY_NS);
    half_delay_ns = MIN(half_delay_ns, GNW_H7B0_DMA_MAX_HALF_DELAY_NS);
    return half_delay_ns;
}

/*
 * Advances this stream's tracked deadline by delay_ns and arms the timer
 * against that deadline -- NOT qemu_clock_get_ns()+delay_ns. Basing the
 * next fire on "now" at each tick bakes in that tick's host-scheduling
 * lateness permanently (the next deadline inherits however late THIS
 * callback happened to run), which compounds over the thousands of
 * half-transfer ticks/sec audio pacing needs into a systematically slow
 * and jittery/stuttery rate -- exactly the symptom reported once the
 * sample-rate math itself was already fixed. Clamping to "now" only when
 * we've fallen behind by more than one period avoids an unbounded
 * catch-up burst after e.g. a host stall (debugger attach, disk I/O),
 * while still not resetting the phase on every ordinary tick.
 */
/*
 * Streams with a request-activity predicate need the timer to run ahead
 * of their deadline, because a stall that starts and ends entirely
 * between two half-transfer ticks would otherwise go unnoticed: real
 * hardware loses that whole interval of transfer progress (and pushes
 * every subsequent HTIF/TCIF back by it), while a deadline-only timer
 * would sail straight through as if nothing had happened. retro-go's
 * SAI clock reconfiguration is ~1.4ms against a 33ms half-transfer
 * period, so this is exactly the case in practice.
 *
 * Only armed for streams that actually registered a predicate (SAI1's
 * audio stream today), so nothing else pays for it.
 */
#define GNW_H7B0_DMA_REQ_POLL_NS (250 * SCALE_US)

static int64_t gnw_h7b0_dma_rebase_time(int64_t value, int64_t delta)
{
    if (delta > 0 && value > INT64_MAX - delta) {
        return INT64_MAX;
    }
    if (delta < 0 && value < INT64_MIN - delta) {
        return INT64_MIN;
    }
    return value + delta;
}

static void gnw_h7b0_dma_arm(GnwH7B0DmaState *s, int stream)
{
    int64_t at = s->stream_deadline_ns[stream];

    if (s->stream_active_fn[stream]) {
        int64_t poll = gnw_h7b0_dma_rebase_time(
                           qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                           GNW_H7B0_DMA_REQ_POLL_NS);
        if (poll < at) {
            at = poll;
        }
    }
    timer_mod(s->stream_timer[stream], at);
}

static void gnw_h7b0_dma_schedule_next(GnwH7B0DmaState *s, int stream,
                                        int64_t delay_ns)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t next = gnw_h7b0_dma_rebase_time(
                       s->stream_deadline_ns[stream], delay_ns);

    if (next < now - delay_ns) {
        next = now;
    }
    s->stream_deadline_ns[stream] = next;
    gnw_h7b0_dma_arm(s, stream);
}

static uint32_t gnw_h7b0_dma_stream_ndtr(GnwH7B0DmaState *s, int stream)
{
    int ctrl = stream / GNW_H7B0_DMA_STREAMS_PER_CTRL;
    int local = stream % GNW_H7B0_DMA_STREAMS_PER_CTRL;
    hwaddr ndtr = (hwaddr)ctrl * GNW_H7B0_DMA_CTRL_SIZE +
                  GNW_H7B0_DMA_S0NDTR_OFFSET +
                  (hwaddr)local * GNW_H7B0_DMA_STREAM_STRIDE;

    return s->regs[ndtr >> 2] & 0xffff;
}

static uint32_t gnw_h7b0_dma_stream_cr(GnwH7B0DmaState *s, int stream,
                                        hwaddr *cr_off)
{
    int ctrl = stream / GNW_H7B0_DMA_STREAMS_PER_CTRL;
    int local = stream % GNW_H7B0_DMA_STREAMS_PER_CTRL;

    *cr_off = (hwaddr)ctrl * GNW_H7B0_DMA_CTRL_SIZE +
              GNW_H7B0_DMA_S0CR_OFFSET +
              (hwaddr)local * GNW_H7B0_DMA_STREAM_STRIDE;
    return s->regs[*cr_off >> 2];
}

/*
 * Complete one exact-cadence phase and install the next phase.  RM0455's
 * half-transfer threshold is reached when current NDTR becomes
 * floor(initial NDTR / 2): an odd N therefore takes ceil(N / 2) requests to
 * HTIF and floor(N / 2) more to TCIF.
 */
static bool gnw_h7b0_dma_exact_complete_phase(GnwH7B0DmaState *s, int stream)
{
    hwaddr cr_off;
    uint32_t cr = gnw_h7b0_dma_stream_cr(s, stream, &cr_off);
    uint32_t ndtr;

    if (!(cr & DMA_SxCR_EN)) {
        s->stream_exact_timing[stream] = false;
        return false;
    }
    if (s->stream_half_pending[stream]) {
        gnw_h7b0_dma_set_isr_bit(s, stream, true);
        cr = s->regs[cr_off >> 2];
        if (!(cr & DMA_SxCR_EN)) {
            s->stream_exact_timing[stream] = false;
            timer_del(s->stream_timer[stream]);
            return false;
        }
        ndtr = gnw_h7b0_dma_stream_ndtr(s, stream);
        s->stream_half_pending[stream] = false;
        s->stream_phase_items[stream] = ndtr / 2;
        return true;
    }

    gnw_h7b0_dma_set_isr_bit(s, stream, false);
    cr = s->regs[cr_off >> 2];
    if (cr & (DMA_SxCR_CIRC | DMA_SxCR_DBM)) {
        if (cr & DMA_SxCR_DBM) {
            s->regs[cr_off >> 2] = cr ^ DMA_SxCR_CT;
        }
        ndtr = gnw_h7b0_dma_stream_ndtr(s, stream);
        s->stream_half_pending[stream] = true;
        s->stream_phase_items[stream] = (ndtr + 1) / 2;
        return true;
    }

    s->regs[cr_off >> 2] = cr & ~DMA_SxCR_EN;
    s->stream_exact_timing[stream] = false;
    timer_del(s->stream_timer[stream]);
    return false;
}

static uint64_t gnw_h7b0_dma_gcd_u64(uint64_t a, uint64_t b)
{
    while (b) {
        uint64_t r = a % b;
        a = b;
        b = r;
    }
    return a;
}

/* Add elapsed time as an exact rational number of 1e-9-item units. */
static void gnw_h7b0_dma_phase_add(GnwH7B0DmaState *s, int stream,
                                    uint64_t elapsed)
{
    uint32_t rate_num = s->stream_rate_num[stream];
    uint32_t rate_den = s->stream_rate_den[stream];
    unsigned __int128 scaled = (unsigned __int128)elapsed * rate_num;
    unsigned __int128 whole128 = scaled / rate_den;
    uint64_t rem = scaled % rate_den;
    uint64_t old_num = s->stream_phase_subnum[stream];
    uint64_t old_den = s->stream_phase_subden[stream];
    uint64_t common_gcd = gnw_h7b0_dma_gcd_u64(old_den, rate_den);
    unsigned __int128 common_den =
        (unsigned __int128)(old_den / common_gcd) * rate_den;
    unsigned __int128 frac_num =
        (unsigned __int128)old_num * (rate_den / common_gcd) +
        (unsigned __int128)rem * (old_den / common_gcd);

    whole128 += frac_num / common_den;
    frac_num %= common_den;
    /* The caller never needs more progress than one DMA phase plus the
     * sub-nanosecond overshoot that selects its timer deadline.  Keep this
     * defensive saturation here so even hostile maximum-rate state cannot
     * narrow a u128 product through uint64_t and wrap phase progress. */
    if (whole128 > UINT64_MAX - s->stream_phase_progress_num[stream]) {
        s->stream_phase_progress_num[stream] = UINT64_MAX;
    } else {
        s->stream_phase_progress_num[stream] += (uint64_t)whole128;
    }
    if (frac_num == 0) {
        s->stream_phase_subnum[stream] = 0;
        s->stream_phase_subden[stream] = 1;
        return;
    }

    uint64_t reduced_num = frac_num;
    uint64_t reduced_den = common_den;
    uint64_t reduce = gnw_h7b0_dma_gcd_u64(reduced_num, reduced_den);

    reduced_num /= reduce;
    reduced_den /= reduce;
    if (reduced_den > UINT32_MAX) {
        /* Bounded approximation: the discarded fraction is below 1e-9 item.
         * Real providers use small related dividers; fail visibly rather
         * than overflow malformed/adversarial coprime test sequences. */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gnw-h7b0-dma: stream %d fractional denominator "
                      "overflow; discarded sub-nanounit residue\n", stream);
        s->stream_phase_subnum[stream] = 0;
        s->stream_phase_subden[stream] = 1;
    } else {
        s->stream_phase_subnum[stream] = reduced_num;
        s->stream_phase_subden[stream] = reduced_den;
    }
}

static void gnw_h7b0_dma_phase_normalize(GnwH7B0DmaState *s, int stream)
{
    uint64_t whole_items =
        s->stream_phase_progress_num[stream] / GNW_H7B0_DMA_PHASE_DENOM;
    uint64_t consume = MIN(whole_items, s->stream_phase_items[stream]);

    s->stream_phase_items[stream] -= consume;
    s->stream_phase_progress_num[stream] -=
        consume * GNW_H7B0_DMA_PHASE_DENOM;
}

static uint64_t gnw_h7b0_dma_phase_delay_ns(GnwH7B0DmaState *s, int stream)
{
    uint64_t subnum = s->stream_phase_subnum[stream];
    uint64_t subden = s->stream_phase_subden[stream];
    unsigned __int128 need_units =
        (unsigned __int128)s->stream_phase_items[stream] *
            GNW_H7B0_DMA_PHASE_DENOM -
        s->stream_phase_progress_num[stream];
    unsigned __int128 numerator =
        (need_units * subden - subnum) * s->stream_rate_den[stream];
    unsigned __int128 denominator =
        (unsigned __int128)subden * s->stream_rate_num[stream];
    unsigned __int128 delay = (numerator + denominator - 1) / denominator;

    return delay > INT64_MAX ? INT64_MAX : (uint64_t)delay;
}

static bool gnw_h7b0_dma_exact_settle(GnwH7B0DmaState *s, int stream,
                                       int64_t now)
{
    unsigned events = 0;

    if (!s->stream_exact_timing[stream] ||
        now < s->stream_last_update_ns[stream]) {
        return true;
    }
    if (s->stream_rate_state[stream] != GNW_H7B0_DMA_RATE_RUNNING ||
        !s->stream_request_active[stream]) {
        s->stream_last_update_ns[stream] = now;
        return true;
    }

    while (s->stream_exact_timing[stream]) {
        int64_t last = s->stream_last_update_ns[stream];
        uint64_t elapsed = now >= last ? (uint64_t)(now - last) : 0;

        gnw_h7b0_dma_phase_normalize(s, stream);
        if (s->stream_phase_items[stream] == 0) {
            events++;
            if (!gnw_h7b0_dma_exact_complete_phase(s, stream)) {
                return true;
            }
            if (events >= GNW_H7B0_DMA_SETTLE_EVENT_BUDGET) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "gnw-h7b0-dma: stream %d exact settlement "
                              "deferred after %u phase events\n",
                              stream, events);
                return s->stream_last_update_ns[stream] == now &&
                       s->stream_phase_items[stream] != 0;
            }
            continue;
        }

        uint64_t delta = gnw_h7b0_dma_phase_delay_ns(s, stream);
        if (elapsed < delta) {
            gnw_h7b0_dma_phase_add(s, stream, elapsed);
            gnw_h7b0_dma_phase_normalize(s, stream);
            s->stream_last_update_ns[stream] = now;
            return true;
        }

        gnw_h7b0_dma_phase_add(s, stream, delta);
        s->stream_last_update_ns[stream] =
            gnw_h7b0_dma_rebase_time(last, delta);
        gnw_h7b0_dma_phase_normalize(s, stream);
        g_assert(s->stream_phase_items[stream] == 0);
    }
    return true;
}

static void gnw_h7b0_dma_exact_arm(GnwH7B0DmaState *s, int stream)
{
    hwaddr cr_off;
    uint32_t cr = gnw_h7b0_dma_stream_cr(s, stream, &cr_off);

    if (!s->stream_exact_timing[stream] || !(cr & DMA_SxCR_EN)) {
        timer_del(s->stream_timer[stream]);
        return;
    }
    if (s->stream_transition_overflow[stream]) {
        /* Fail closed until an architectural stream disable or reset clears
         * the queue; never spin an already-due timer at this timestamp. */
        timer_del(s->stream_timer[stream]);
        return;
    }
    if (s->stream_transition_count[stream] &&
        (!s->stream_request_active[stream] ||
         s->stream_rate_state[stream] != GNW_H7B0_DMA_RATE_RUNNING)) {
        int64_t deadline = gnw_h7b0_dma_first_transition_ns(s, stream);

        s->stream_deadline_ns[stream] = deadline;
        timer_mod(s->stream_timer[stream], deadline);
        return;
    }
    if (!s->stream_request_active[stream] ||
        s->stream_rate_state[stream] == GNW_H7B0_DMA_RATE_STOPPED) {
        timer_del(s->stream_timer[stream]);
        return;
    }
    if (s->stream_rate_state[stream] == GNW_H7B0_DMA_RATE_UNKNOWN) {
        /* Preserve the legacy assumed-rate/clamp fallback exactly. */
        s->stream_deadline_ns[stream] = s->stream_last_update_ns[stream];
        gnw_h7b0_dma_schedule_next(
            s, stream, gnw_h7b0_dma_half_delay_ns(s, stream));
        return;
    }

    gnw_h7b0_dma_phase_normalize(s, stream);
    uint64_t delay = s->stream_phase_items[stream] == 0 ? 0 :
                     gnw_h7b0_dma_phase_delay_ns(s, stream);
    int64_t deadline = gnw_h7b0_dma_rebase_time(
        s->stream_last_update_ns[stream], MIN(delay, (uint64_t)INT64_MAX));

    if (s->stream_transition_count[stream]) {
        deadline = MIN(deadline,
                       gnw_h7b0_dma_first_transition_ns(s, stream));
    }

    s->stream_deadline_ns[stream] = deadline;
    if (deadline == INT64_MAX &&
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) == INT64_MAX) {
        /* No later virtual timestamp is representable; leave the exact phase
         * pending instead of repeatedly rearming an already-due timer. */
        timer_del(s->stream_timer[stream]);
        return;
    }
    timer_mod(s->stream_timer[stream], deadline);
}

static void gnw_h7b0_dma_stream_tick(void *opaque)
{
    GnwH7B0DmaStreamCtx *ctx = opaque;
    GnwH7B0DmaState *s = ctx->s;
    int stream = ctx->stream;

    if (gnw_timer_late_enabled()) {
        /*
         * Per-tick printing perturbs the guest badly on Windows (msvcrt
         * stderr is slow enough that 30-60 lines/s measurably slows
         * emulation -- it produced a completely bogus reading once).
         * Summarise once a second instead; the per-tick line is still
         * available at GNW_TIMER_LATE=2.
         */
        int64_t late = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                       s->stream_deadline_ns[stream];
        static int64_t t0;
        static uint64_t ticks;
        static int64_t late_sum, late_max;
        static int verbose = -1;
        if (verbose < 0) {
            const char *e = getenv("GNW_TIMER_LATE");
            verbose = (e && *e && atoi(e) >= 2);
        }
        if (verbose) {
            fprintf(stderr, "DMA%d late=%0.2fms\n", stream, late / 1e6);
        }
        ticks++;
        late_sum += late;
        if (late > late_max) {
            late_max = late;
        }
        int64_t now_rt = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        if (now_rt - t0 >= 1000000000LL) {
            fprintf(stderr, "DMALATE ticks/s=%" PRIu64 " avg=%.2fms "
                    "max=%.2fms\n", ticks,
                    ticks ? late_sum / 1e6 / ticks : 0.0, late_max / 1e6);
            t0 = now_rt;
            ticks = 0;
            late_sum = late_max = 0;
        }
    }
    if (stream == gnw_dma_trace_stream()) {
        gnw_dma_trace_acc.have_actfn = s->stream_active_fn[stream] ? 1 : 0;
    }
    gnw_dma_trace_tick_summary();

    int ctrl = stream / GNW_H7B0_DMA_STREAMS_PER_CTRL;
    int local = stream % GNW_H7B0_DMA_STREAMS_PER_CTRL;
    hwaddr ctrl_base = (hwaddr)ctrl * GNW_H7B0_DMA_CTRL_SIZE;
    hwaddr cr_off = ctrl_base + GNW_H7B0_DMA_S0CR_OFFSET +
                     (hwaddr)local * GNW_H7B0_DMA_STREAM_STRIDE;
    uint32_t cr = s->regs[cr_off >> 2];

    if (!(cr & DMA_SxCR_EN)) {
        return;
    }

    if (s->stream_exact_timing[stream]) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        if (s->stream_rate_state[stream] == GNW_H7B0_DMA_RATE_UNKNOWN &&
            s->stream_request_active[stream]) {
            unsigned zero_phases = 0;
            int64_t last = s->stream_last_update_ns[stream];
            int64_t elapsed = now >= last ?
                MIN((uint64_t)now - (uint64_t)last,
                    (uint64_t)INT64_MAX) : 0;

            s->stream_last_update_ns[stream] = now;

            /*
             * UNKNOWN deliberately retains the old integer rate callback,
             * clamp and request-predicate behavior.  gnw_h7b0_dma_arm()
             * may wake us at the 250us predicate poll before the phase
             * deadline; that wakeup observes activity only and must not
             * manufacture a completed half transfer.
             */
            if (s->stream_active_fn[stream] &&
                !s->stream_active_fn[stream](
                    s->stream_active_fn_opaque[stream])) {
                s->stream_deadline_ns[stream] = gnw_h7b0_dma_rebase_time(
                    s->stream_deadline_ns[stream], elapsed);
                gnw_h7b0_dma_arm(s, stream);
                return;
            }
            if (now < s->stream_deadline_ns[stream]) {
                gnw_h7b0_dma_arm(s, stream);
                return;
            }

            /* The unknown-rate compatibility timer denotes one complete
             * legacy phase; it intentionally has no inferred fraction. */
            s->stream_phase_items[stream] = 0;
            s->stream_phase_progress_num[stream] = 0;
            s->stream_phase_subnum[stream] = 0;
            s->stream_phase_subden[stream] = 1;
            if (gnw_h7b0_dma_exact_complete_phase(s, stream)) {
                while (s->stream_phase_items[stream] == 0 &&
                       zero_phases++ < 2 &&
                       gnw_h7b0_dma_exact_complete_phase(s, stream)) {
                    /* NDTR=1 has an immediate empty full phase. */
                }
            }
        } else {
            if (s->stream_transition_count[stream]) {
                int64_t transition =
                    gnw_h7b0_dma_first_transition_ns(s, stream);

                if (gnw_h7b0_dma_exact_settle(s, stream, transition)) {
                    if (s->stream_exact_timing[stream]) {
                        gnw_h7b0_dma_apply_first_request_state(s, stream);
                        if (!s->stream_transition_count[stream]) {
                            gnw_h7b0_dma_exact_settle(s, stream, now);
                        }
                    } else {
                        s->stream_transition_count[stream] = 0;
                        s->stream_transition_head[stream] = 0;
                    }
                }
            } else {
                gnw_h7b0_dma_exact_settle(s, stream, now);
            }
        }
        gnw_h7b0_dma_exact_arm(s, stream);
        return;
    }

    /*
     * The peripheral feeding this stream has stopped requesting (it was
     * disabled, or -- the case this exists for -- its kernel clock was
     * taken away). Real hardware's stream then simply makes no
     * progress: SxCR.EN stays set, NDTR stops decrementing, and no
     * HTIF/TCIF is raised until requests resume, at which point the
     * transfer picks up where it left off. Model that by sliding the
     * deadline forward by however long the stall lasted, rather than
     * letting virtual time run the transfer on regardless.
     *
     * Getting this wrong wedges the guest permanently: retro-go's
     * "quit to main menu" reprograms PLL2 (the SAI kernel clock source)
     * -- which per RCCEx_PLL2_Config() means switching PLL2 off, here
     * for ~1.4ms -- a couple of milliseconds before it disables the
     * stream and clears the flags, and its DMA1_Stream0 ISR no longer
     * acknowledges anything in that window. One manufactured
     * half-transfer interrupt there latches the level-triggered line
     * forever and the CPU tail-chains exception 27 for the rest of the
     * session: permanent black screen.
     */
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t last = s->stream_last_tick_ns[stream];
    int64_t elapsed = now >= last ?
        MIN((uint64_t)now - (uint64_t)last, (uint64_t)INT64_MAX) : 0;

    s->stream_last_tick_ns[stream] = now;
    if (elapsed < 0) {
        elapsed = 0;
    }

    if (s->stream_active_fn[stream]) {
        if (!s->stream_active_fn[stream](s->stream_active_fn_opaque[stream])) {
            s->stream_deadline_ns[stream] = gnw_h7b0_dma_rebase_time(
                s->stream_deadline_ns[stream], elapsed);
            if (stream == gnw_dma_trace_stream()) {
                gnw_dma_trace_event("STALL", "no request; deadline +%" PRId64
                                    "us", elapsed / 1000);
            }
            gnw_h7b0_dma_arm(s, stream);
            return;
        }
        if (now < s->stream_deadline_ns[stream]) {
            /* Just a poll: still requesting, not time for a flag yet. */
            gnw_h7b0_dma_arm(s, stream);
            return;
        }
    }

    if (s->stream_half_pending[stream]) {
        gnw_h7b0_dma_set_isr_bit(s, stream, true);
        s->stream_half_pending[stream] = false;
        gnw_h7b0_dma_schedule_next(s, stream,
                                    gnw_h7b0_dma_half_delay_ns(s, stream));
        return;
    }

    gnw_h7b0_dma_set_isr_bit(s, stream, false);

    if (cr & (DMA_SxCR_CIRC | DMA_SxCR_DBM)) {
        /*
         * Circular mode restarts the same buffer; double-buffer mode
         * (stock Zelda's audio path: DMA2 Stream6, SAI1_A) additionally
         * toggles CT at each transfer-complete so firmware's TC ISR can
         * refill the now-inactive MxAR while hardware streams the other
         * -- EN stays set in both, real hardware auto-reloads NDTR.
         * Treating DBM as one-shot (the old behavior) killed the stream
         * after its first 240-sample chime buffer and hung stock's
         * power-on/TIME transition state machine forever.
         */
        if (cr & DMA_SxCR_DBM) {
            s->regs[cr_off >> 2] = cr ^ DMA_SxCR_CT;
        }
        s->stream_half_pending[stream] = true;
        gnw_h7b0_dma_schedule_next(s, stream,
                                    gnw_h7b0_dma_half_delay_ns(s, stream));
    } else {
        s->regs[cr_off >> 2] = cr & ~DMA_SxCR_EN;
    }
}

/*
 * This device only ever models transfer timing/IRQs -- the actual byte
 * movement for every other stream use (SAI1 audio, HASH input) is done by
 * the consuming peripheral itself pulling straight from M0AR/NDTR via a
 * registered stream notifier, never by this DMA controller. Pure
 * memory-to-memory transfers (DMA_MEMORY_TO_MEMORY, no peripheral
 * involved at all -- diag suite's dma1_m2m/dma2_m2m cases) have no such
 * consumer, so nothing was ever actually copying guest memory: firmware's
 * completion wait was satisfied (a real bug fix, see the stream_tick
 * history above), but the destination buffer stayed all-zero, failing
 * every correctness check. Perform the real copy synchronously on the
 * EN 0->1 edge, matching HAL_DMA_SetConfig()'s M2M address convention
 * (PAR = source, M0AR = destination -- same as the P2M case, see
 * sdk/stm32h7xx-hal-driver/Src/stm32h7xx_hal_dma.c's DMA_SetConfig()).
 * Only the common PINC/MINC-enabled, equal-PSIZE/MSIZE case (the only
 * one any known firmware here uses) is implemented; anything else logs
 * unimplemented rather than silently doing the wrong thing.
 */
static void gnw_h7b0_dma_do_m2m_copy(GnwH7B0DmaState *s, int stream)
{
    int ctrl = stream / GNW_H7B0_DMA_STREAMS_PER_CTRL;
    int local = stream % GNW_H7B0_DMA_STREAMS_PER_CTRL;
    hwaddr stream_base = (hwaddr)ctrl * GNW_H7B0_DMA_CTRL_SIZE +
                          GNW_H7B0_DMA_S0CR_OFFSET +
                          (hwaddr)local * GNW_H7B0_DMA_STREAM_STRIDE;
    uint32_t cr = s->regs[stream_base >> 2];

    if ((cr & DMA_SxCR_DIR) != DMA_SxCR_DIR_M2M) {
        return;
    }

    uint32_t ndtr = s->regs[(stream_base + 0x4) >> 2];
    uint32_t par  = s->regs[(stream_base + 0x8) >> 2];
    uint32_t m0ar = s->regs[(stream_base + 0xc) >> 2];
    uint32_t psize = 1U << ((cr & DMA_SxCR_PSIZE) >> DMA_SxCR_PSIZE_SHIFT);
    uint32_t msize = 1U << ((cr & DMA_SxCR_MSIZE) >> DMA_SxCR_MSIZE_SHIFT);
    bool pinc = (cr & DMA_SxCR_PINC) != 0;
    bool minc = (cr & DMA_SxCR_MINC) != 0;

    if (psize != msize) {
        qemu_log_mask(LOG_UNIMP,
                      "gnw-h7b0-dma: M2M stream %d PSIZE(%u) != MSIZE(%u) "
                      "not implemented\n", stream, psize, msize);
        return;
    }

    for (uint32_t i = 0; i < ndtr; i++) {
        uint8_t buf[4];
        hwaddr src = par + (pinc ? (hwaddr)i * psize : 0);
        hwaddr dst = m0ar + (minc ? (hwaddr)i * msize : 0);

        cpu_physical_memory_read(src, buf, psize);
        cpu_physical_memory_write(dst, buf, msize);
    }
}

static void gnw_h7b0_dma_start_stream(GnwH7B0DmaState *s, int stream)
{
    gnw_h7b0_dma_do_m2m_copy(s, stream);

    if (s->timing_mode && s->stream_state_fn[stream] &&
        !s->stream_low_latency[stream]) {
        hwaddr cr_off;
        uint32_t cr = gnw_h7b0_dma_stream_cr(s, stream, &cr_off);
        uint32_t ndtr = gnw_h7b0_dma_stream_ndtr(s, stream);
        int request = -1;

        for (int slot = 0; slot < GNW_H7B0_DMA_REQ_REG_COUNT; slot++) {
            if (s->req_reg[slot].bound_stream == stream) {
                request = s->req_reg[slot].req_id;
                break;
            }
        }
        if ((cr & DMA_SxCR_DIR) != DMA_SxCR_DIR_M2M && ndtr != 0) {
            int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

            s->stream_exact_timing[stream] = true;
            s->stream_half_pending[stream] = true;
            s->stream_phase_items[stream] = (ndtr + 1) / 2;
            s->stream_phase_progress_num[stream] = 0;
            s->stream_phase_subnum[stream] = 0;
            s->stream_phase_subden[stream] = 1;
            s->stream_transition_head[stream] = 0;
            s->stream_transition_count[stream] = 0;
            s->stream_transition_overflow[stream] = false;
            s->stream_last_update_ns[stream] = now;
            s->stream_deadline_ns[stream] = now;
            gnw_h7b0_dma_cache_request_state(s, stream, request);
            gnw_h7b0_dma_exact_arm(s, stream);
            return;
        }
    }

    /*
     * A low_latency stream whose peripheral is not asserting its request
     * yet must NOT complete inline here: SPI1's DMA streams are enabled
     * (HAL_DMA_Start_IT) several register writes *before* CR1.CSTART
     * actually starts the transfer, so completing at EN time would move
     * bytes before the SPI is running and retire the transfer out of
     * order with HAL_SPI_TransmitReceive_DMA()'s own setup sequence.
     * Fall through to the timer path, which polls the predicate every
     * GNW_H7B0_DMA_REQ_POLL_NS and -- once active -- still uses this
     * stream's 1ns low_latency half delay, so it finishes promptly.
     */
    bool req_active = !s->stream_active_fn[stream] ||
        s->stream_active_fn[stream](s->stream_active_fn_opaque[stream]);

    if (s->stream_low_latency[stream] && req_active) {
        /*
         * Complete synchronously in this same call, no QEMUTimer round
         * trip at all -- see gnw_h7b0_dma_set_request_notifier()'s
         * low_latency parameter doc. A real (even a ~0ns) timer_mod()
         * still only fires on QEMU's next event-loop iteration, whose
         * timing depends on host scheduling; under real host
         * contention (this dev machine routinely runs several
         * concurrent qemu-gnw instances from other agents) that can
         * still lose the race against firmware's fixed CPU busy-spin
         * completion wait, confirmed via repeated live runs: the same
         * hash_*_dma/hmac_*_dma cases flip-flopped OK/FAIL run to run
         * with a mere 1ns-timer version of this fix in place. Doing the
         * whole half+full sequence inline removes the host-timing
         * dependency entirely for these consumers.
         */
        int ctrl = stream / GNW_H7B0_DMA_STREAMS_PER_CTRL;
        int local = stream % GNW_H7B0_DMA_STREAMS_PER_CTRL;
        hwaddr cr_off = (hwaddr)ctrl * GNW_H7B0_DMA_CTRL_SIZE +
                         GNW_H7B0_DMA_S0CR_OFFSET +
                         (hwaddr)local * GNW_H7B0_DMA_STREAM_STRIDE;
        uint32_t cr;

        gnw_h7b0_dma_set_isr_bit(s, stream, true);
        cr = s->regs[cr_off >> 2];
        gnw_h7b0_dma_set_isr_bit(s, stream, false);

        if (cr & (DMA_SxCR_CIRC | DMA_SxCR_DBM)) {
            /* No current low_latency consumer (HASH_IN, one-shot only)
             * uses circular/double-buffer mode; recursing synchronously
             * here to "auto-restart" would be an unbounded recursion
             * for a stream that never clears EN, so just log and leave
             * EN set for a real timer-driven restart instead of
             * pretending to support it. */
            qemu_log_mask(LOG_UNIMP,
                          "gnw-h7b0-dma: low_latency stream %d is "
                          "circular/DBM -- unsupported combination, "
                          "falling back to timer-paced restart\n", stream);
            if (cr & DMA_SxCR_DBM) {
                s->regs[cr_off >> 2] = cr ^ DMA_SxCR_CT;
            }
            s->stream_half_pending[stream] = true;
            s->stream_deadline_ns[stream] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            s->stream_last_tick_ns[stream] = s->stream_deadline_ns[stream];
            gnw_h7b0_dma_schedule_next(s, stream,
                                        gnw_h7b0_dma_half_delay_ns(s, stream));
        } else {
            s->regs[cr_off >> 2] = cr & ~DMA_SxCR_EN;
        }
        return;
    }

    s->stream_half_pending[stream] = true;
    s->stream_deadline_ns[stream] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->stream_last_tick_ns[stream] = s->stream_deadline_ns[stream];
    gnw_h7b0_dma_schedule_next(s, stream,
                                gnw_h7b0_dma_half_delay_ns(s, stream));
}

void gnw_h7b0_dma_kick_request(GnwH7B0DmaState *s, int request)
{
    for (int slot = 0; slot < GNW_H7B0_DMA_REQ_REG_COUNT; slot++) {
        const GnwH7B0DmaReqReg *reg = &s->req_reg[slot];
        int stream = reg->bound_stream;
        int ctrl, local;
        hwaddr cr_off;

        if (reg->req_id != request || stream < 0 || !s->stream_low_latency[stream]) {
            continue;
        }

        ctrl = stream / GNW_H7B0_DMA_STREAMS_PER_CTRL;
        local = stream % GNW_H7B0_DMA_STREAMS_PER_CTRL;
        cr_off = (hwaddr)ctrl * GNW_H7B0_DMA_CTRL_SIZE +
                  GNW_H7B0_DMA_S0CR_OFFSET +
                  (hwaddr)local * GNW_H7B0_DMA_STREAM_STRIDE;
        if (!(s->regs[cr_off >> 2] & DMA_SxCR_EN)) {
            return;
        }
        if (s->stream_active_fn[stream] &&
            !s->stream_active_fn[stream](s->stream_active_fn_opaque[stream])) {
            return;
        }

        /* Drop the pending stall-poll first: start_stream() completes a
         * low_latency stream inline and clears EN, and a stale timer
         * would then just re-enter the tick for a disabled stream. */
        timer_del(s->stream_timer[stream]);
        gnw_h7b0_dma_start_stream(s, stream);
        return;
    }
}

bool gnw_h7b0_dma_peripheral_request(GnwH7B0DmaState *s, int request)
{
    for (int stream = 0; stream < GNW_H7B0_DMA_STREAM_COUNT; stream++) {
        uint32_t route = s->regs[(GNW_H7B0_DMAMUX_BLOCK_OFFSET +
                                  4 * stream) >> 2];
        int ctrl = stream / GNW_H7B0_DMA_STREAMS_PER_CTRL;
        int local = stream % GNW_H7B0_DMA_STREAMS_PER_CTRL;
        hwaddr base = (hwaddr)ctrl * GNW_H7B0_DMA_CTRL_SIZE +
                      GNW_H7B0_DMA_S0CR_OFFSET +
                      (hwaddr)local * GNW_H7B0_DMA_STREAM_STRIDE;
        uint32_t cr = s->regs[base >> 2];

        if ((route & GNW_H7B0_DMAMUX_REQ_ID_MASK) != request ||
            !(cr & DMA_SxCR_EN)) {
            continue;
        }
        /* The v6 pacer is the exact one-item circular Stream7 transfer.
         * Other peripheral requests retain their existing timer/provider
         * implementations. */
        if (stream != 7 || !(cr & DMA_SxCR_CIRC) ||
            s->regs[(base + 0x4) >> 2] != 1) {
            return false;
        }
        gnw_h7b0_dma_set_isr_bit(s, stream, false);
        return true;
    }
    return false;
}

static void gnw_h7b0_dma_reset(DeviceState *dev)
{
    GnwH7B0DmaState *s = GNW_H7B0_DMA(dev);
    for (int i = 0; i < (GNW_H7B0_DMA_SIZE / 4); i++) {
        s->regs[i] = get_dma_reset_value(i * 4);
    }
    for (int i = 0; i < GNW_H7B0_DMA_STREAM_COUNT; i++) {
        timer_del(s->stream_timer[i]);
        s->stream_half_pending[i] = false;
        s->stream_deadline_ns[i] = 0;
        s->stream_last_tick_ns[i] = 0;
        s->stream_exact_timing[i] = false;
        s->stream_phase_items[i] = 0;
        s->stream_phase_progress_num[i] = 0;
        s->stream_rate_state[i] = GNW_H7B0_DMA_RATE_UNKNOWN;
        s->stream_rate_num[i] = 0;
        s->stream_rate_den[i] = 0;
        s->stream_phase_subnum[i] = 0;
        s->stream_phase_subden[i] = 1;
        s->stream_request_active[i] = false;
        s->stream_provider_identity[i] = 0;
        s->stream_last_update_ns[i] = 0;
        s->stream_transition_head[i] = 0;
        s->stream_transition_count[i] = 0;
        s->stream_transition_overflow[i] = false;
        qemu_set_irq(s->irq[i], 0);
    }
    /* DMAMUX routing reset to zero above; drop any request binding. */
    gnw_h7b0_dma_rebind_request(s);
}

static uint64_t gnw_h7b0_dma_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0DmaState *s = GNW_H7B0_DMA(opaque);
    if (addr >= GNW_H7B0_DMA_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return 0;
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_dma_write(void *opaque, hwaddr addr, uint64_t val64, unsigned int size)
{
    GnwH7B0DmaState *s = GNW_H7B0_DMA(opaque);
    if (addr >= GNW_H7B0_DMA_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return;
    }

    /* DMAMUX1 block: plain raw shadow (the DMA1 mask tables don't apply
     * to its layout), plus request-binding re-resolution on any CxCR
     * write so a request-registered peripheral (SAI1) tracks firmware's
     * actual routing. */
    if (addr >= GNW_H7B0_DMAMUX_BLOCK_OFFSET) {
        uint32_t mux_old = s->regs[addr >> 2];
        s->regs[addr >> 2] = (uint32_t)val64;
        if (addr < GNW_H7B0_DMAMUX_BLOCK_OFFSET +
                    4 * GNW_H7B0_DMA_STREAM_COUNT) {
            int ch = (int)((addr - GNW_H7B0_DMAMUX_BLOCK_OFFSET) / 4);
            if (ch == gnw_dma_trace_stream() && gnw_dma_trace_level()) {
                gnw_dma_trace_event("MUX", "ch%d old=0x%08x new=0x%08x "
                                    "reqid %u->%u", ch, mux_old,
                                    (uint32_t)val64,
                                    mux_old & GNW_H7B0_DMAMUX_REQ_ID_MASK,
                                    (uint32_t)val64 &
                                     GNW_H7B0_DMAMUX_REQ_ID_MASK);
            }
            gnw_h7b0_dma_rebind_request(s);
        }
        return;
    }

    /* DMA1 and DMA2 share an identical per-controller register layout
     * (see the header); reuse DMA1's offset->mask/reset tables for
     * DMA2 by masking down to the local per-controller offset. */
    uint32_t local_addr = addr % GNW_H7B0_DMA_CTRL_SIZE;
    uint32_t mask = get_dma_write_mask(local_addr);
    uint32_t old_value = s->regs[addr >> 2];
    s->regs[addr >> 2] = (old_value & ~mask) | ((uint32_t)val64 & mask);

    int ctrl = (int)(addr / GNW_H7B0_DMA_CTRL_SIZE);
    if (ctrl >= GNW_H7B0_DMA_CONTROLLER_COUNT) {
        return;
    }

    int trace_stream = gnw_dma_trace_stream();
    int trace_ctrl = trace_stream / GNW_H7B0_DMA_STREAMS_PER_CTRL;
    int trace_local = trace_stream % GNW_H7B0_DMA_STREAMS_PER_CTRL;
    hwaddr trace_base = GNW_H7B0_DMA_S0CR_OFFSET +
                        (hwaddr)trace_local * GNW_H7B0_DMA_STREAM_STRIDE;

    if (ctrl == trace_ctrl && local_addr > trace_base &&
        local_addr < trace_base + GNW_H7B0_DMA_STREAM_STRIDE &&
        gnw_dma_trace_level()) {
        gnw_dma_trace_event("SREG", "off=0x%02x old=0x%08x new=0x%08x",
                            (unsigned)(local_addr - trace_base), old_value,
                            s->regs[addr >> 2]);
    }

    if (local_addr == GNW_H7B0_DMA1_LIFCR_OFFSET ||
        local_addr == GNW_H7B0_DMA1_HIFCR_OFFSET) {
        /* Write-1-to-clear: LIFCR/HIFCR bit positions mirror
         * LISR/HISR's exactly. */
        hwaddr isr_off = (hwaddr)ctrl * GNW_H7B0_DMA_CTRL_SIZE +
            (local_addr == GNW_H7B0_DMA1_LIFCR_OFFSET
             ? GNW_H7B0_DMA1_LISR_OFFSET : GNW_H7B0_DMA1_HISR_OFFSET);
        bool trace_low = trace_local < 4;
        hwaddr trace_ifcr = trace_low ? GNW_H7B0_DMA1_LIFCR_OFFSET :
                                       GNW_H7B0_DMA1_HIFCR_OFFSET;
        if (ctrl == trace_ctrl && local_addr == trace_ifcr &&
            gnw_dma_trace_level()) {
            int bit_base = gnw_h7b0_dma_isr_bit_base(trace_local);
            uint32_t cleared = s->regs[isr_off >> 2] & (uint32_t)val64;
            if (cleared & (1U << (bit_base + 4))) {
                gnw_dma_trace_acc.clr_ht++;
            }
            if (cleared & (1U << (bit_base + 5))) {
                gnw_dma_trace_acc.clr_tc++;
            }
            gnw_dma_trace_event("IFCR", "wr=0x%08x cleared=0x%02x",
                                (uint32_t)val64,
                                (cleared >> bit_base) & 0x3f);
        }
        s->regs[isr_off >> 2] &= ~(uint32_t)val64;
        int base_stream = ctrl * GNW_H7B0_DMA_STREAMS_PER_CTRL +
                           (local_addr == GNW_H7B0_DMA1_LIFCR_OFFSET ? 0 : 4);
        for (int i = 0; i < 4; i++) {
            gnw_h7b0_dma_update_irq(s, base_stream + i);
        }
        return;
    }

    if (local_addr >= GNW_H7B0_DMA_S0CR_OFFSET &&
        (local_addr - GNW_H7B0_DMA_S0CR_OFFSET) % GNW_H7B0_DMA_STREAM_STRIDE == 0) {
        int local_stream = (int)((local_addr - GNW_H7B0_DMA_S0CR_OFFSET) /
                                  GNW_H7B0_DMA_STREAM_STRIDE);
        if (local_stream < GNW_H7B0_DMA_STREAMS_PER_CTRL) {
            int stream = ctrl * GNW_H7B0_DMA_STREAMS_PER_CTRL + local_stream;
            bool new_en = (val64 & DMA_SxCR_EN) != 0;
            bool old_en = (old_value & DMA_SxCR_EN) != 0;

            if (stream == gnw_dma_trace_stream() && gnw_dma_trace_level()) {
                uint32_t nv = s->regs[addr >> 2];
                gnw_dma_trace_acc.cr_writes++;
                gnw_dma_trace_event("CRW",
                    "old=0x%08x new=0x%08x EN %d->%d HTIE %d->%d TCIE %d->%d",
                    old_value, nv, old_en, new_en,
                    !!(old_value & DMA_SxCR_HTIE), !!(nv & DMA_SxCR_HTIE),
                    !!(old_value & DMA_SxCR_TCIE), !!(nv & DMA_SxCR_TCIE));
            }

            if (new_en && !old_en) {
                gnw_h7b0_dma_start_stream(s, stream);
            } else if (!new_en) {
                timer_del(s->stream_timer[stream]);
                s->stream_half_pending[stream] = false;
                s->stream_exact_timing[stream] = false;
                s->stream_phase_items[stream] = 0;
                s->stream_phase_progress_num[stream] = 0;
                s->stream_phase_subnum[stream] = 0;
                s->stream_phase_subden[stream] = 1;
                s->stream_provider_identity[stream] = 0;
                s->stream_transition_head[stream] = 0;
                s->stream_transition_count[stream] = 0;
                s->stream_transition_overflow[stream] = false;
            }
            gnw_h7b0_dma_update_irq(s, stream);
        }
    }
}

static const MemoryRegionOps gnw_h7b0_dma_ops = {
    .read = gnw_h7b0_dma_read,
    .write = gnw_h7b0_dma_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void gnw_h7b0_dma_init(Object *obj)
{
    GnwH7B0DmaState *s = GNW_H7B0_DMA(obj);
    s->migration_live_rate_identity = GNW_H7B0_DMA_LIVE_RATE_IDENTITY;
    s->test_request = GNW_H7B0_DMA_TEST_REQUEST_DEFAULT;
    s->test_rate_state = GNW_H7B0_DMA_RATE_RUNNING;
    s->test_rate_num = 64000;
    s->test_rate_den = 1;
    s->test_request_active = true;
    s->test_provider_identity = GNW_H7B0_DMA_TEST_PROVIDER_IDENTITY;
    for (int slot = 0; slot < GNW_H7B0_DMA_REQ_REG_COUNT; slot++) {
        s->req_reg[slot].req_id = -1;
        s->req_reg[slot].bound_stream = -1;
    }
    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_dma_ops, s, TYPE_GNW_H7B0_DMA, GNW_H7B0_DMA_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);

    for (int i = 0; i < GNW_H7B0_DMA_STREAM_COUNT; i++) {
        s->stream_ctx[i].s = s;
        s->stream_ctx[i].stream = i;
        s->stream_timer[i] = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                           gnw_h7b0_dma_stream_tick,
                                           &s->stream_ctx[i]);
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
}

static int gnw_h7b0_dma_pre_save(void *opaque)
{
    GnwH7B0DmaState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    for (int i = 0; i < GNW_H7B0_DMA_STREAM_COUNT; i++) {
        if (s->stream_exact_timing[i]) {
            if (s->stream_transition_overflow[i]) {
                continue;
            } else if (s->stream_transition_count[i]) {
                gnw_h7b0_dma_exact_settle(
                    s, i, gnw_h7b0_dma_first_transition_ns(s, i));
            } else {
                gnw_h7b0_dma_exact_settle(s, i, now);
            }
        }
    }

    s->migration_timing_mode = s->timing_mode;
    s->migration_clock_ns = now;
    s->migration_live_rate_identity = GNW_H7B0_DMA_LIVE_RATE_IDENTITY;
    return 0;
}


static bool gnw_h7b0_dma_post_load(void *opaque, int version_id, Error **errp)
{
    GnwH7B0DmaState *s = opaque;
    int64_t delta = 0;

    if (version_id >= 2 && s->migration_timing_mode != s->timing_mode) {
        error_setg(errp, "DMA timing mode differs between migration endpoints");
        return false;
    }
    if (version_id >= 2) {
        if (s->migration_clock_ns < 0) {
            error_setg(errp, "DMA migration has an invalid virtual clock epoch");
            return false;
        }
        delta = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->migration_clock_ns;
    }
    /* Host callback pointers are reconstructed from restored DMAMUX routing,
     * never serialized or invoked as completion notifications. */
    s->suppress_rebind_state_refresh = true;
    gnw_h7b0_dma_rebind_request(s);
    s->suppress_rebind_state_refresh = false;
    for (int i = 0; i < GNW_H7B0_DMA_STREAM_COUNT; i++) {
        int ctrl = i / GNW_H7B0_DMA_STREAMS_PER_CTRL;
        int local = i % GNW_H7B0_DMA_STREAMS_PER_CTRL;
        hwaddr cr = ctrl * GNW_H7B0_DMA_CTRL_SIZE +
                    GNW_H7B0_DMA_S0CR_OFFSET +
                    local * GNW_H7B0_DMA_STREAM_STRIDE;

        if (version_id < 2 &&
            (s->timing_mode || (s->regs[cr >> 2] & DMA_SxCR_EN))) {
            error_setg(errp, "DMA v1 migration lacks active stream timing state");
            return false;
        }
        if (version_id >= 2) {
            QEMUTimer *timer = s->stream_timer[i];

            s->stream_deadline_ns[i] =
                gnw_h7b0_dma_rebase_time(s->stream_deadline_ns[i], delta);
            s->stream_last_tick_ns[i] =
                gnw_h7b0_dma_rebase_time(s->stream_last_tick_ns[i], delta);
            if (timer_pending(timer)) {
                timer_mod(timer, gnw_h7b0_dma_rebase_time(
                              timer_expire_time_ns(timer), delta));
            }
        }
        if (version_id >= 3 && s->stream_exact_timing[i]) {
            GnwH7B0DmaRequestState current;
            uint32_t ndtr = gnw_h7b0_dma_stream_ndtr(s, i);
            uint32_t phase_limit = s->stream_half_pending[i] ?
                                   (ndtr + 1) / 2 : ndtr / 2;
            uint8_t state = s->stream_rate_state[i];

            s->stream_last_update_ns[i] = gnw_h7b0_dma_rebase_time(
                s->stream_last_update_ns[i], delta);
            if (version_id >= 4) {
                int64_t previous = s->stream_last_update_ns[i];
                unsigned count = s->stream_transition_count[i];

                if (s->stream_transition_head[i] >=
                        GNW_H7B0_DMA_TRANSITION_QUEUE_DEPTH ||
                    count > GNW_H7B0_DMA_TRANSITION_QUEUE_DEPTH ||
                    (s->stream_transition_overflow[i] &&
                     (count != GNW_H7B0_DMA_TRANSITION_QUEUE_DEPTH ||
                      s->stream_request_active[i]))) {
                    error_setg(errp, "DMA exact-rate stream %d has corrupt "
                               "transition queue", i);
                    return false;
                }
                for (unsigned offset = 0; offset < count; offset++) {
                    unsigned slot =
                        gnw_h7b0_dma_transition_slot(s, i, offset);
                    uint8_t pending_state = s->pending_rate_state[slot];

                    s->transition_ns[slot] = gnw_h7b0_dma_rebase_time(
                        s->transition_ns[slot], delta);
                    if (s->transition_ns[slot] < previous ||
                        pending_state > GNW_H7B0_DMA_RATE_RUNNING ||
                        (pending_state == GNW_H7B0_DMA_RATE_RUNNING) !=
                            (s->pending_rate_num[slot] != 0 &&
                             s->pending_rate_den[slot] != 0) ||
                        (pending_state != GNW_H7B0_DMA_RATE_RUNNING &&
                         (s->pending_rate_num[slot] != 0 ||
                          s->pending_rate_den[slot] != 0)) ||
                        s->pending_provider_identity[slot] == 0) {
                        error_setg(errp, "DMA exact-rate stream %d has "
                                   "corrupt queued transition", i);
                        return false;
                    }
                    previous = s->transition_ns[slot];
                }
            }
            if (!s->timing_mode || !(s->regs[cr >> 2] & DMA_SxCR_EN) ||
                !s->stream_state_fn[i]) {
                error_setg(errp, "DMA exact-rate stream %d has no matching "
                           "active destination provider", i);
                return false;
            }
            if (s->stream_phase_items[i] > phase_limit ||
                s->stream_phase_progress_num[i] >=
                    GNW_H7B0_DMA_PHASE_DENOM) {
                error_setg(errp, "DMA exact-rate stream %d has corrupt phase "
                           "state", i);
                return false;
            }
            if (state > GNW_H7B0_DMA_RATE_RUNNING ||
                (state == GNW_H7B0_DMA_RATE_RUNNING) !=
                    (s->stream_rate_num[i] != 0 &&
                     s->stream_rate_den[i] != 0) ||
                (state != GNW_H7B0_DMA_RATE_RUNNING &&
                 (s->stream_rate_num[i] != 0 ||
                  s->stream_rate_den[i] != 0)) ||
                s->stream_phase_subden[i] == 0 ||
                s->stream_phase_subnum[i] >= s->stream_phase_subden[i] ||
                s->stream_provider_identity[i] == 0) {
                error_setg(errp, "DMA exact-rate stream %d has corrupt rate "
                           "state", i);
                return false;
            }
            current = gnw_h7b0_dma_validate_request_state(
                s->stream_state_fn[i](s->stream_state_fn_opaque[i]), -1);
            uint32_t expected_identity = s->stream_provider_identity[i];

            if (version_id >= 4 && s->stream_transition_count[i]) {
                unsigned tail = gnw_h7b0_dma_transition_slot(
                    s, i, s->stream_transition_count[i] - 1);
                expected_identity = s->pending_provider_identity[tail];
            }
            if (current.provider_identity != expected_identity) {
                error_setg(errp, "DMA exact-rate stream %d provider identity "
                           "differs between migration endpoints", i);
                return false;
            }
            timer_del(s->stream_timer[i]);
            if (state == GNW_H7B0_DMA_RATE_UNKNOWN &&
                s->stream_request_active[i]) {
                /* UNKNOWN uses the legacy absolute phase deadline.  It was
                 * migrated and rebased above; exact_arm() is the helper that
                 * creates a *new* fallback phase and would replace the
                 * remaining source interval with a full interval here. */
                gnw_h7b0_dma_arm(s, i);
            } else {
                gnw_h7b0_dma_exact_arm(s, i);
            }
        }
        gnw_h7b0_dma_update_irq(s, i);
    }
    return true;
}

static const VMStateDescription vmstate_gnw_h7b0_dma = {
    .name = TYPE_GNW_H7B0_DMA,
    .version_id = 4,
    .minimum_version_id = 1,
    .pre_save = gnw_h7b0_dma_pre_save,
    .post_load_errp = gnw_h7b0_dma_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0DmaState, GNW_H7B0_DMA_SIZE / 4),
        VMSTATE_BOOL_V(migration_timing_mode, GnwH7B0DmaState, 2),
        VMSTATE_INT64_V(migration_clock_ns, GnwH7B0DmaState, 2),
        VMSTATE_BOOL_ARRAY_V(stream_half_pending, GnwH7B0DmaState,
                             GNW_H7B0_DMA_STREAM_COUNT, 2),
        VMSTATE_INT64_ARRAY_V(stream_deadline_ns, GnwH7B0DmaState,
                              GNW_H7B0_DMA_STREAM_COUNT, 2),
        VMSTATE_INT64_ARRAY_V(stream_last_tick_ns, GnwH7B0DmaState,
                              GNW_H7B0_DMA_STREAM_COUNT, 2),
        VMSTATE_ARRAY_OF_POINTER(stream_timer, GnwH7B0DmaState,
                                 GNW_H7B0_DMA_STREAM_COUNT, 2,
                                 vmstate_info_timer, QEMUTimer *),
        VMSTATE_UINT32_EQUAL_V(migration_live_rate_identity,
                               GnwH7B0DmaState, 3,
                               "DMA live-rate implementation identity "
                               "differs"),
        VMSTATE_BOOL_ARRAY_V(stream_exact_timing, GnwH7B0DmaState,
                             GNW_H7B0_DMA_STREAM_COUNT, 3),
        VMSTATE_UINT32_ARRAY_V(stream_phase_items, GnwH7B0DmaState,
                               GNW_H7B0_DMA_STREAM_COUNT, 3),
        VMSTATE_UINT64_ARRAY_V(stream_phase_progress_num, GnwH7B0DmaState,
                               GNW_H7B0_DMA_STREAM_COUNT, 3),
        VMSTATE_UINT8_ARRAY_V(stream_rate_state, GnwH7B0DmaState,
                              GNW_H7B0_DMA_STREAM_COUNT, 3),
        VMSTATE_UINT32_ARRAY_V(stream_rate_num, GnwH7B0DmaState,
                               GNW_H7B0_DMA_STREAM_COUNT, 3),
        VMSTATE_UINT32_ARRAY_V(stream_rate_den, GnwH7B0DmaState,
                               GNW_H7B0_DMA_STREAM_COUNT, 3),
        VMSTATE_UINT32_ARRAY_V(stream_phase_subnum, GnwH7B0DmaState,
                               GNW_H7B0_DMA_STREAM_COUNT, 3),
        VMSTATE_UINT32_ARRAY_V(stream_phase_subden, GnwH7B0DmaState,
                               GNW_H7B0_DMA_STREAM_COUNT, 3),
        VMSTATE_BOOL_ARRAY_V(stream_request_active, GnwH7B0DmaState,
                             GNW_H7B0_DMA_STREAM_COUNT, 3),
        VMSTATE_UINT32_ARRAY_V(stream_provider_identity, GnwH7B0DmaState,
                               GNW_H7B0_DMA_STREAM_COUNT, 3),
        VMSTATE_INT64_ARRAY_V(stream_last_update_ns, GnwH7B0DmaState,
                              GNW_H7B0_DMA_STREAM_COUNT, 3),
        VMSTATE_UINT8_ARRAY_V(stream_transition_head, GnwH7B0DmaState,
                              GNW_H7B0_DMA_STREAM_COUNT, 4),
        VMSTATE_UINT8_ARRAY_V(stream_transition_count, GnwH7B0DmaState,
                              GNW_H7B0_DMA_STREAM_COUNT, 4),
        VMSTATE_BOOL_ARRAY_V(stream_transition_overflow, GnwH7B0DmaState,
                             GNW_H7B0_DMA_STREAM_COUNT, 4),
        VMSTATE_INT64_ARRAY_V(transition_ns, GnwH7B0DmaState,
                              GNW_H7B0_DMA_TRANSITION_SLOT_COUNT, 4),
        VMSTATE_UINT8_ARRAY_V(pending_rate_state, GnwH7B0DmaState,
                              GNW_H7B0_DMA_TRANSITION_SLOT_COUNT, 4),
        VMSTATE_UINT32_ARRAY_V(pending_rate_num, GnwH7B0DmaState,
                               GNW_H7B0_DMA_TRANSITION_SLOT_COUNT, 4),
        VMSTATE_UINT32_ARRAY_V(pending_rate_den, GnwH7B0DmaState,
                               GNW_H7B0_DMA_TRANSITION_SLOT_COUNT, 4),
        VMSTATE_BOOL_ARRAY_V(pending_request_active, GnwH7B0DmaState,
                             GNW_H7B0_DMA_TRANSITION_SLOT_COUNT, 4),
        VMSTATE_UINT32_ARRAY_V(pending_provider_identity, GnwH7B0DmaState,
                               GNW_H7B0_DMA_TRANSITION_SLOT_COUNT, 4),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_dma_test_notify(void *opaque, bool half,
                                      uint32_t m0ar, uint32_t ndtr)
{
    /* Flags and real DMA MMIO are the observation surface in qtest. */
}

static GnwH7B0DmaRequestState gnw_h7b0_dma_test_state(void *opaque)
{
    GnwH7B0DmaState *s = opaque;

    return (GnwH7B0DmaRequestState) {
        .rate_state = s->test_rate_state,
        .item_rate_num = s->test_rate_state == GNW_H7B0_DMA_RATE_RUNNING ?
                         s->test_rate_num : 0,
        .item_rate_den = s->test_rate_state == GNW_H7B0_DMA_RATE_RUNNING ?
                         s->test_rate_den : 0,
        .request_active = s->test_request_active,
        .provider_identity = s->test_provider_identity,
    };
}

static void gnw_h7b0_dma_test_u32_set(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    const Property *prop = opaque;
    uint32_t *value = object_field_prop_ptr(obj, prop);

    if (!visit_type_uint32(v, name, value, errp)) {
        return;
    }
    GnwH7B0DmaState *s = GNW_H7B0_DMA(obj);
    if (s->test_provider) {
        gnw_h7b0_dma_request_state_changed(s, s->test_request);
    }
}

static void gnw_h7b0_dma_test_bool_set(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    const Property *prop = opaque;
    bool *value = object_field_prop_ptr(obj, prop);

    if (!visit_type_bool(v, name, value, errp)) {
        return;
    }
    GnwH7B0DmaState *s = GNW_H7B0_DMA(obj);
    if (s->test_provider) {
        gnw_h7b0_dma_request_state_changed(s, s->test_request);
    }
}

static const PropertyInfo gnw_h7b0_dma_test_u32_prop = {
    .type = "uint32",
    .set = gnw_h7b0_dma_test_u32_set,
    .realized_set_allowed = true,
};

static const PropertyInfo gnw_h7b0_dma_test_bool_prop = {
    .type = "bool",
    .set = gnw_h7b0_dma_test_bool_set,
    .realized_set_allowed = true,
};

static void gnw_h7b0_dma_realize(DeviceState *dev, Error **errp)
{
    GnwH7B0DmaState *s = GNW_H7B0_DMA(dev);

    if (!s->test_provider) {
        return;
    }
    if (s->test_request > GNW_H7B0_DMAMUX_REQ_ID_MASK) {
        error_setg(errp, "DMA x-test-request must fit DMAMUX DMAREQ_ID");
        return;
    }
    gnw_h7b0_dma_set_request_notifier(
        s, s->test_request, gnw_h7b0_dma_test_notify, s,
        NULL, NULL, false);
    gnw_h7b0_dma_set_request_state_fn(
        s, s->test_request, gnw_h7b0_dma_test_state, s);
}

static const Property gnw_h7b0_dma_properties[] = {
    DEFINE_PROP_BOOL("timing-mode", GnwH7B0DmaState, timing_mode, false),
    DEFINE_PROP_BOOL("x-test-provider", GnwH7B0DmaState,
                     test_provider, false),
    DEFINE_PROP_UINT32("x-test-request", GnwH7B0DmaState,
                       test_request, GNW_H7B0_DMA_TEST_REQUEST_DEFAULT),
    DEFINE_PROP("x-test-rate-state", GnwH7B0DmaState, test_rate_state,
                gnw_h7b0_dma_test_u32_prop, uint32_t),
    DEFINE_PROP("x-test-rate-num", GnwH7B0DmaState, test_rate_num,
                gnw_h7b0_dma_test_u32_prop, uint32_t),
    DEFINE_PROP("x-test-rate-den", GnwH7B0DmaState, test_rate_den,
                gnw_h7b0_dma_test_u32_prop, uint32_t),
    DEFINE_PROP("x-test-request-active", GnwH7B0DmaState,
                test_request_active, gnw_h7b0_dma_test_bool_prop, bool),
    DEFINE_PROP("x-test-provider-identity", GnwH7B0DmaState,
                test_provider_identity, gnw_h7b0_dma_test_u32_prop,
                uint32_t),
};

static void gnw_h7b0_dma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &vmstate_gnw_h7b0_dma;
    dc->realize = gnw_h7b0_dma_realize;
    device_class_set_props(dc, gnw_h7b0_dma_properties);
    device_class_set_legacy_reset(dc, gnw_h7b0_dma_reset);
}

static const TypeInfo gnw_h7b0_dma_info = {
    .name          = TYPE_GNW_H7B0_DMA,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0DmaState),
    .instance_init = gnw_h7b0_dma_init,
    .class_init    = gnw_h7b0_dma_class_init,
};

static void gnw_h7b0_dma_register_types(void)
{
    type_register_static(&gnw_h7b0_dma_info);
}
type_init(gnw_h7b0_dma_register_types)
