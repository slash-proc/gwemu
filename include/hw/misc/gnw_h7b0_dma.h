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

#include "hw/sysbus.h"
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

    GnwH7B0DmaStreamNotifier stream_notifier[GNW_H7B0_DMA_STREAM_COUNT];
    void *stream_notifier_opaque[GNW_H7B0_DMA_STREAM_COUNT];

    GnwH7B0DmaStreamRateFn stream_rate_fn[GNW_H7B0_DMA_STREAM_COUNT];
    void *stream_rate_fn_opaque[GNW_H7B0_DMA_STREAM_COUNT];
};

void gnw_h7b0_dma_set_stream_notifier(GnwH7B0DmaState *s, int stream,
                                       GnwH7B0DmaStreamNotifier cb,
                                       void *opaque);
void gnw_h7b0_dma_set_stream_rate_fn(GnwH7B0DmaState *s, int stream,
                                      GnwH7B0DmaStreamRateFn fn,
                                      void *opaque);

#endif
