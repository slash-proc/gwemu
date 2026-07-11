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
#include "hw/irq.h"
#include "hw/misc/gnw_h7b0_dma.h"
#include "hw/misc/gnw_h7b0_regs_dma.h"

#define GNW_H7B0_DMA_CTRL_SIZE      0x400
#define GNW_H7B0_DMA_STREAM_STRIDE  0x18
#define GNW_H7B0_DMA_S0CR_OFFSET    GNW_H7B0_DMA1_S0CR_OFFSET

#define DMA_SxCR_EN    (1U << 0)
#define DMA_SxCR_HTIE  (1U << 3)
#define DMA_SxCR_TCIE  (1U << 4)
#define DMA_SxCR_CIRC  (1U << 8)

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
        uint32_t ndtr = s->regs[(stream_base + 0x4) >> 2];
        uint32_t m0ar = s->regs[(stream_base + 0xc) >> 2];
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

static uint64_t gnw_h7b0_dma_half_delay_ns(GnwH7B0DmaState *s, int stream)
{
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

    if (cr & DMA_SxCR_CIRC) {
        s->stream_half_pending[stream] = true;
        gnw_h7b0_dma_schedule_next(s, stream,
                                    gnw_h7b0_dma_half_delay_ns(s, stream));
    } else {
        s->regs[cr_off >> 2] = cr & ~DMA_SxCR_EN;
    }
}

static void gnw_h7b0_dma_start_stream(GnwH7B0DmaState *s, int stream)
{
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

static void gnw_h7b0_dma_class_init(ObjectClass *klass, void *data)
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
