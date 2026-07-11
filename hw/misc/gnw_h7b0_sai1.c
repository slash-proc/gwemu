/*
 * SAI1 -- see gnw_h7b0_sai1.h for the overall approach (DMA1 Stream0
 * transfer-complete snoop -> Fifo8 queue -> AUD_write pulled by
 * QEMU's own audio timer, no per-sample SAI FIFO modeling).
 *
 * Earlier versions called AUD_write() directly from the DMA tick
 * (gnw_h7b0_sai1_dma_notify), i.e. push-model delivery paced by our
 * approximated DMA timing. That's the wrong architecture: QEMU's
 * audio_run() drains a SW voice's *own* buffer at the real backend
 * rate on its own timer, entirely independent of when we happen to
 * call AUD_write -- so any drift between our DMA-tick cadence and
 * real playback rate was audible directly as pitch/speed distortion,
 * not just occasional under/overrun. Queuing into a Fifo8 and only
 * calling AUD_write from gnw_h7b0_sai1_voice_cb (invoked by
 * audio_run() itself, telling us exactly how many bytes it can accept
 * right now) lets QEMU's audio core govern the real-time rate like it
 * does for every other AUD_* device -- our job becomes only "don't
 * starve the queue", not "get the timing exactly right ourselves".
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "hw/qdev-properties.h"
#include "hw/misc/gnw_h7b0_sai1.h"
#include "hw/misc/gnw_h7b0_regs_sai1.h"

#define SAI_xCR1_SAIEN  (1U << 16)
#define SAI_xCR1_DMAEN  (1U << 17)
#define SAI_xCR1_MCKDIV_SHIFT 20
#define SAI_xCR1_MCKDIV_MASK  (0x3fU << SAI_xCR1_MCKDIV_SHIFT)
#define SAI_xCR1_OSR          (1U << 26)

/* gw_audio.c only ever configures Block A for 16-bit I2S at
 * AUDIO_SAMPLE_RATE (gw_audio.h) -- see task summary; no need to
 * decode AFRCR/ASLOTR/ACR1 DS bits, this is the only mode used.
 *
 * Content is mono, not stereo-interleaved: main.c sets
 * hsai_BlockA1.Init.MonoStereoMode = SAI_MONOMODE (the SAI hardware
 * itself duplicates one data slot to both output channels), and every
 * porting mixer (main_*.c) confirms this by writing one sample per
 * audiobuffer_dma[] index (e.g. gwenesis main_gwenesis.c line 242,
 * sound_buffer[i] = ...), not interleaved L/R pairs. Opening the
 * AUD_* voice as stereo and feeding it these mono samples directly
 * scrambled consecutive samples across L/R -- audible as distortion,
 * not just the buffer-timing stutter from gnw_h7b0_dma.c's
 * approximated pacing. */
#define GNW_H7B0_SAI1_RATE 48000
#define GNW_H7B0_SAI1_CHANNELS 1

/*
 * Decode the actual configured sample rate from ACR1's MCKDIV/OSR bits
 * combined with RCC's live PLL2 state, instead of assuming a fixed
 * 48kHz -- see gnw_h7b0_rcc.h and the audio-jank investigation this
 * fixes: game-and-watch-retro-go-sd's odroid_audio.c reprograms PLL2
 * and MCKDIV at runtime per game core (e.g. 22050/32768/53050Hz), and
 * without this our DMA pacing and host audio backend voice both stayed
 * silently stuck at 48kHz for every core except NES (whose 48kHz
 * request happens to match the boot-time default).
 *
 * Formula (matches HAL_SAI_InitFrameClock's NODIV=0 case, which is the
 * only NoDivider setting this firmware ever uses -- see main.c's
 * hsai_BlockA1.Init.NoDivider = SAI_MASTERDIVIDER_ENABLE, meaning the
 * ACR1 NODIV bit is clear):
 *   rate = SAI1_kernel_clk / (MCKDIV * (OSR ? 512 : 256))
 * Verified bit-exact against HAL_RCCEx_GetPLL2ClockFreq's actual compiled
 * float-math disassembly (retro-go-temp/elf/gw_retro_go_bank1.elf) for the
 * PLL2 VCO/PLL2P computation itself.
 *
 * Known imprecise case: some cores' set_audio_frequency() (odroid_audio.c)
 * skip HAL's auto-MCKDIV computation and instead assign hsai->Init.Mckdiv
 * directly with a hand-picked constant (AudioFrequency left at the
 * SAI_AUDIO_FREQUENCY_MCKDIV sentinel) -- confirmed via live disassembly
 * that this build's Celeste core does this for its 22050Hz target
 * (Mckdiv=8, not the ~3 the auto-formula would derive from this PLL2
 * config), same as the already-known G&W-native/Genesis manual cases.
 * Those constants were apparently tuned empirically against real hardware
 * output, not from this formula, so decoding them back through this
 * formula won't always reproduce the exact intended Hz for those specific
 * cores -- but DMA pacing and the host audio voice both derive their rate
 * from this same decode, so they stay internally consistent with each
 * other either way: the dma_counter race this whole change targets (game
 * logic vs. audio DMA completion falling out of sync, see gnw_h7b0_dma.c)
 * is fixed regardless of whether the absolute Hz is spot-on, since that
 * race only depends on both sides agreeing on the same rate, not on the
 * rate being perfectly correct.
 *
 * Falls back to GNW_H7B0_SAI1_RATE if RCC isn't wired, the kernel
 * clock can't be determined (e.g. SAI1SEL not pointed at PLL2), or
 * MCKDIV reads back as 0 (would divide by zero) -- preserves today's
 * working NES behavior as the safety net rather than producing
 * garbage.
 */
static uint32_t gnw_h7b0_sai1_get_rate_hz(GnwH7B0Sai1State *s)
{
    uint32_t acr1 = s->regs[GNW_H7B0_SAI1_SAI_ACR1_OFFSET >> 2];
    uint32_t mckdiv = (acr1 & SAI_xCR1_MCKDIV_MASK) >> SAI_xCR1_MCKDIV_SHIFT;
    uint32_t kernel_hz;

    if (!s->rcc || mckdiv == 0) {
        return GNW_H7B0_SAI1_RATE;
    }

    kernel_hz = gnw_h7b0_rcc_get_sai1_kernel_hz(s->rcc);
    if (kernel_hz == 0) {
        return GNW_H7B0_SAI1_RATE;
    }

    return kernel_hz / (mckdiv * ((acr1 & SAI_xCR1_OSR) ? 512 : 256));
}

static void gnw_h7b0_sai1_dma_notify(void *opaque, bool half,
                                      uint32_t m0ar, uint32_t ndtr)
{
    GnwH7B0Sai1State *s = opaque;
    uint32_t regs_acr1 = s->regs[GNW_H7B0_SAI1_SAI_ACR1_OFFSET >> 2];

    if (!(regs_acr1 & SAI_xCR1_SAIEN) || !s->voice_open) {
        return;
    }

    /* ndtr is the stream's configured item count (halfwords, per
     * HAL_DMA_Init's PeriphDataAlignment=HALFWORD); half-transfer
     * covers the first ndtr/2 items, full-transfer the second half.
     * Each item is one int16_t sample (stereo-interleaved), so bytes
     * transferred this half = (ndtr/2) * 2. */
    uint32_t half_items = ndtr / 2;
    uint32_t half_bytes = half_items * sizeof(int16_t);
    uint32_t addr = m0ar + (half ? 0 : half_bytes);

    if (half_bytes == 0 || !s->fifo_inited) {
        return;
    }

    if (half_bytes > GNW_H7B0_SAI1_FIFO_CAPACITY) {
        /*
         * Pathological NDTR (bigger than this device's whole queue) --
         * clamp rather than let fifo8_drop()/push_all() below violate
         * their preconditions. Real firmware never configures a single
         * DMA half anywhere near this size; this is a defensive bound,
         * not an expected path.
         */
        half_bytes = GNW_H7B0_SAI1_FIFO_CAPACITY;
    }

    if (half_bytes > fifo8_num_free(&s->fifo)) {
        /*
         * Queue has backed up (guest producing faster than the real
         * backend is draining, e.g. after a host hiccup) -- drop the
         * oldest queued audio to make room rather than growing latency
         * unboundedly or overflowing fifo8_push_all().
         */
        fifo8_drop(&s->fifo, half_bytes - fifo8_num_free(&s->fifo));
    }

    g_autofree uint8_t *buf = g_malloc(half_bytes);
    cpu_physical_memory_read(addr, buf, half_bytes);
    fifo8_push_all(&s->fifo, buf, half_bytes);
}

/*
 * Called by QEMU's own audio_run() on its real-time timer, telling us
 * exactly how many bytes the backend can accept right now -- this
 * (not the DMA tick) is what should govern actual playback rate. See
 * file header for why pushing from the DMA tick directly caused
 * pitch/speed distortion instead of just occasional glitches.
 */
static void gnw_h7b0_sai1_voice_cb(void *opaque, int avail)
{
    GnwH7B0Sai1State *s = opaque;

    while (avail > 0 && !fifo8_is_empty(&s->fifo)) {
        uint32_t chunk;
        /*
         * fifo8_pop_bufptr()'s max must not exceed what's actually
         * queued (fifo8.h's documented precondition) -- avail is what
         * the *backend* can accept, which is routinely more than our
         * queue currently holds (e.g. right after SAIEN, before the
         * queue has filled up), so it has to be clamped here or this
         * aborts.
         */
        uint32_t max = MIN((uint32_t)avail, fifo8_num_used(&s->fifo));
        const uint8_t *ptr = fifo8_pop_bufptr(&s->fifo, max, &chunk);
        int n = AUD_write(s->voice, (void *)ptr, chunk);

        if (n <= 0) {
            break;
        }
        avail -= n;
    }
}

/* Wrapper matching GnwH7B0DmaStreamRateFn's signature, registered with
 * the DMA controller so its half/full-transfer pacing tracks the real
 * configured rate instead of assuming a flat item rate -- see
 * gnw_h7b0_sai1_get_rate_hz() above and gnw_h7b0_dma.c's use of it. */
static uint32_t gnw_h7b0_sai1_dma_rate_fn(void *opaque)
{
    return gnw_h7b0_sai1_get_rate_hz(opaque);
}

static void gnw_h7b0_sai1_update_voice(GnwH7B0Sai1State *s)
{
    uint32_t regs_acr1 = s->regs[GNW_H7B0_SAI1_SAI_ACR1_OFFSET >> 2];
    bool want_enabled = (regs_acr1 & SAI_xCR1_SAIEN) != 0;

    if (want_enabled && !s->voice_open) {
        struct audsettings as = {
            .freq = gnw_h7b0_sai1_get_rate_hz(s),
            .nchannels = GNW_H7B0_SAI1_CHANNELS,
            .fmt = AUDIO_FORMAT_S16,
            .endianness = AUDIO_HOST_ENDIANNESS,
        };

        s->voice = AUD_open_out(&s->card, s->voice, "gnw-h7b0-sai1", s,
                                 gnw_h7b0_sai1_voice_cb, &as);
        if (s->voice) {
            AUD_set_active_out(s->voice, 1);
            s->voice_open = true;
            if (s->dma) {
                gnw_h7b0_dma_set_stream_notifier(s->dma,
                    GNW_H7B0_SAI1_DMA_STREAM, gnw_h7b0_sai1_dma_notify, s);
                gnw_h7b0_dma_set_stream_rate_fn(s->dma,
                    GNW_H7B0_SAI1_DMA_STREAM, gnw_h7b0_sai1_dma_rate_fn, s);
            }
        }
    } else if (!want_enabled && s->voice_open) {
        AUD_set_active_out(s->voice, 0);
        s->voice_open = false;
        fifo8_reset(&s->fifo);
        if (s->dma) {
            gnw_h7b0_dma_set_stream_notifier(s->dma,
                GNW_H7B0_SAI1_DMA_STREAM, NULL, NULL);
            gnw_h7b0_dma_set_stream_rate_fn(s->dma,
                GNW_H7B0_SAI1_DMA_STREAM, NULL, NULL);
        }
    }
}

void gnw_h7b0_sai1_set_dma(GnwH7B0Sai1State *s, GnwH7B0DmaState *dma)
{
    s->dma = dma;
    if (s->voice_open) {
        gnw_h7b0_dma_set_stream_notifier(s->dma, GNW_H7B0_SAI1_DMA_STREAM,
                                          gnw_h7b0_sai1_dma_notify, s);
        gnw_h7b0_dma_set_stream_rate_fn(s->dma, GNW_H7B0_SAI1_DMA_STREAM,
                                         gnw_h7b0_sai1_dma_rate_fn, s);
    }
}

void gnw_h7b0_sai1_set_rcc(GnwH7B0Sai1State *s, GnwH7B0RccState *rcc)
{
    s->rcc = rcc;
}

static void gnw_h7b0_sai1_reset(DeviceState *dev)
{
    GnwH7B0Sai1State *s = GNW_H7B0_SAI1(dev);
    for (int i = 0; i < (GNW_H7B0_SAI1_SIZE / 4); i++) {
        s->regs[i] = get_sai1_reset_value(i * 4);
    }
    if (s->voice_open) {
        AUD_set_active_out(s->voice, 0);
        s->voice_open = false;
        fifo8_reset(&s->fifo);
        if (s->dma) {
            gnw_h7b0_dma_set_stream_notifier(s->dma,
                GNW_H7B0_SAI1_DMA_STREAM, NULL, NULL);
            gnw_h7b0_dma_set_stream_rate_fn(s->dma,
                GNW_H7B0_SAI1_DMA_STREAM, NULL, NULL);
        }
    }
}

#define SAI_xSR_FLVL_FULL (4U << 16)

static uint64_t gnw_h7b0_sai1_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0Sai1State *s = GNW_H7B0_SAI1(opaque);
    if (addr >= GNW_H7B0_SAI1_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return 0;
    }

    /* HAL_SAI_Transmit_DMA busy-waits on ASR/BSR's FLVL field going
     * non-empty right after setting CR1.DMAEN, before it ever enables
     * SAIEN (see stm32h7xx_hal_sai.c's HAL_SAI_Transmit_DMA) -- same
     * "firmware polls a status flag our stub never sets" pattern as
     * SPI1 RXP/JPEG EOCF/etc (see docs/session-2026-07-10-part3).
     * With FLVL permanently 0 (reset value), that wait never ends and
     * SAIEN -- and therefore all audio output -- never happens. Once
     * DMAEN is set, report the FIFO as full; real hardware would have
     * it filling continuously off the DMA request line we don't model
     * at that granularity. */
    if (addr == GNW_H7B0_SAI1_SAI_ASR_OFFSET &&
        (s->regs[GNW_H7B0_SAI1_SAI_ACR1_OFFSET >> 2] & SAI_xCR1_DMAEN)) {
        return s->regs[addr >> 2] | SAI_xSR_FLVL_FULL;
    }
    if (addr == GNW_H7B0_SAI1_SAI_BSR_OFFSET &&
        (s->regs[GNW_H7B0_SAI1_SAI_BCR1_OFFSET >> 2] & SAI_xCR1_DMAEN)) {
        return s->regs[addr >> 2] | SAI_xSR_FLVL_FULL;
    }

    return s->regs[addr >> 2];
}

static void gnw_h7b0_sai1_write(void *opaque, hwaddr addr, uint64_t val64, unsigned int size)
{
    GnwH7B0Sai1State *s = GNW_H7B0_SAI1(opaque);
    if (addr >= GNW_H7B0_SAI1_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return;
    }
    uint32_t mask = get_sai1_write_mask(addr);
    s->regs[addr >> 2] = (s->regs[addr >> 2] & ~mask) | ((uint32_t)val64 & mask);

    if (addr == GNW_H7B0_SAI1_SAI_ACR1_OFFSET) {
        gnw_h7b0_sai1_update_voice(s);
    }
}

static const MemoryRegionOps gnw_h7b0_sai1_ops = {
    .read = gnw_h7b0_sai1_read,
    .write = gnw_h7b0_sai1_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void gnw_h7b0_sai1_init(Object *obj)
{
    GnwH7B0Sai1State *s = GNW_H7B0_SAI1(obj);
    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_sai1_ops, s, TYPE_GNW_H7B0_SAI1, GNW_H7B0_SAI1_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void gnw_h7b0_sai1_realize(DeviceState *dev, Error **errp)
{
    GnwH7B0Sai1State *s = GNW_H7B0_SAI1(dev);

    if (!AUD_register_card(TYPE_GNW_H7B0_SAI1, &s->card, errp)) {
        return;
    }
    fifo8_create(&s->fifo, GNW_H7B0_SAI1_FIFO_CAPACITY);
    s->fifo_inited = true;
}

static const VMStateDescription vmstate_gnw_h7b0_sai1 = {
    .name = TYPE_GNW_H7B0_SAI1,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0Sai1State, GNW_H7B0_SAI1_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static Property gnw_h7b0_sai1_properties[] = {
    DEFINE_AUDIO_PROPERTIES(GnwH7B0Sai1State, card),
    DEFINE_PROP_END_OF_LIST(),
};

static void gnw_h7b0_sai1_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &vmstate_gnw_h7b0_sai1;
    dc->realize = gnw_h7b0_sai1_realize;
    device_class_set_legacy_reset(dc, gnw_h7b0_sai1_reset);
    device_class_set_props(dc, gnw_h7b0_sai1_properties);
}

static const TypeInfo gnw_h7b0_sai1_info = {
    .name          = TYPE_GNW_H7B0_SAI1,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0Sai1State),
    .instance_init = gnw_h7b0_sai1_init,
    .class_init    = gnw_h7b0_sai1_class_init,
};

static void gnw_h7b0_sai1_register_types(void)
{
    type_register_static(&gnw_h7b0_sai1_info);
}
type_init(gnw_h7b0_sai1_register_types)
