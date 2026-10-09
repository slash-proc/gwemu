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
#include "data/mario_bezel.png.h"
#include "data/zelda_bezel.png.h"
#include "notifications.hh"
#include "stb_image.h"
#include <fpng.h>
#include <climits>
#include <cstdlib>
#include <math.h>
#include <stdio.h>
#include <vector>

SDL_Texture *g_logo_tex;
static SDL_Texture *g_mario_bezel_tex;
static SDL_Texture *g_zelda_bezel_tex;

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
    g_mario_bezel_tex = LoadTextureFromMemory(mario_bezel_data, mario_bezel_size);
    g_zelda_bezel_tex = LoadTextureFromMemory(zelda_bezel_data, zelda_bezel_size);
}

void gwemu_hud_set_window_aspect_ratio_for_size(int width, int height)
{
    SDL_Window *win = gwemu_get_window();
    if (!win || width <= 0 || height <= 0) {
        return;
    }
    /* Let the window manager constrain interactive drags. Resizing the
     * window from a resize event makes SDL and the WM fight over geometry. */
    float aspect = (float)width / height;
    SDL_SetWindowAspectRatio(win, aspect, aspect);
}

void gwemu_hud_get_display_window_size(int display_scale, int *width,
                                       int *height)
{
    SDL_Window *win = gwemu_get_window();
    int tw = 0, th = 0;
    if (!win || !width || !height || display_scale < 1 ||
        !gwemu_hud_get_framebuffer_size(&tw, &th)) {
        return;
    }
    *width = tw * display_scale;
    *height = th * display_scale;
    if (g_config.display.ui.bezel == CONFIG_DISPLAY_UI_BEZEL_MARIO) {
        float screen_scale = fmaxf(tw * display_scale / 406.0f,
                                   th * display_scale / 307.0f);
        *width = (int)ceilf(919 * screen_scale);
        *height = (int)ceilf(550 * screen_scale);
    } else if (g_config.display.ui.bezel == CONFIG_DISPLAY_UI_BEZEL_ZELDA) {
        float screen_scale = fmaxf(tw * display_scale / 415.0f,
                                   th * display_scale / 318.0f);
        *width = (int)ceilf(950 * screen_scale);
        *height = (int)ceilf(567 * screen_scale);
    }
    if (g_config.display.ui.show_menubar &&
        g_config.display.ui.menubar_behavior ==
            CONFIG_DISPLAY_UI_MENUBAR_BEHAVIOR_FIXED) {
        /* The menu height is measured in renderer pixels, while the window
         * target above is converted through SDL's display scale before
         * resizing. Scale the menu contribution into the same target units
         * or the bezel loses that height and is letterboxed horizontally. */
        float pixel_density = fmaxf(SDL_GetWindowPixelDensity(win), 1.0f);
        float target_units_per_renderer_pixel =
            gwemu_get_window_pixel_scale(win) / pixel_density;
        *height += (int)ceilf(
            gwemu_hud_get_menu_bar_height_pixels(win) *
            target_units_per_renderer_pixel);
    }
}

int gwemu_hud_get_current_display_scale(void)
{
    SDL_Window *win = gwemu_get_window();
    if (!win) {
        return 1;
    }

    int current_w, current_h;
    SDL_GetWindowSize(win, &current_w, &current_h);
    int best_scale = 1;
    int best_distance = INT_MAX;
    for (int scale = 1; scale <= 4; scale++) {
        int target_w, target_h, target_w_points, target_h_points;
        gwemu_hud_get_display_window_size(scale, &target_w, &target_h);
        if (target_w <= 0 || target_h <= 0) {
            break;
        }
        gwemu_snap_window_points(win, target_w, target_h, &target_w_points,
                                 &target_h_points);
        int distance = abs(current_w - target_w_points) +
                       abs(current_h - target_h_points);
        if (distance < best_distance) {
            best_distance = distance;
            best_scale = scale;
        }
    }
    return best_scale;
}

void gwemu_hud_resize_for_bezel(int display_scale)
{
    SDL_Window *win = gwemu_get_window();
    int width = 0, height = 0;
    if (!win || display_scale < 1 || display_scale > 4) {
        return;
    }
    gwemu_hud_get_display_window_size(display_scale, &width, &height);
    if (width <= 0 || height <= 0) {
        return;
    }
    gwemu_hud_set_window_aspect_ratio_for_size(width, height);

    if (g_config.display.ui.bezel == CONFIG_DISPLAY_UI_BEZEL_NONE) {
        /* With no artwork, return to the guest display's native 1x size
         * instead of retaining the larger bezel window geometry. */
        gwemu_set_window_size_pixels(win, width, height);
        return;
    }
    const SDL_DisplayMode *mode =
        SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win));
    if (mode && (width > mode->w || height > mode->h)) return;
    gwemu_set_window_size_pixels(win, width, height);
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
 * Fit the guest framebuffer to the window. With a bezel selected, fit the
 * artwork to the available area and place the framebuffer in its opening.
 * `flip` is vestigial from the GL days (texture data is top-down now)
 * but kept in the signature to minimize call-site churn.
 */
void RenderFramebuffer(SDL_Texture *tex, int width, int height, bool flip,
                       int top_offset)
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

    SDL_Texture *bezel = NULL;
    float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    if (g_config.display.ui.bezel == CONFIG_DISPLAY_UI_BEZEL_MARIO) {
        bezel = g_mario_bezel_tex;
        bx0 = 257.0f; by0 = 124.0f; bx1 = 663.0f; by1 = 431.0f;
    } else if (g_config.display.ui.bezel == CONFIG_DISPLAY_UI_BEZEL_ZELDA) {
        bezel = g_zelda_bezel_tex;
        bx0 = 269.0f; by0 = 128.0f; bx1 = 684.0f; by1 = 446.0f;
    }

    SDL_FRect dst;
    if (bezel) {
        float bwf = 0, bhf = 0;
        SDL_GetTextureSize(bezel, &bwf, &bhf);
        /* The window is constrained to the selected bezel ratio. Fill the
         * available device area directly so rounding and menu-height
         * differences cannot expose black strips at either side. */
        SDL_FRect outer = { 0.0f, (float)top_offset, (float)width,
                            (float)height };
        SDL_RenderTexture(gwemu_get_renderer(), bezel, NULL, &outer);
        float sx = outer.w / bwf, sy = outer.h / bhf;
        dst = (SDL_FRect){ outer.x + bx0 * sx, outer.y + by0 * sy,
                           (bx1 - bx0) * sx, (by1 - by0) * sy };
    } else {
        float t_ratio = GetDisplayAspectRatio(tw, th);
        if (g_config.display.ui.fit == CONFIG_DISPLAY_UI_FIT_STRETCH) {
            dst = (SDL_FRect){ 0, (float)top_offset, (float)width,
                               (float)height };
        } else if (g_config.display.ui.fit == CONFIG_DISPLAY_UI_FIT_CENTER) {
            float dw = t_ratio * th;
            dst = (SDL_FRect){ (width - dw) / 2.0f, (float)top_offset,
                               dw, (float)th };
        } else {
            float w_ratio = (float)width / (float)height;
            float dw, dh;
            if (w_ratio >= t_ratio) {
                dh = height;
                dw = height * t_ratio;
            } else {
                dw = width;
                dh = width / t_ratio;
            }
            dst = (SDL_FRect){ (width - dw) / 2.0f,
                               top_offset + (height - dh) / 2.0f, dw, dh };
        }
    }
    SDL_SetTextureScaleMode(tex,
        g_config.display.filtering == CONFIG_DISPLAY_FILTERING_NEAREST ?
        SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);

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
                    "SCALE_DEBUG blit driver=%s render_area=%dx%d guest="
                    "%dx%d win=%dx%dpt %dx%dpx out=%dx%d curout=%dx%d "
                    "rscale=%.3fx%.3f dst=(%.1f,%.1f %.1fx%.1f)\n",
                    SDL_GetCurrentVideoDriver(), width, height, tw, th, wpt,
                    hpt, wpx, hpx, ow, oh, cow, coh, sx, sy, dst.x, dst.y,
                    dst.w, dst.h);
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
