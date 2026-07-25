/*
 * Auto-generated stub for DMA, extended with real per-stream transfer
 * completion -- see gnw_h7b0_dma.c for rationale (this used to be a
 * plain shadow with no side effects at all, so any code waiting on a
 * DMA half/full-transfer-complete interrupt -- e.g. game-and-watch
 * retro-go-sd's audio_start_playing_full_length()/gw_audio.c, which
 * waits on dma_counter, incremented only by HAL_SAI_Tx{Half,}CpltCallback
 * -- hung forever).
 */
#ifndef HW_MISC_GNW_H7B0_DMA_H
#define HW_MISC_GNW_H7B0_DMA_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/timer.h"

#define TYPE_GNW_H7B0_DMA "gnw-h7b0-dma"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0DmaState, GNW_H7B0_DMA)

#define GNW_H7B0_DMA_SIZE 0xC00

/* DMA1 at block 0, DMA2 at block 1 (see soc.h/regs header: contiguous
 * 0x400-per-controller, DMAMUX1 at block 2 not modeled here -- plain
 * shadow, nothing waits on it). 8 streams per controller. */
#define GNW_H7B0_DMA_CONTROLLER_COUNT  2
#define GNW_H7B0_DMA_STREAMS_PER_CTRL  8
#define GNW_H7B0_DMA_STREAM_COUNT \
    (GNW_H7B0_DMA_CONTROLLER_COUNT * GNW_H7B0_DMA_STREAMS_PER_CTRL)

struct GnwH7B0DmaState;

typedef struct GnwH7B0DmaStreamCtx {
    struct GnwH7B0DmaState *s;
    int stream;   /* 0..GNW_H7B0_DMA_STREAM_COUNT-1 */
} GnwH7B0DmaStreamCtx;

/* Fired right after a stream's HTIF/TCIF flag is latched (and its IRQ
 * line updated), before the possible circular-mode restart. Lets a
 * peripheral that knows it owns a specific stream's DMA request line
 * (e.g. SAI1, for DMA1 Stream0 -- see gnw_h7b0_sai1.c) observe real
 * transfer-complete timing without this generic controller needing to
 * know about DMAMUX request routing at all. `m0ar`/`ndtr` are that
 * stream's current register values at fire time. */
typedef void (*GnwH7B0DmaStreamNotifier)(void *opaque, bool half,
                                          uint32_t m0ar, uint32_t ndtr);

/* Optional per-stream override for the item rate gnw_h7b0_dma_half_delay_ns()
 * assumes when pacing a stream's half/full-transfer timer -- lets a
 * peripheral that knows its own real configured rate (e.g. gnw_h7b0_sai1.c,
 * whose rate is reprogrammed at runtime via PLL2/MCKDIV, not fixed) correct
 * this generic controller's flat-rate assumption. Returning 0 means "don't
 * know right now", falling back to the flat assumption. */
typedef uint32_t (*GnwH7B0DmaStreamRateFn)(void *opaque);

/*
 * "Is the peripheral this stream is wired to actually requesting DMA
 * right now?"
 *
 * On real hardware a peripheral-flow stream transfers only when the
 * peripheral asserts its DMA request line (RM0455 DMA chapter, stream
 * configuration: the stream is a slave to the peripheral request), and
 * HTIF/TCIF are set purely as a consequence of data actually moving.
 * Kill the peripheral -- disable it, or take away its kernel clock --
 * and the stream simply stops making progress with SxCR.EN still set
 * and no further flags, indefinitely.
 *
 * This controller instead paces flags off a QEMUTimer keyed only on
 * SxCR.EN, which is the whole reason this hook exists: without it we
 * keep manufacturing half/full-transfer interrupts for a peripheral
 * firmware has already shut down. Returning false makes the stream
 * stall (no flag, no IRQ) exactly like hardware; the stream resumes
 * where it left off if the request source comes back.
 */
typedef bool (*GnwH7B0DmaStreamActiveFn)(void *opaque);

/* One request-ID-based registration slot -- see GnwH7B0DmaState's
 * req_reg[] comment for why this is an array, not a single set of
 * fields. */
typedef struct GnwH7B0DmaReqReg {
    int req_id; /* -1 = free slot */
    GnwH7B0DmaStreamNotifier notifier;
    void *notifier_opaque;
    GnwH7B0DmaStreamRateFn rate_fn;
    void *rate_opaque;
    GnwH7B0DmaStreamActiveFn active_fn;
    void *active_opaque;
    /* See gnw_h7b0_dma_set_request_notifier()'s low_latency parameter. */
    bool low_latency;
    int bound_stream; /* -1 = unbound */
} GnwH7B0DmaReqReg;

/* Max simultaneous request-ID registrations. Currently used by SAI1
 * (audio, request 87) and HASH (DMA-in, request 78); generously
 * headroomed for future DMA-capable peripherals. */
#define GNW_H7B0_DMA_REQ_REG_COUNT 4

struct GnwH7B0DmaState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_DMA_SIZE / 4];

    qemu_irq irq[GNW_H7B0_DMA_STREAM_COUNT];
    QEMUTimer *stream_timer[GNW_H7B0_DMA_STREAM_COUNT];
    GnwH7B0DmaStreamCtx stream_ctx[GNW_H7B0_DMA_STREAM_COUNT];
    /* true once the pending timer's next fire is the half-transfer
     * point; false once it's the full-transfer point. */
    bool stream_half_pending[GNW_H7B0_DMA_STREAM_COUNT];

    /* Ideal next-fire deadline (QEMU_CLOCK_VIRTUAL ns), tracked
     * independently of "now" -- see gnw_h7b0_dma_stream_tick()'s comment
     * for why: rescheduling from qemu_clock_get_ns() at each fire bakes
     * in that fire's host-scheduling lateness permanently, compounding
     * over thousands of ticks/sec into audible slowdown/stutter for
     * audio pacing. Advancing this from its own previous value instead
     * keeps the long-run average rate exact regardless of individual
     * callback jitter. */
    int64_t stream_deadline_ns[GNW_H7B0_DMA_STREAM_COUNT];

    /* Virtual time this stream's timer last ran, for accounting the
     * time a stalled stream spends making no progress -- see
     * GnwH7B0DmaStreamActiveFn and gnw_h7b0_dma_stream_tick(). */
    int64_t stream_last_tick_ns[GNW_H7B0_DMA_STREAM_COUNT];

    GnwH7B0DmaStreamNotifier stream_notifier[GNW_H7B0_DMA_STREAM_COUNT];
    void *stream_notifier_opaque[GNW_H7B0_DMA_STREAM_COUNT];

    GnwH7B0DmaStreamRateFn stream_rate_fn[GNW_H7B0_DMA_STREAM_COUNT];
    void *stream_rate_fn_opaque[GNW_H7B0_DMA_STREAM_COUNT];
    /* See GnwH7B0DmaStreamActiveFn. NULL = always requesting. */
    GnwH7B0DmaStreamActiveFn stream_active_fn[GNW_H7B0_DMA_STREAM_COUNT];
    void *stream_active_fn_opaque[GNW_H7B0_DMA_STREAM_COUNT];

    /* Per-stream mirror of the owning request registration's
     * low_latency flag (see gnw_h7b0_dma_set_request_notifier()) --
     * gnw_h7b0_dma_half_delay_ns() needs this indexed by stream, not
     * by request id. */
    bool stream_low_latency[GNW_H7B0_DMA_STREAM_COUNT];

    /*
     * Request-ID-based registration (see
     * gnw_h7b0_dma_set_request_notifier()): which DMAMUX1 request ID each
     * registered peripheral owns, and where its callbacks are currently
     * bound. Different firmware routes the same peripheral to different
     * streams (retro-go: SAI1_A on DMA1 Stream0/DMAMUX ch0; stock
     * Zelda: DMA2 Stream6/DMAMUX ch14), so the binding is re-resolved
     * from the DMAMUX1 CxCR shadow registers on every CxCR write
     * instead of being a compile-time stream number.
     *
     * This is a small fixed-size registry, not a single slot -- more
     * than one peripheral can have a live request-ID registration at
     * once (e.g. SAI1's audio request id 87 and HASH's DMA-in request
     * id 78 simultaneously). A single-slot version of this used to
     * exist and was a real bug: SAI1 re-registers itself on every
     * enable/disable (see gnw_h7b0_sai1.c), which silently clobbered
     * HASH's one-time boot-time registration, permanently breaking
     * hash_*_dma/hmac_*_dma firmware test cases (confirmed via live
     * tracing: the HASH DMA notifier never fired at all once SAI1's
     * audio path re-registered after boot).
     */
    GnwH7B0DmaReqReg req_reg[GNW_H7B0_DMA_REQ_REG_COUNT];
};

void gnw_h7b0_dma_set_stream_notifier(GnwH7B0DmaState *s, int stream,
                                       GnwH7B0DmaStreamNotifier cb,
                                       void *opaque);
void gnw_h7b0_dma_set_stream_rate_fn(GnwH7B0DmaState *s, int stream,
                                      GnwH7B0DmaStreamRateFn fn,
                                      void *opaque);

/*
 * Register (or clear, with cb == NULL) a transfer notifier + rate fn for
 * whichever stream firmware's DMAMUX1 routing assigns to `request`
 * (DMAMUX1 DMAREQ_ID, e.g. 87 = sai1_a_dma). DMAMUX channels 0-7 feed
 * DMA1 streams 0-7 (global stream index 0-7), channels 8-15 feed DMA2
 * streams 0-7 (global 8-15). The binding follows later CxCR rewrites
 * automatically.
 *
 * `low_latency`: gnw_h7b0_dma_half_delay_ns()'s pacing model (a flat
 * assumed item rate, floored at a 1ms-per-half minimum) exists purely
 * to make audio DMA (SAI1) sound right -- real hardware's actual
 * transfer rate is far higher, but pacing it realistically would
 * fire this controller's timer thousands of times a second for no
 * benefit. That same 1ms-per-half floor is wrong for a bulk, one-shot,
 * not-perceptually-paced consumer like HASH_IN: real hardware moves an
 * 8KB buffer over DMA in microseconds, and firmware waiting on such a
 * transfer (e.g. this project's diag suite's hash_*_dma cases) bounds
 * its wait with a fixed CPU busy-spin count -- under light host load,
 * TCG races through that spin count far faster than our artificial
 * 1ms-per-half floor, so the wait times out before the real transfer
 * "completes", intermittently (confirmed via repeated live runs: same
 * case flip-flopped OK/FAIL run to run with no code change in between,
 * the signature of a real timing race rather than a logic bug).
 * Pass true for consumers like this to skip the floor (and the flat-
 * rate assumption entirely) and complete near-instantly instead; pass
 * false for perceptually-paced consumers like SAI1 where the floor is
 * the intended behavior.
 */
/* GNW_DMA_TRACE diagnostics -- see gnw_h7b0_dma.c. */
int gnw_dma_trace_level(void);
void gnw_dma_trace_event(const char *what, const char *fmt, ...)
    G_GNUC_PRINTF(2, 3);

void gnw_h7b0_dma_set_request_notifier(GnwH7B0DmaState *s, int request,
                                        GnwH7B0DmaStreamNotifier cb,
                                        void *cb_opaque,
                                        GnwH7B0DmaStreamRateFn rate_fn,
                                        void *rate_opaque,
                                        bool low_latency);

/*
 * Attach a request-activity predicate (see GnwH7B0DmaStreamActiveFn) to
 * an already-registered request id. Separate from the call above so the
 * consumers that don't need it are untouched; must be called after each
 * gnw_h7b0_dma_set_request_notifier() for the same id, since clearing a
 * registration frees the whole slot.
 */
void gnw_h7b0_dma_set_request_active_fn(GnwH7B0DmaState *s, int request,
                                         GnwH7B0DmaStreamActiveFn fn,
                                         void *opaque);

#endif
