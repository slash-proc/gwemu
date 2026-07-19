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
#include "migration/vmstate.h"
#include "exec/cpu-common.h"
#include "hw/core/irq.h"
#include "hw/misc/gnw_h7b0_dma.h"
#include "hw/misc/gnw_h7b0_regs_dma.h"

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
            gnw_h7b0_dma_set_stream_notifier(s, reg->bound_stream, NULL, NULL);
            gnw_h7b0_dma_set_stream_rate_fn(s, reg->bound_stream, NULL, NULL);
            s->stream_low_latency[reg->bound_stream] = false;
        }
        if (stream >= 0) {
            gnw_h7b0_dma_set_stream_notifier(s, stream, reg->notifier,
                                              reg->notifier_opaque);
            gnw_h7b0_dma_set_stream_rate_fn(s, stream, reg->rate_fn,
                                             reg->rate_opaque);
            s->stream_low_latency[stream] = reg->low_latency;
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
            gnw_h7b0_dma_set_stream_notifier(s, reg->bound_stream, NULL, NULL);
            gnw_h7b0_dma_set_stream_rate_fn(s, reg->bound_stream, NULL, NULL);
            s->stream_low_latency[reg->bound_stream] = false;
        }
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

    if (s->stream_rate_fn[stream]) {
        uint32_t real_rate = s->stream_rate_fn[stream](s->stream_rate_fn_opaque[stream]);
        if (real_rate != 0) {
            item_rate = real_rate;
        }
    }

    uint64_t half_delay_ns = (uint64_t)(ndtr / 2) * NANOSECONDS_PER_SECOND /
                              item_rate;

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
static void gnw_h7b0_dma_schedule_next(GnwH7B0DmaState *s, int stream,
                                        int64_t delay_ns)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t next = s->stream_deadline_ns[stream] + delay_ns;

    if (next < now - delay_ns) {
        next = now;
    }
    s->stream_deadline_ns[stream] = next;
    timer_mod(s->stream_timer[stream], next);
}

static void gnw_h7b0_dma_stream_tick(void *opaque)
{
    GnwH7B0DmaStreamCtx *ctx = opaque;
    GnwH7B0DmaState *s = ctx->s;
    int stream = ctx->stream;
    int ctrl = stream / GNW_H7B0_DMA_STREAMS_PER_CTRL;
    int local = stream % GNW_H7B0_DMA_STREAMS_PER_CTRL;
    hwaddr ctrl_base = (hwaddr)ctrl * GNW_H7B0_DMA_CTRL_SIZE;
    hwaddr cr_off = ctrl_base + GNW_H7B0_DMA_S0CR_OFFSET +
                     (hwaddr)local * GNW_H7B0_DMA_STREAM_STRIDE;
    uint32_t cr = s->regs[cr_off >> 2];

    if (!(cr & DMA_SxCR_EN)) {
        return;
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

    if (s->stream_low_latency[stream]) {
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
            gnw_h7b0_dma_schedule_next(s, stream,
                                        gnw_h7b0_dma_half_delay_ns(s, stream));
        } else {
            s->regs[cr_off >> 2] = cr & ~DMA_SxCR_EN;
        }
        return;
    }

    s->stream_half_pending[stream] = true;
    s->stream_deadline_ns[stream] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    gnw_h7b0_dma_schedule_next(s, stream,
                                gnw_h7b0_dma_half_delay_ns(s, stream));
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
        s->regs[addr >> 2] = (uint32_t)val64;
        if (addr < GNW_H7B0_DMAMUX_BLOCK_OFFSET +
                    4 * GNW_H7B0_DMA_STREAM_COUNT) {
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

    if (local_addr == GNW_H7B0_DMA1_LIFCR_OFFSET ||
        local_addr == GNW_H7B0_DMA1_HIFCR_OFFSET) {
        /* Write-1-to-clear: LIFCR/HIFCR bit positions mirror
         * LISR/HISR's exactly. */
        hwaddr isr_off = (hwaddr)ctrl * GNW_H7B0_DMA_CTRL_SIZE +
            (local_addr == GNW_H7B0_DMA1_LIFCR_OFFSET
             ? GNW_H7B0_DMA1_LISR_OFFSET : GNW_H7B0_DMA1_HISR_OFFSET);
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

            if (new_en && !old_en) {
                gnw_h7b0_dma_start_stream(s, stream);
            } else if (!new_en) {
                timer_del(s->stream_timer[stream]);
                s->stream_half_pending[stream] = false;
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

static const VMStateDescription vmstate_gnw_h7b0_dma = {
    .name = TYPE_GNW_H7B0_DMA,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0DmaState, GNW_H7B0_DMA_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_dma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &vmstate_gnw_h7b0_dma;
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
