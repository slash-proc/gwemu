/*
 * QEMU SDL3 audio driver
 *
 * Ported from the SDL2 driver (sdlaudio.c, (c) 2004-2005 Vassili Karpov)
 * for this fork: the GUI already ships a statically-linked SDL3, and SDL3
 * audio reaches the modern native API on every host (WASAPI on Windows,
 * CoreAudio on macOS, PipeWire/PulseAudio on Linux) -- one audiodev name
 * ("sdl3") that works identically everywhere, instead of per-OS backends
 * (and instead of dsound, whose pre-Vista API path failed outright on a
 * real Windows 11 guest).
 *
 * SDL3's stream API does all format/rate conversion internally, so unlike
 * the SDL2 driver there is no obtained-spec renegotiation: the stream is
 * opened with QEMU's requested format and SDL converts to whatever the
 * device wants.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include <SDL3/SDL.h>
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qemu/audio.h"
#include "qom/object.h"

#include "audio_int.h"

#define TYPE_AUDIO_SDL3 "audio-sdl3"
OBJECT_DECLARE_SIMPLE_TYPE(AudioSdl3, AUDIO_SDL3)

static AudioBackendClass *audio_sdl3_parent_class;

struct AudioSdl3 {
    AudioMixengBackend parent_obj;
};

typedef struct SDL3VoiceOut {
    HWVoiceOut hw;
    int exit;
    int initialized;
    Audiodev *dev;
    SDL_AudioStream *stream;
} SDL3VoiceOut;

typedef struct SDL3VoiceIn {
    HWVoiceIn hw;
    int exit;
    int initialized;
    Audiodev *dev;
    SDL_AudioStream *stream;
} SDL3VoiceIn;

static SDL_AudioFormat aud_to_sdl3fmt(AudioFormat fmt)
{
    switch (fmt) {
    case AUDIO_FORMAT_S8:
        return SDL_AUDIO_S8;
    case AUDIO_FORMAT_U8:
        return SDL_AUDIO_U8;
    case AUDIO_FORMAT_S16:
        return SDL_AUDIO_S16LE;
    case AUDIO_FORMAT_S32:
        return SDL_AUDIO_S32LE;
    case AUDIO_FORMAT_F32:
        return SDL_AUDIO_F32LE;
    /* SDL3 has no unsigned 16/32-bit formats -- use the signed
     * equivalent; sdl3_to_audfmt() reports the substitution back so
     * mixeng converts accordingly. */
    case AUDIO_FORMAT_U16:
        return SDL_AUDIO_S16LE;
    case AUDIO_FORMAT_U32:
        return SDL_AUDIO_S32LE;
    default:
        error_report("sdl3: internal logic error: bad audio format %d", fmt);
        return SDL_AUDIO_U8;
    }
}

/*
 * The requested format may be adjusted (U16/U32 -> S16) since SDL3 has no
 * unsigned 16/32-bit formats; report what was actually opened back into
 * obt_as so the mixeng conversion matches.
 */
static void sdl3_to_audfmt(SDL_AudioFormat sdlfmt, struct audsettings *as)
{
    as->big_endian = false;
    switch (sdlfmt) {
    case SDL_AUDIO_S8:
        as->fmt = AUDIO_FORMAT_S8;
        break;
    case SDL_AUDIO_U8:
        as->fmt = AUDIO_FORMAT_U8;
        break;
    case SDL_AUDIO_S16LE:
        as->fmt = AUDIO_FORMAT_S16;
        break;
    case SDL_AUDIO_S32LE:
        as->fmt = AUDIO_FORMAT_S32;
        break;
    case SDL_AUDIO_F32LE:
        as->fmt = AUDIO_FORMAT_F32;
        break;
    default:
        as->fmt = AUDIO_FORMAT_U8;
        break;
    }
}

static void sdl3_close_out(SDL3VoiceOut *sdl)
{
    if (sdl->initialized && sdl->stream) {
        SDL_LockAudioStream(sdl->stream);
        sdl->exit = 1;
        SDL_UnlockAudioStream(sdl->stream);
        SDL_PauseAudioStreamDevice(sdl->stream);
        sdl->initialized = 0;
    }
    if (sdl->stream) {
        SDL_DestroyAudioStream(sdl->stream);
        sdl->stream = NULL;
    }
}

/*
 * Playback feed: the device side of the stream wants additional_amount
 * more bytes -- hand over whatever the emulation has buffered, zero-fill
 * the rest (same underrun policy as the SDL2 driver's callback).
 */
static void sdl3_callback_out(void *opaque, SDL_AudioStream *stream,
                              int additional_amount, int total_amount)
{
    SDL3VoiceOut *sdl = opaque;
    HWVoiceOut *hw = &sdl->hw;
    int len = additional_amount;

    if (!sdl->exit) {
        while (hw->pending_emul && len) {
            size_t write_len, start;

            start = audio_ring_posb(hw->pos_emul, hw->pending_emul,
                                    hw->size_emul);
            assert(start < hw->size_emul);

            write_len = MIN(MIN(hw->pending_emul, (size_t)len),
                            hw->size_emul - start);

            SDL_PutAudioStreamData(stream, hw->buf_emul + start, write_len);
            hw->pending_emul -= write_len;
            len -= write_len;
        }
    }

    if (len) {
        size_t frames = len / hw->info.bytes_per_frame;
        if (frames) {
            g_autofree uint8_t *silence =
                g_malloc(frames * hw->info.bytes_per_frame);
            audio_pcm_info_clear_buf(&hw->info, silence, frames);
            SDL_PutAudioStreamData(stream, silence,
                                   frames * hw->info.bytes_per_frame);
        }
    }
}

static void sdl3_close_in(SDL3VoiceIn *sdl)
{
    if (sdl->initialized && sdl->stream) {
        SDL_LockAudioStream(sdl->stream);
        sdl->exit = 1;
        SDL_UnlockAudioStream(sdl->stream);
        SDL_PauseAudioStreamDevice(sdl->stream);
        sdl->initialized = 0;
    }
    if (sdl->stream) {
        SDL_DestroyAudioStream(sdl->stream);
        sdl->stream = NULL;
    }
}

/* Recording drain: the stream has captured data available -- pull it. */
static void sdl3_callback_in(void *opaque, SDL_AudioStream *stream,
                             int additional_amount, int total_amount)
{
    SDL3VoiceIn *sdl = opaque;
    HWVoiceIn *hw = &sdl->hw;

    if (sdl->exit) {
        return;
    }

    while (hw->pending_emul < hw->size_emul) {
        size_t read_len = MIN(hw->size_emul - hw->pos_emul,
                              hw->size_emul - hw->pending_emul);
        int got = SDL_GetAudioStreamData(stream, hw->buf_emul + hw->pos_emul,
                                         read_len);
        if (got <= 0) {
            break;
        }
        hw->pending_emul += got;
        hw->pos_emul = (hw->pos_emul + got) % hw->size_emul;
    }
}

#define SDL3_WRAPPER_FUNC(name, ret_type, args_decl, args, dir)  \
    static ret_type glue(sdl3_, name)args_decl                   \
    {                                                            \
        ret_type ret;                                            \
        glue(SDL3Voice, dir) *sdl = (glue(SDL3Voice, dir) *)hw;  \
                                                                 \
        SDL_LockAudioStream(sdl->stream);                        \
        ret = glue(audio_generic_, name)args;                    \
        SDL_UnlockAudioStream(sdl->stream);                      \
                                                                 \
        return ret;                                              \
    }

#define SDL3_WRAPPER_VOID_FUNC(name, args_decl, args, dir)       \
    static void glue(sdl3_, name)args_decl                       \
    {                                                            \
        glue(SDL3Voice, dir) *sdl = (glue(SDL3Voice, dir) *)hw;  \
                                                                 \
        SDL_LockAudioStream(sdl->stream);                        \
        glue(audio_generic_, name)args;                          \
        SDL_UnlockAudioStream(sdl->stream);                      \
    }

SDL3_WRAPPER_FUNC(buffer_get_free, size_t, (HWVoiceOut *hw), (hw), Out)
SDL3_WRAPPER_FUNC(get_buffer_out, void *, (HWVoiceOut *hw, size_t *size),
                  (hw, size), Out)
SDL3_WRAPPER_FUNC(put_buffer_out, size_t,
                  (HWVoiceOut *hw, void *buf, size_t size), (hw, buf, size),
                  Out)
SDL3_WRAPPER_FUNC(write, size_t,
                  (HWVoiceOut *hw, void *buf, size_t size), (hw, buf, size),
                  Out)
SDL3_WRAPPER_FUNC(read, size_t, (HWVoiceIn *hw, void *buf, size_t size),
                  (hw, buf, size), In)
SDL3_WRAPPER_FUNC(get_buffer_in, void *, (HWVoiceIn *hw, size_t *size),
                  (hw, size), In)
SDL3_WRAPPER_VOID_FUNC(put_buffer_in, (HWVoiceIn *hw, void *buf, size_t size),
                       (hw, buf, size), In)
#undef SDL3_WRAPPER_FUNC
#undef SDL3_WRAPPER_VOID_FUNC

static void sdl3_fini_out(HWVoiceOut *hw)
{
    sdl3_close_out((SDL3VoiceOut *)hw);
}

static int sdl3_init_out(HWVoiceOut *hw, struct audsettings *as)
{
    SDL3VoiceOut *sdl = (SDL3VoiceOut *)hw;
    SDL_AudioSpec req;
    Audiodev *dev = hw->s->dev;
    AudiodevSdlPerDirectionOptions *spdo = dev->u.sdl3.out;
    struct audsettings obt_as;
    uint32_t frames;

    req.freq = as->freq;
    req.format = aud_to_sdl3fmt(as->fmt);
    req.channels = as->nchannels;

    sdl->dev = dev;
    sdl->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                            &req, sdl3_callback_out, sdl);
    if (!sdl->stream) {
        error_report("sdl3: SDL_OpenAudioDeviceStream for playback "
                     "failed: %s", SDL_GetError());
        return -1;
    }

    obt_as = *as;
    sdl3_to_audfmt(req.format, &obt_as);
    audio_pcm_init_info(&hw->info, &obt_as);

    frames = audio_buffer_frames(
        qapi_AudiodevSdlPerDirectionOptions_base(spdo), &obt_as, 11610);
    hw->samples = (spdo->has_buffer_count ? spdo->buffer_count : 4) * frames;

    sdl->initialized = 1;
    sdl->exit = 0;
    return 0;
}

static void sdl3_enable_out(HWVoiceOut *hw, bool enable)
{
    SDL3VoiceOut *sdl = (SDL3VoiceOut *)hw;

    if (enable) {
        SDL_ResumeAudioStreamDevice(sdl->stream);
    } else {
        SDL_PauseAudioStreamDevice(sdl->stream);
    }
}

static void sdl3_fini_in(HWVoiceIn *hw)
{
    sdl3_close_in((SDL3VoiceIn *)hw);
}

static int sdl3_init_in(HWVoiceIn *hw, struct audsettings *as)
{
    SDL3VoiceIn *sdl = (SDL3VoiceIn *)hw;
    SDL_AudioSpec req;
    Audiodev *dev = hw->s->dev;
    AudiodevSdlPerDirectionOptions *spdo = dev->u.sdl3.in;
    struct audsettings obt_as;
    uint32_t frames;

    req.freq = as->freq;
    req.format = aud_to_sdl3fmt(as->fmt);
    req.channels = as->nchannels;

    sdl->dev = dev;
    sdl->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING,
                                            &req, sdl3_callback_in, sdl);
    if (!sdl->stream) {
        error_report("sdl3: SDL_OpenAudioDeviceStream for recording "
                     "failed: %s", SDL_GetError());
        return -1;
    }

    obt_as = *as;
    sdl3_to_audfmt(req.format, &obt_as);
    audio_pcm_init_info(&hw->info, &obt_as);

    frames = audio_buffer_frames(
        qapi_AudiodevSdlPerDirectionOptions_base(spdo), &obt_as, 11610);
    hw->samples = (spdo->has_buffer_count ? spdo->buffer_count : 4) * frames;
    hw->size_emul = hw->samples * hw->info.bytes_per_frame;
    hw->buf_emul = g_malloc(hw->size_emul);
    hw->pos_emul = hw->pending_emul = 0;

    sdl->initialized = 1;
    sdl->exit = 0;
    return 0;
}

static void sdl3_enable_in(HWVoiceIn *hw, bool enable)
{
    SDL3VoiceIn *sdl = (SDL3VoiceIn *)hw;

    if (enable) {
        SDL_ResumeAudioStreamDevice(sdl->stream);
    } else {
        SDL_PauseAudioStreamDevice(sdl->stream);
    }
}

static bool audio_sdl3_realize(AudioBackend *abe, Audiodev *dev, Error **errp)
{
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        error_setg(errp, "SDL3 failed to initialize audio subsystem: %s",
                   SDL_GetError());
        qapi_free_Audiodev(dev);
        return false;
    }

    return audio_sdl3_parent_class->realize(abe, dev, errp);
}

static void audio_sdl3_finalize(Object *obj)
{
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

static void audio_sdl3_class_init(ObjectClass *klass, const void *data)
{
    AudioBackendClass *b = AUDIO_BACKEND_CLASS(klass);
    AudioMixengBackendClass *k = AUDIO_MIXENG_BACKEND_CLASS(klass);

    audio_sdl3_parent_class =
        AUDIO_BACKEND_CLASS(object_class_get_parent(klass));

    b->realize = audio_sdl3_realize;
    k->max_voices_out = INT_MAX;
    k->max_voices_in = INT_MAX;
    k->voice_size_out = sizeof(SDL3VoiceOut);
    k->voice_size_in = sizeof(SDL3VoiceIn);

    k->init_out = sdl3_init_out;
    k->fini_out = sdl3_fini_out;
    k->write = sdl3_write;
    k->buffer_get_free = sdl3_buffer_get_free;
    k->get_buffer_out = sdl3_get_buffer_out;
    k->put_buffer_out = sdl3_put_buffer_out;
    k->enable_out = sdl3_enable_out;

    k->init_in = sdl3_init_in;
    k->fini_in = sdl3_fini_in;
    k->read = sdl3_read;
    k->get_buffer_in = sdl3_get_buffer_in;
    k->put_buffer_in = sdl3_put_buffer_in;
    k->enable_in = sdl3_enable_in;
}

static const TypeInfo audio_types[] = {
    {
        .name = TYPE_AUDIO_SDL3,
        .parent = TYPE_AUDIO_MIXENG_BACKEND,
        .instance_size = sizeof(AudioSdl3),
        .class_init = audio_sdl3_class_init,
        .instance_finalize = audio_sdl3_finalize,
    },
};

DEFINE_TYPES(audio_types)
module_obj(TYPE_AUDIO_SDL3);
