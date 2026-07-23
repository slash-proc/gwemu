//
// GWemu User Interface
//
// Copyright (C) 2020-2022 Matt Borgerson
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// See gl-helpers.hh: formerly raw-OpenGL decal/FBO/shader helpers from
// xemu, now thin SDL_Renderer equivalents. The animated SDF logo shader
// was dropped along with GL -- the logo texture is drawn as a plain
// image (see widgets.cc's Logo()).
#include "gl-helpers.hh"
#include "common.hh"
#include "gwemu-hud.h"
#include <glib/gstdio.h>
#include "data/logo_sdf.png.h"
#include "notifications.hh"
#include "stb_image.h"
#include <fpng.h>
#include <math.h>
#include <stdio.h>
#include <vector>

SDL_Texture *g_logo_tex;

SDL_Texture *LoadTextureFromMemory(const unsigned char *buf,
                                   unsigned int size)
{
    int w, h, n;
    stbi_set_flip_vertically_on_load(0);
    unsigned char *data = stbi_load_from_memory(buf, size, &w, &h, &n, 4);
    if (data == NULL) {
        return NULL;
    }

    SDL_Texture *tex = SDL_CreateTexture(gwemu_get_renderer(),
                                         SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STATIC, w, h);
    if (tex) {
        SDL_UpdateTexture(tex, NULL, data, w * 4);
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
    }
    stbi_image_free(data);
    return tex;
}

void InitCustomRendering(void)
{
    g_logo_tex = LoadTextureFromMemory(logo_sdf_data, logo_sdf_size);
}

static float GetDisplayAspectRatio(int width, int height)
{
    switch (g_config.display.ui.aspect_ratio) {
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_NATIVE:
        return (float)width / (float)height;
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_16X9:
        return 16.0f / 9.0f;
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_4X3:
        return 4.0f / 3.0f;
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_AUTO:
    default:
        /* Fixed real-hardware panel: its native ratio is the answer. */
        return (float)width / (float)height;
    }
}

/*
 * Draw the guest framebuffer into the window, honoring the configured
 * fit mode (scale/stretch/center), aspect-ratio override and filtering.
 * `flip` is vestigial from the GL days (texture data is top-down now)
 * but kept in the signature to minimize call-site churn.
 */
void RenderFramebuffer(SDL_Texture *tex, int width, int height, bool flip)
{
    if (!tex) {
        return;
    }

    float tw_f = 0, th_f = 0;
    SDL_GetTextureSize(tex, &tw_f, &th_f);
    int tw = (int)tw_f, th = (int)th_f;
    if (tw <= 0 || th <= 0) {
        return;
    }

    SDL_SetTextureScaleMode(tex,
        g_config.display.filtering == CONFIG_DISPLAY_FILTERING_NEAREST ?
        SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);

    SDL_FRect dst;
    if (g_config.display.ui.fit == CONFIG_DISPLAY_UI_FIT_STRETCH) {
        dst = (SDL_FRect){ 0, 0, (float)width, (float)height };
    } else if (g_config.display.ui.fit == CONFIG_DISPLAY_UI_FIT_CENTER) {
        float t_ratio = GetDisplayAspectRatio(tw, th);
        float dw = t_ratio * th;
        dst = (SDL_FRect){ (width - dw) / 2.0f, (height - th) / 2.0f,
                           dw, (float)th };
    } else { /* scale to fit */
        float t_ratio = GetDisplayAspectRatio(tw, th);
        float w_ratio = (float)width / (float)height;
        float dw, dh;
        if (w_ratio >= t_ratio) {
            dh = height;
            dw = height * t_ratio;
        } else {
            dw = width;
            dh = width / t_ratio;
        }
        dst = (SDL_FRect){ (width - dw) / 2.0f, (height - dh) / 2.0f,
                           dw, dh };
    }

    /*
     * Integer-multiple snap: window sizes are snapped UP a few pixels
     * past N x native (fractional-scale Wayland can't hit N x native
     * exactly in whole points -- see gwemu_snap_window_points()), so
     * when the fit lands within a few pixels of an exact integer
     * multiple of the guest resolution, draw at EXACTLY that multiple,
     * centered, nearest-neighbor: pixel-sharp at the intended size with
     * an imperceptible letterbox. Free-form window sizes far from a
     * multiple keep the plain fit (and configured filtering) above.
     */
    if (g_config.display.ui.fit != CONFIG_DISPLAY_UI_FIT_STRETCH) {
        const float snap_thresh = 6.0f;
        int mult = (int)lroundf(dst.w / tw);
        if (mult >= 1 &&
            fabsf(dst.w - (float)(mult * tw)) <= snap_thresh &&
            fabsf(dst.h - (float)(mult * th)) <= snap_thresh &&
            mult * tw <= width && mult * th <= height) {
            dst.w = mult * tw;
            dst.h = mult * th;
            dst.x = floorf((width - dst.w) / 2.0f);
            dst.y = floorf((height - dst.h) / 2.0f);
            SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
        }
    }

    SDL_Renderer *r = gwemu_get_renderer();

    if (getenv("GNW_SCALE_DEBUG")) {
        static SDL_FRect last_dst;
        static int last_ow, last_oh;
        int ow = 0, oh = 0, cow = 0, coh = 0, wpt = 0, hpt = 0, wpx = 0,
            hpx = 0;
        float sx = 1, sy = 1;
        SDL_GetRenderOutputSize(r, &ow, &oh);
        SDL_GetCurrentRenderOutputSize(r, &cow, &coh);
        SDL_GetRenderScale(r, &sx, &sy);
        SDL_Window *win = gwemu_get_window();
        SDL_GetWindowSize(win, &wpt, &hpt);
        SDL_GetWindowSizeInPixels(win, &wpx, &hpx);
        if (memcmp(&dst, &last_dst, sizeof(dst)) != 0 || ow != last_ow ||
            oh != last_oh) {
            last_dst = dst;
            last_ow = ow;
            last_oh = oh;
            fprintf(stderr,
                    "SCALE_DEBUG blit driver=%s in=%dx%d win=%dx%dpt "
                    "%dx%dpx out=%dx%d curout=%dx%d rscale=%.3fx%.3f "
                    "dst=(%.1f,%.1f %.1fx%.1f)\n",
                    SDL_GetCurrentVideoDriver(), width, height, wpt, hpt,
                    wpx, hpx, ow, oh, cow, coh, sx, sy, dst.x, dst.y, dst.w,
                    dst.h);
        }
    }

    if (flip) {
        SDL_RenderTextureRotated(r, tex, NULL, &dst, 0, NULL,
                                 SDL_FLIP_VERTICAL);
    } else {
        SDL_RenderTexture(r, tex, NULL, &dst);
    }
}

void ScaleDimensions(int src_width, int src_height, int max_width,
                     int max_height, int *out_width, int *out_height)
{
    float w_ratio = (float)max_width / (float)max_height;
    float t_ratio = (float)src_width / (float)src_height;

    if (w_ratio >= t_ratio) {
        *out_width = (float)max_width * t_ratio / w_ratio;
        *out_height = max_height;
    } else {
        *out_width = max_width;
        *out_height = (float)max_height * w_ratio / t_ratio;
    }
}

/*
 * Encode the guest framebuffer as PNG. Sourced from the raw guest
 * pixels (gwemu_get_fb_pixels(), RGBA, top-down) rather than reading
 * anything back from the GPU -- no render-target/readback support
 * required of the SDL renderer backend, and the screenshot is the
 * pristine pre-HUD frame by construction. `tex`/`flip` are unused
 * legacy parameters kept to minimize call-site churn.
 */
bool RenderFramebufferToPng(SDL_Texture *tex, bool flip,
                            std::vector<uint8_t> &png, int max_width,
                            int max_height)
{
    (void)tex;
    (void)flip;

    uint8_t *pixels = NULL;
    int src_w = 0, src_h = 0;
    if (!gwemu_get_fb_pixels(&pixels, &src_w, &src_h) || !pixels) {
        return false;
    }

    int width = src_h * GetDisplayAspectRatio(src_w, src_h);
    int height = src_h;

    if (!max_width) {
        max_width = width;
    }
    if (!max_height) {
        max_height = height;
    }
    ScaleDimensions(width, height, max_width, max_height, &width, &height);

    std::vector<uint8_t> rgb;
    rgb.resize((size_t)width * height * 3);
    for (int y = 0; y < height; y++) {
        int sy = (int)((int64_t)y * src_h / height);
        const uint8_t *src_row = pixels + (size_t)sy * src_w * 4;
        uint8_t *dst_row = rgb.data() + (size_t)y * width * 3;
        for (int x = 0; x < width; x++) {
            int sx = (int)((int64_t)x * src_w / width);
            dst_row[x * 3 + 0] = src_row[sx * 4 + 0];
            dst_row[x * 3 + 1] = src_row[sx * 4 + 1];
            dst_row[x * 3 + 2] = src_row[sx * 4 + 2];
        }
    }
    free(pixels);

    return fpng::fpng_encode_image_to_memory(rgb.data(), width, height, 3,
                                             png);
}

void SaveScreenshot(SDL_Texture *tex, bool flip)
{
    Error *err = NULL;
    char fname[128];
    std::vector<uint8_t> png;

    if (RenderFramebufferToPng(tex, flip, png)) {
        time_t t = time(NULL);
        struct tm *tmp = localtime(&t);
        if (tmp) {
            strftime(fname, sizeof(fname), "gwemu-%Y-%m-%d-%H-%M-%S.png",
                     tmp);
        } else {
            strcpy(fname, "gwemu.png");
        }

        const char *output_dir = g_config.general.screenshot_dir;
        if (!strlen(output_dir)) {
            output_dir = ".";
        }
        // FIXME: Check for existing path
        char *path = g_strdup_printf("%s/%s", output_dir, fname);
        FILE *fd = g_fopen(path, "wb");
        if (fd) {
            int s = fwrite(png.data(), png.size(), 1, fd);
            if (s != 1) {
                error_setg(&err, "Failed to write %s", path);
            }
            fclose(fd);
        } else {
            error_setg(&err, "Failed to open %s for writing", path);
        }
        g_free(path);
    } else {
        error_setg(&err, "Failed to encode PNG image");
    }

    if (err) {
        gwemu_queue_error_message(error_get_pretty(err));
        error_report_err(err);
    } else {
        char *msg = g_strdup_printf("Screenshot Saved: %s", fname);
        gwemu_queue_notification(msg);
        free(msg);
    }
}
