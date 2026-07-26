/*
 * GWemu SDL display driver
 *
 * Copyright (c) 2020-2025 Matt Borgerson
 *
 * Based on sdl2.c, sdl2-gl.c
 *
 * Copyright (c) 2003 Fabrice Bellard
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
/* Ported SDL 1.2 code to 2.0 by Dave Airlie. */

#include "qemu/osdep.h"
#ifdef __linux__
#include <libgen.h>
#endif
#include "qemu/module.h"
#include "qemu/thread.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "qemu-version.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-block.h"
#include "qobject/qdict.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/kbd-state.h"
#include "system/runstate.h"
#include "system/runstate-action.h"
#include "system/system.h"
#include "xui/gwemu-hud.h"
#include "gwemu-gnw-input.h"
#include "hw/misc/gnw_env.h"
#include "gwemu-input.h"
#include "gwemu-settings.h"
#include "gwemu-snapshots.h"
#include "gwemu-version.h"
#include "gwemu-os-utils.h"

#include "ui/gwemu-notifications.h"

#include <stb_image.h>
#include <locale.h>
#include <math.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#ifndef DEBUG_GWEMU_C
#define DEBUG_GWEMU_C 0
#endif

#if DEBUG_GWEMU_C
#define DPRINTF(...) fprintf(stderr, __VA_ARGS__)
#else
#define DPRINTF(...)
#endif

uint64_t vblank_interval_ns = 16666666LL;
bool use_vblank_timer_thread = true;

struct gwemu_console {
    DisplayChangeListener dcl;
    DisplaySurface *surface;
    DisplayOptions *opts;
    SDL_Window *real_window;
    int idx;
    int hidden;
    int ignore_hotkeys;
    SDL_Renderer *renderer;
    QKbdState *kbd;
};

#ifdef _WIN32
// Provide hint to prefer high-performance graphics for hybrid systems
// https://gpuopen.com/learn/amdpowerxpressrequesthighperformance/
__declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 1;
// https://docs.nvidia.com/gameworks/content/technologies/desktop/optimus.htm
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
#endif

static int num_outputs;
static struct gwemu_console *scon_list;
static SDL_Surface *guest_sprite_surface;
static int gui_grab; /* if true, all keyboard/mouse events are grabbed */
static bool alt_grab;
static bool ctrl_grab;
static int gui_saved_grab;
static int gui_fullscreen;
static int gui_grab_code = SDL_KMOD_LALT | SDL_KMOD_LCTRL;
static SDL_Cursor *sdl_cursor_normal;
static SDL_Cursor *sdl_cursor_hidden;
static int absolute_enabled;
static int guest_cursor;
static int guest_x, guest_y;
static SDL_Cursor *guest_sprite;
static Notifier mouse_mode_notifier;
static SDL_Window *m_window;
static SDL_Window *m_settings_window;
static SDL_Renderer *m_settings_renderer;
static SDL_Renderer *m_renderer;
/* Streaming texture holding the guest framebuffer, sized to the current
 * DisplaySurface. Replaces the GL surface-texture upload path. */
static SDL_Texture *m_fb_tex;
static int m_fb_w, m_fb_h;
static QemuSemaphore display_init_sem;
static QemuSemaphore display_shutdown_sem;
static QEMUTimer *vblank_timer;
static QemuThread vblank_thread;
static bool qemu_exiting;
static int exit_status;


#if DEBUG_GWEMU_C
static uint64_t lock_held_acc;
static uint64_t lock_start;
#endif

void gwemu_main_loop_lock(void)
{
    bql_lock();
#if DEBUG_GWEMU_C
    lock_start = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
#endif
}

void gwemu_main_loop_unlock(void)
{
#if DEBUG_GWEMU_C
    lock_held_acc += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - lock_start;
#endif
    bql_unlock();
}

SDL_Window *gwemu_get_window(void)
{
    return m_window;
}


static uint32_t get_window_id_from_event(SDL_Event *ev) {
    switch (ev->type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: return ev->key.windowID;
        case SDL_EVENT_TEXT_EDITING: return ev->edit.windowID;
        case SDL_EVENT_TEXT_INPUT: return ev->text.windowID;
        case SDL_EVENT_MOUSE_MOTION: return ev->motion.windowID;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: return ev->button.windowID;
        case SDL_EVENT_MOUSE_WHEEL: return ev->wheel.windowID;
        case SDL_EVENT_WINDOW_SHOWN:
        case SDL_EVENT_WINDOW_HIDDEN:
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_MOVED:
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MOUSE_ENTER:
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                case SDL_EVENT_WINDOW_HIT_TEST: return ev->window.windowID;
        case SDL_EVENT_DROP_FILE:
        case SDL_EVENT_DROP_TEXT:
        case SDL_EVENT_DROP_BEGIN:
        case SDL_EVENT_DROP_COMPLETE:
        case SDL_EVENT_DROP_POSITION: return ev->drop.windowID;
        default: return 0;
    }
}

static struct gwemu_console *get_scon_from_window(uint32_t window_id)
{
    int i;
    for (i = 0; i < num_outputs; i++) {
        if (scon_list[i].real_window == SDL_GetWindowFromID(window_id)) {
            return &scon_list[i];
        }
    }
    return NULL;
}

/*
 * Convert a desired PHYSICAL pixel size into an SDL window size in
 * POINTS, snapped so that points * fractional-scale is (near-)integral.
 *
 * Why snapping matters (confirmed via WAYLAND_DEBUG protocol traces on
 * GNOME at 1.75x fractional scale): SDL3 does use wp_fractional_scale_v1
 * + wp_viewporter, committing a buffer of round(points * scale) pixels
 * with wp_viewport.set_destination(points). If points * scale is not an
 * integer (e.g. 366pt * 1.75 = 640.5), the buffer (641px) cannot map
 * 1:1 onto the destination (640.5 device px) and the compositor
 * resamples the whole surface -- visibly blurry. Snapping to the
 * nearest point size whose product with the scale is integral (with
 * 1.75 = 7/4, any multiple of 4pt) restores exact 1:1 presentation.
 * On density-1 hosts (x11/Windows) scale is 1.0 and this reduces to a
 * pass-through of px_w/px_h.
 */
void gwemu_snap_window_points(SDL_Window *win, int px_w, int px_h,
                              int *pt_w, int *pt_h)
{
    float scale = fmaxf(SDL_GetWindowDisplayScale(win), 1.0f);
    float density = fmaxf(SDL_GetWindowPixelDensity(win), 1.0f);
    /* The buffer size follows the pixel density (== fractional scale on
     * Wayland); prefer the display scale when the two agree, since it
     * comes straight from preferred_scale/120 and is exact. */
    float d = (fabsf(scale - density) < 0.05f) ? scale : density;
    /*
     * Snap UP, not to nearest: the smallest integral-product point size
     * whose pixel size is >= the requested one. The requested size is
     * typically an exact integer multiple of the guest resolution (e.g.
     * 640x480 = 2x native), which is usually unreachable exactly at a
     * fractional scale (needs 365.71pt at 7/4) -- a slightly LARGER
     * window lets the blit path draw the guest at the exact integer
     * multiple, pixel-sharp, with a few pixels of letterbox, instead of
     * resampling the guest at a non-integer factor.
     */
    int dims_px[2] = { px_w, px_h };
    int *dims_pt[2] = { pt_w, pt_h };
    for (int i = 0; i < 2; i++) {
        int base = (int)ceilf(dims_px[i] / d - 0.001f);
        if (base < 1) {
            base = 1;
        }
        int best = base;
        for (int c = base; c <= base + 7; c++) {
            float prod = c * d;
            if (fabsf(prod - lroundf(prod)) > 0.01f &&
                d > 1.0f) {
                continue; /* not integral: compositor would resample */
            }
            if (lroundf(prod) < dims_px[i]) {
                continue;
            }
            best = c;
            break;
        }
        *dims_pt[i] = best;
    }
}

static void window_resize(struct gwemu_console *scon)
{
    if (!scon->real_window) {
        return;
    }

    /*
     * surface dims are PIXELS; SDL_SetWindowSize takes POINTS. Equal on
     * x11/Windows, but on Wayland with display scaling points*density =
     * pixels, so dividing keeps the physical size right (previously the
     * window ballooned by the scale factor).
     */
    int rw_pt, rh_pt;
    gwemu_snap_window_points(scon->real_window,
                             surface_width(scon->surface),
                             surface_height(scon->surface), &rw_pt, &rh_pt);
    SDL_SetWindowSize(scon->real_window, rw_pt, rh_pt);
}

static void hide_cursor(struct gwemu_console *scon)
{
    if (scon->opts->has_show_cursor && scon->opts->show_cursor) {
        return;
    }

    SDL_HideCursor();
    SDL_SetCursor(sdl_cursor_hidden);

    if (!qemu_input_is_absolute(scon->dcl.con)) {
        SDL_SetWindowRelativeMouseMode(scon->real_window, true);
    }
}

static void show_cursor(struct gwemu_console *scon)
{
    if (scon->opts->has_show_cursor && scon->opts->show_cursor) {
        return;
    }

    if (!qemu_input_is_absolute(scon->dcl.con)) {
        SDL_SetWindowRelativeMouseMode(scon->real_window, false);
    }

    if (guest_cursor &&
        (gui_grab || qemu_input_is_absolute(scon->dcl.con) || absolute_enabled)) {
        SDL_SetCursor(guest_sprite);
    } else {
        SDL_SetCursor(sdl_cursor_normal);
    }

    SDL_ShowCursor();
}

static void grab_start(struct gwemu_console *scon)
{
}

static void grab_end(struct gwemu_console *scon)
{
    SDL_SetWindowKeyboardGrab(scon->real_window, false);
    SDL_SetWindowMouseGrab(scon->real_window, false);
    gui_grab = 0;
    show_cursor(scon);
}

static void absolute_mouse_grab(struct gwemu_console *scon)
{
    float mouse_x, mouse_y;
    int scr_w, scr_h;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    SDL_GetWindowSize(scon->real_window, &scr_w, &scr_h);
    if (mouse_x > 0 && mouse_x < scr_w - 1 &&
        mouse_y > 0 && mouse_y < scr_h - 1) {
        grab_start(scon);
    }
}

static void mouse_mode_change(Notifier *notify, void *data)
{
    if (qemu_input_is_absolute(scon_list[0].dcl.con)) {
        if (!absolute_enabled) {
            absolute_enabled = 1;
            SDL_SetWindowRelativeMouseMode(scon_list[0].real_window, false);
            absolute_mouse_grab(&scon_list[0]);
        }
    } else if (absolute_enabled) {
        if (!gui_fullscreen) {
            grab_end(&scon_list[0]);
        }
        absolute_enabled = 0;
    }
}

static void send_mouse_event(struct gwemu_console *scon, int dx, int dy,
                                 int x, int y, int state)
{
    static uint32_t bmap[INPUT_BUTTON__MAX] = {
        [INPUT_BUTTON_LEFT]       = SDL_BUTTON_MASK(SDL_BUTTON_LEFT),
        [INPUT_BUTTON_MIDDLE]     = SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE),
        [INPUT_BUTTON_RIGHT]      = SDL_BUTTON_MASK(SDL_BUTTON_RIGHT),
    };
    static uint32_t prev_state;

    if (prev_state != state) {
        qemu_input_update_buttons(scon->dcl.con, bmap, prev_state, state);
        prev_state = state;
    }

    if (qemu_input_is_absolute(scon->dcl.con)) {
        qemu_input_queue_abs(scon->dcl.con, INPUT_AXIS_X,
                             x, 0, surface_width(scon->surface));
        qemu_input_queue_abs(scon->dcl.con, INPUT_AXIS_Y,
                             y, 0, surface_height(scon->surface));
    } else {
        if (guest_cursor) {
            x -= guest_x;
            y -= guest_y;
            guest_x += x;
            guest_y += y;
            dx = x;
            dy = y;
        }
        qemu_input_queue_rel(scon->dcl.con, INPUT_AXIS_X, dx);
        qemu_input_queue_rel(scon->dcl.con, INPUT_AXIS_Y, dy);
    }
    qemu_input_event_sync();
}

static void set_full_screen(struct gwemu_console *scon, bool set)
{
    gui_fullscreen = set;

    if (gui_fullscreen) {
        const SDL_DisplayMode *mode = NULL;
        SDL_DisplayMode **modes = NULL;
        if (g_config.display.window.fullscreen_exclusive) {
            SDL_DisplayID display = SDL_GetDisplayForWindow(scon->real_window);
            if (display) {
                int num_modes = 0;
                modes = SDL_GetFullscreenDisplayModes(display, &num_modes);
                if (modes && num_modes > 0) {
                    // First mode is the highest resolution, typically the native resolution
                    mode = modes[0];
                }
            }
            if (mode) {
                fprintf(stderr, "Selected exclusive fullscreen mode: %dx%d pixel_density=%f refresh_rate=%f\n", mode->w, mode->h, mode->pixel_density, mode->refresh_rate);
            } else {
                fprintf(stderr, "Failed to get fullscreen display mode: %s\n", SDL_GetError());
            }
        }
        SDL_SetWindowFullscreenMode(scon->real_window, mode);
        SDL_free(modes);
        SDL_SetWindowFullscreen(scon->real_window, true);
        gui_saved_grab = gui_grab;
        grab_start(scon);
    } else {
        if (!gui_saved_grab) {
            grab_end(scon);
        }
        SDL_SetWindowFullscreen(scon->real_window, false);
    }
}

static void toggle_full_screen(struct gwemu_console *scon)
{
    set_full_screen(scon, !gui_fullscreen);
}

void gwemu_toggle_fullscreen(void)
{
    toggle_full_screen(&scon_list[0]);
}

int gwemu_is_fullscreen(void)
{
    return gui_fullscreen;
}

static int get_mod_state(void)
{
    SDL_Keymod mod = SDL_GetModState();

    if (alt_grab) {
        return (mod & (gui_grab_code | SDL_KMOD_LSHIFT)) ==
            (gui_grab_code | SDL_KMOD_LSHIFT);
    } else if (ctrl_grab) {
        return (mod & SDL_KMOD_RCTRL) == SDL_KMOD_RCTRL;
    } else {
        return (mod & gui_grab_code) == gui_grab_code;
    }
}

static void process_key(struct gwemu_console *scon, SDL_KeyboardEvent *ev)
{
    int qcode;

    if (ev->scancode >= qemu_input_map_usb_to_qcode_len) {
        return;
    }
    qcode = qemu_input_map_usb_to_qcode[ev->scancode];
    qkbd_state_key_event(scon->kbd, qcode, ev->type == SDL_EVENT_KEY_DOWN);
}

static void handle_keydown(SDL_Event *ev)
{
    int win;
    struct gwemu_console *scon = get_scon_from_window(ev->key.windowID);
    if (scon == NULL) return;
    int gui_key_modifier_pressed = get_mod_state();
    int gui_keysym = 0;

    if (!scon->ignore_hotkeys && gui_key_modifier_pressed && !ev->key.repeat) {
        switch (ev->key.scancode) {
        case SDL_SCANCODE_2:
        case SDL_SCANCODE_3:
        case SDL_SCANCODE_4:
        case SDL_SCANCODE_5:
        case SDL_SCANCODE_6:
        case SDL_SCANCODE_7:
        case SDL_SCANCODE_8:
        case SDL_SCANCODE_9:
            if (gui_grab) {
                grab_end(scon);
            }

            win = ev->key.scancode - SDL_SCANCODE_1;
            if (win < num_outputs) {
                scon_list[win].hidden = !scon_list[win].hidden;
                if (scon_list[win].real_window) {
                    if (scon_list[win].hidden) {
                        SDL_HideWindow(scon_list[win].real_window);
                    } else {
                        SDL_ShowWindow(scon_list[win].real_window);
                    }
                }
                gui_keysym = 1;
            }
            break;
        case SDL_SCANCODE_F:
            toggle_full_screen(scon);
            gui_keysym = 1;
            break;
        case SDL_SCANCODE_G:
            gui_keysym = 1;
            if (!gui_grab) {
                grab_start(scon);
            } else if (!gui_fullscreen) {
                grab_end(scon);
            }
            break;
        case SDL_SCANCODE_U:
            window_resize(scon);
            gui_keysym = 1;
            break;
        default:
            break;
        }
    }
    if (!gui_keysym) {
        process_key(scon, &ev->key);
    }
}

static void handle_keyup(SDL_Event *ev)
{
    struct gwemu_console *scon = get_scon_from_window(ev->key.windowID);
    if (!scon) return;

    scon->ignore_hotkeys = false;
    process_key(scon, &ev->key);
}

static void handle_mousemotion(SDL_Event *ev)
{
    int max_x, max_y;
    struct gwemu_console *scon = get_scon_from_window(ev->motion.windowID);

    if (!scon || !qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    if (qemu_input_is_absolute(scon->dcl.con) || absolute_enabled) {
        int scr_w, scr_h;
        SDL_GetWindowSize(scon->real_window, &scr_w, &scr_h);
        max_x = scr_w - 1;
        max_y = scr_h - 1;
        if (gui_grab && !gui_fullscreen
            && (ev->motion.x == 0 || ev->motion.y == 0 ||
                ev->motion.x == max_x || ev->motion.y == max_y)) {
            grab_end(scon);
        }
        if (!gui_grab &&
            (ev->motion.x > 0 && ev->motion.x < max_x &&
             ev->motion.y > 0 && ev->motion.y < max_y)) {
            grab_start(scon);
        }
    }
    if (gui_grab || qemu_input_is_absolute(scon->dcl.con) || absolute_enabled) {
        send_mouse_event(scon, ev->motion.xrel, ev->motion.yrel,
                             ev->motion.x, ev->motion.y, ev->motion.state);
    }
}

static void handle_mousebutton(SDL_Event *ev)
{
    int buttonstate = SDL_GetMouseState(NULL, NULL);
    SDL_MouseButtonEvent *bev;
    struct gwemu_console *scon = get_scon_from_window(ev->button.windowID);

    if (!scon || !qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    bev = &ev->button;
    if (!gui_grab && !qemu_input_is_absolute(scon->dcl.con)) {
        if (ev->type == SDL_EVENT_MOUSE_BUTTON_UP && bev->button == SDL_BUTTON_LEFT) {
            /* start grabbing all events */
            grab_start(scon);
        }
    } else {
        if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            buttonstate |= SDL_BUTTON_MASK(bev->button);
        } else {
            buttonstate &= ~SDL_BUTTON_MASK(bev->button);
        }
        send_mouse_event(scon, 0, 0, bev->x, bev->y, buttonstate);
    }
}

static void handle_mousewheel(SDL_Event *ev)
{
    struct gwemu_console *scon = get_scon_from_window(ev->wheel.windowID);
    SDL_MouseWheelEvent *wev = &ev->wheel;
    InputButton btn;

    if (!scon || !qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    if (wev->y > 0) {
        btn = INPUT_BUTTON_WHEEL_UP;
    } else if (wev->y < 0) {
        btn = INPUT_BUTTON_WHEEL_DOWN;
    } else {
        return;
    }

    qemu_input_queue_btn(scon->dcl.con, btn, true);
    qemu_input_event_sync();
    qemu_input_queue_btn(scon->dcl.con, btn, false);
    qemu_input_event_sync();
}

static void handle_windowevent(SDL_Event *ev)
{
    struct gwemu_console *scon = get_scon_from_window(ev->window.windowID);
    bool allow_close = true;

    if (!scon) {
        return;
    }

    switch (ev->type) {
    case SDL_EVENT_WINDOW_RESIZED:
        {
            QemuUIInfo info;
            memset(&info, 0, sizeof(info));
            info.width = ev->window.data1;
            info.height = ev->window.data2;
            dpy_set_ui_info(scon->dcl.con, &info, true);

            if (!gui_fullscreen) {
                /*
                 * Store PHYSICAL pixels, not points: everywhere that
                 * consumes window sizes (startup creation below, the xN
                 * presets, window_resize()) treats configured sizes as
                 * pixels and divides by the pixel density when calling
                 * point-based SDL window APIs. SDL_EVENT_WINDOW_RESIZED
                 * reports points, so convert here; on x11/Windows
                 * density == 1 and this is a no-op.
                 */
                float wr_d = fmaxf(
                    SDL_GetWindowPixelDensity(scon->real_window), 1.0f);
                g_config.display.window.last_width =
                    (int)lroundf(ev->window.data1 * wr_d);
                g_config.display.window.last_height =
                    (int)lroundf(ev->window.data2 * wr_d);
            }
        }
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
        if (!gui_grab && (qemu_input_is_absolute(scon->dcl.con) || absolute_enabled)) {
            absolute_mouse_grab(scon);
        }
        /* If a new console window opened using a hotkey receives the
         * focus, SDL sends another KEYDOWN event to the new window,
         * closing the console window immediately after.
         *
         * Work around this by ignoring further hotkey events until a
         * key is released.
         */
        scon->ignore_hotkeys = get_mod_state();
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (gui_grab && !gui_fullscreen) {
            grab_end(scon);
        }
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (qemu_console_is_graphic(scon->dcl.con)) {
            if (scon->opts->has_window_close && !scon->opts->window_close) {
                allow_close = false;
            }
            if (allow_close) {
                shutdown_action = SHUTDOWN_ACTION_POWEROFF;
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
            }
        } else {
            SDL_HideWindow(scon->real_window);
            scon->hidden = true;
        }
        break;
    case SDL_EVENT_WINDOW_SHOWN:
        scon->hidden = false;
        break;
    case SDL_EVENT_WINDOW_HIDDEN:
        scon->hidden = true;
        break;
    }
}

static void mouse_warp(DisplayChangeListener *dcl,
                       int x, int y, bool on)
{
    struct gwemu_console *scon = container_of(dcl, struct gwemu_console, dcl);

    if (!qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    if (on) {
        if (!guest_cursor) {
            show_cursor(scon);
        }
        if (gui_grab || qemu_input_is_absolute(scon->dcl.con) || absolute_enabled) {
            SDL_SetCursor(guest_sprite);
            if (!qemu_input_is_absolute(scon->dcl.con) && !absolute_enabled) {
                SDL_WarpMouseInWindow(scon->real_window, x, y);
            }
        }
    } else if (gui_grab) {
        hide_cursor(scon);
    }
    guest_cursor = on;
    guest_x = x, guest_y = y;
}

static void mouse_define(DisplayChangeListener *dcl,
                             QEMUCursor *c)
{

    if (guest_sprite) {
        SDL_DestroyCursor(guest_sprite);
    }

    if (guest_sprite_surface) {
        SDL_DestroySurface(guest_sprite_surface);
    }

    guest_sprite_surface =
        SDL_CreateSurfaceFrom(c->width, c->height, SDL_PIXELFORMAT_ARGB8888, c->data, c->width * 4);

    if (!guest_sprite_surface) {
        fprintf(stderr, "Failed to make rgb surface from %p\n", c);
        return;
    }
    guest_sprite = SDL_CreateColorCursor(guest_sprite_surface,
                                         c->hot_x, c->hot_y);
    if (!guest_sprite) {
        fprintf(stderr, "Failed to make color cursor from %p\n", c);
        return;
    }
    if (guest_cursor &&
        (gui_grab || qemu_input_is_absolute(dcl->con) || absolute_enabled)) {
        SDL_SetCursor(guest_sprite);
    }
}

/*
 * xemu's own QEMU fork added glformat/gltype fields directly to
 * DisplaySurface; our (unmodified upstream) surface.h doesn't have them --
 * compute the same values as local variables instead (matches how
 * ui/console-gl.c's own surface_gl_create_texture() already does this for
 * every other display backend in this tree).
 */
/*
 * Set when the guest surface has ACTUALLY changed: driven by the
 * dpy_gfx_update DisplayChangeListener op (gl_update below), which the
 * LTDC model only issues from gnw_h7b0_ltdc_update_display() when the
 * compositor has published a genuinely new frame. Consumed by
 * gl_render_frame, which otherwise reuses the texture it already has,
 * and by the main loop's content gate, which otherwise doesn't render
 * at all.
 *
 * It used to be set unconditionally after every graphic_hw_update() in
 * process_vblank() -- i.e. once per 60Hz vblank pump regardless of
 * whether the guest had drawn anything. That is why a 30fps guest still
 * produced 60 texture uploads per second (field-measured on a Pi 400):
 * the flag was a "we asked" signal, not a "there is new content" signal.
 * Registering the real dpy_gfx_update op makes it the latter.
 *
 * This matters more than it looks: the staging half of the upload holds
 * the BQL, which the vCPU and every device timer contend for -- audible
 * as periodic audio rips that vanish the moment the window is hidden and
 * rendering stops.
 */
static int m_fb_dirty = 1;
/* Count of dpy_gfx_update calls, i.e. frames the device published --
 * reported once per second by GNW_UI_FRAME_TRACE. Distinguishing this
 * from the render rate is what separates "the device is publishing each
 * frame twice" from "the UI is spinning"; they look identical from the
 * outside. */
static int m_fb_dirty_sets;
static void *m_fb_stage;
static size_t m_fb_stage_size;
static size_t m_fb_stage_stride;

/*
 * Stages the guest surface for upload under the caller's BQL.
 *
 * There used to be a memcmp against the previously staged bytes here, to
 * drop byte-identical republishes: the LTDC model published every guest
 * frame twice (the SRCR.IMR capture and the SRCR.VBR capture of the same
 * frame, whose shadow registers are already identical), so a 30fps guest
 * drove ~45 dpy_gfx_update calls and ~45 HUD rebuilds per second. That
 * was a workaround at the UI boundary for a device-model bug, and the
 * bug is fixed device-side now (gnw_h7b0_ltdc.c gates the IMR capture
 * when a non-structural VBR reload already covered it) -- publishes now
 * match the guest frame rate exactly. Don't reintroduce the compare: it
 * would be a full-surface scan on ~95% of frames to catch ~5%, and it
 * costs BQL time the vCPU and every device timer contend for.
 */
static SDL_Texture *gwemu_update_fb_texture(DisplaySurface *surface)
{
    SDL_PixelFormat fmt;
    switch (surface_format(surface)) {
    case PIXMAN_BE_b8g8r8x8:
    case PIXMAN_BE_b8g8r8a8:
        fmt = SDL_PIXELFORMAT_BGRA32;
        break;
    case PIXMAN_BE_x8r8g8b8:
    case PIXMAN_BE_a8r8g8b8:
        fmt = SDL_PIXELFORMAT_RGBA32;
        break;
    case PIXMAN_r5g6b5:
        fmt = SDL_PIXELFORMAT_RGB565;
        break;
    default:
        g_assert_not_reached();
    }

    int w = surface_width(surface), h = surface_height(surface);
    if (m_fb_tex && (m_fb_w != w || m_fb_h != h)) {
        SDL_DestroyTexture(m_fb_tex);
        m_fb_tex = NULL;
    }
    if (!m_fb_tex) {
        m_fb_tex = SDL_CreateTexture(m_renderer, fmt,
                                     SDL_TEXTUREACCESS_STREAMING, w, h);
        m_fb_w = w;
        m_fb_h = h;
        /* Every branch of this function fails silently, and a silent
         * failure here IS the "GUI works but game screen stays black"
         * symptom -- log the outcome of each (re)creation, it happens
         * once per surface size change. */
        fprintf(stderr, "fb_texture: create %dx%d sdl_fmt=%s -> %s%s%s\n",
                w, h, SDL_GetPixelFormatName(fmt),
                m_fb_tex ? "ok" : "FAILED",
                m_fb_tex ? "" : ": ", m_fb_tex ? "" : SDL_GetError());
    }
    /*
     * Stage the pixels under the caller's BQL, then hand the slow part
     * (SDL_UpdateTexture -> GPU) back to the caller to do UNLOCKED.
     * The lock exists only to stop the vblank thread's
     * graphic_hw_update() rewriting the surface mid-read; it does not
     * need to cover the transfer. Measured: ~0.7ms of BQL per upload
     * before, and 60 uploads/s is 40ms/s the vCPU spends blocked.
     * A memcpy of the same bytes is roughly a tenth of that.
     */
    size_t stride = surface_stride(surface);
    size_t need = stride * (size_t)h;
    if (m_fb_stage_size < need) {
        m_fb_stage = g_realloc(m_fb_stage, need);
        m_fb_stage_size = need;
    }
    memcpy(m_fb_stage, surface_data(surface), need);
    m_fb_stage_stride = stride;
    return m_fb_tex;
}

/* Unlocked half of the upload -- see gwemu_update_fb_texture(). */
static void gwemu_upload_fb_texture(void)
{
    if (!m_fb_tex || !m_fb_stage) {
        return;
    }
    if (!SDL_UpdateTexture(m_fb_tex, NULL, m_fb_stage, m_fb_stage_stride)) {
        static bool warned;
        if (!warned) {
            warned = true;
            fprintf(stderr, "fb_texture: SDL_UpdateTexture FAILED: %s\n",
                    SDL_GetError());
        }
    }
}

static bool xb_console_gl_check_format(DisplayChangeListener *dcl,
                                       pixman_format_code_t format)
{
    switch (format) {
    case PIXMAN_BE_b8g8r8x8:
    case PIXMAN_BE_b8g8r8a8:
    case PIXMAN_r5g6b5:
        return true;
    default:
        return false;
    }
}

/* New DisplaySurface => the cached texture is stale/wrong-sized. */
static void gl_switch(DisplayChangeListener *dcl,
                      DisplaySurface *new_surface)
{
    struct gwemu_console *scon = container_of(dcl, struct gwemu_console, dcl);
    scon->surface = new_surface;
    /* New surface => the cached texture is stale/wrong-sized, and the
     * content gate must let the next frame through. */
    qatomic_set(&m_fb_dirty, 1);
}

/*
 * The real "guest drew something" signal. Called (under the BQL) from
 * dpy_gfx_update(), i.e. from the LTDC model's update_display once the
 * compositor thread has published a new frame -- NOT once per vblank
 * pump. See the m_fb_dirty comment above.
 */
static void gl_update(DisplayChangeListener *dcl, int x, int y, int w, int h)
{
    qatomic_set(&m_fb_dirty, 1);
    qatomic_inc(&m_fb_dirty_sets);
}

static float update_avg(float avg, float ms, float r) {
    if (fabs(avg-ms) > 0.25*avg) avg = ms;
    else avg = avg*(1.0-r)+ms*r;
    return avg;
}

static float fps = 1.0;

static void update_fps(void)
{
    static float avg = 1.0;
    static int64_t last_update = 0;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (!last_update) {
        last_update = now;
        return;
    }
    float ms = ((float)(now-last_update)/1000000.0);
    last_update = now;
    avg = update_avg(avg, ms, 0.5);
    fps = 1000.0/avg;
}

static void process_vblank(struct gwemu_console *scon)
{
    assert(bql_locked());

    update_fps();

#if 0
    static uint64_t last_ns = 0;
    uint64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    uint64_t delta_ns = last_ns ? now_ns - last_ns : 0;
    fprintf(stderr, "%s delta_ns=%"PRId64"\n", __func__, delta_ns);
    last_ns = now_ns;
#endif

    /* Pumps the display device; m_fb_dirty is set from gl_update() only
     * if this actually produced a new frame. */
    graphic_hw_update(scon->dcl.con);
}

static void vblank_timer_callback(void *opaque)
{
    struct gwemu_console *scon = (struct gwemu_console *)opaque;

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    process_vblank(scon);
    timer_mod_ns(vblank_timer, now + vblank_interval_ns);
}

static void *vblank_timer_thread(void *opaque)
{
    struct gwemu_console *scon = (struct gwemu_console *)opaque;
    int64_t next_vblank = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    while (!qatomic_read(&qemu_exiting)) {
        // Schedule next vblank at fixed interval (absolute deadline)
        next_vblank += vblank_interval_ns;

        // Wait until deadline
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        if (now < next_vblank) {
            /* SDL_DelayPrecise() busy-spins the tail of the wait to hit
             * sub-microsecond accuracy. This thread only pumps
             * graphic_hw_update() -- nothing downstream cares about
             * jitter of a few hundred microseconds, and the absolute
             * deadline below corrects any drift -- so the spin was pure
             * CPU burn (measured ~9-10% of a core on a Pi 400).
             * SDL_DelayNS() is a plain nanosleep. */
            SDL_DelayNS(next_vblank - now);
        } else if (now > next_vblank + vblank_interval_ns) {
            // We've fallen behind by more than one frame, reset to avoid
            // rapid-fire catch-up
            next_vblank = now;
        }

        if (!qatomic_read(&qemu_exiting)) {
            static int vtrace = -1;
            static int64_t vt0;
            static uint64_t vloops, vlockwait, vrun;
            if (vtrace < 0) {
                const char *e = getenv("GNW_UI_FRAME_TRACE");
                vtrace = (e && *e && strcmp(e, "0") != 0);
            }
            int64_t l0 = vtrace ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;
            gwemu_main_loop_lock();
            int64_t l1 = vtrace ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;
            process_vblank(scon);
            gwemu_main_loop_unlock();
            if (vtrace) {
                int64_t l2 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
                vloops++;
                vlockwait += l1 - l0;
                vrun += l2 - l1;
                if (l2 - vt0 >= 1000000000LL) {
                    fprintf(stderr, "VBLANK loops=%llu lockwait=%.1fms "
                            "run=%.1fms\n", (unsigned long long)vloops,
                            vlockwait / 1e6, vrun / 1e6);
                    vt0 = l2;
                    vloops = vlockwait = vrun = 0;
                }
            }
        }
    }

    return NULL;
}

#if DEBUG_GWEMU_C
static void report_stats(void)
{
    uint64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    static uint64_t last_reported = 0;
    static int num_frames = 0;
    uint64_t delta_ms = now - last_reported;
    num_frames += 1;
    if (delta_ms >= 1000) {
        DPRINTF("[[ ");
        DPRINTF("vblank @%fHz avg", fps);
        DPRINTF(" - bql %"PRId64"ns/iter, %g%% time avg", lock_held_acc/num_frames, (double)lock_held_acc/(double)(delta_ms * 10000.0));
        DPRINTF(" ]]\n");
        lock_held_acc = 0;
        last_reported = now;
        num_frames = 0;
    }
}
#endif

/**
 * Renders the main interface. Usually called from the main thread,
 * but may sometimes be called from another thread.
 */
/* Main-window occlusion, event-driven (SDL_EVENT_WINDOW_OCCLUDED/
 * EXPOSED in poll_events) OR'd with the live window flag -- belt and
 * braces, the flag alone is compositor-dependent. */
static bool m_main_window_occluded;

/* GNW_UI_FRAME_TRACE=1 per-second accumulators (see gl_render_frame). */
static struct {
    uint64_t main_present_ns, main_present_max_ns;
    uint64_t settings_present_ns, settings_present_max_ns;
    uint32_t main_rendered, main_skipped;
    uint32_t iters;
    uint32_t idle_skipped;
    uint32_t throttle_retries;
    uint64_t fb_wait_ns, fb_held_ns, hud_held_ns;
    uint32_t fb_uploads;
    uint64_t last_report;
    /* Content-gate attribution: which condition admitted each frame, and
     * (via m_fb_dirty_sets) how many device publishes arrived. Kept (not
     * scratch instrumentation) because "renders > guest frames" is
     * otherwise indistinguishable from "the UI is spinning", and telling
     * those apart took a full diagnosis session. */
    uint32_t gate_content, gate_probe, gate_active, gate_heartbeat;
} ui_trace;

/* Adaptive present-stall throttle (self-clocking; needed because some
 * compositors -- GNOME/Mutter over XWayland with the vulkan renderer,
 * confirmed by field trace -- withhold swapchain images from an occluded
 * window WITHOUT ever setting SDL_WINDOW_OCCLUDED or sending the OCCLUDED
 * event, so the event/flag path above never engages and each main
 * SDL_RenderPresent blocks ~1s, starving the settings window). We measure
 * every main present; two consecutive presents slower than the threshold
 * (while settings is visible) enter throttled mode, where the main
 * window's render/present is skipped except for one probe present every
 * retry-cadence interval. A fast probe exits throttled mode immediately.
 *
 * Resuming is primarily EVENT-driven (EXPOSED/FOCUS_GAINED/MOUSE_ENTER/
 * RESTORED/SHOWN on the main window, tracked in poll_events): a probe
 * present against a still-occluded window does NOT return fast -- the
 * vulkan swapchain withholds the image and the present blocks ~500ms
 * until a compositor timeout (field-traced), so probing often turns into
 * a stall of its own. The timer probe is kept only as a slow fallback
 * (5s cadence) so a missed/never-delivered event can't strand the main
 * window in throttled mode forever; one 500ms stall per 5s is tolerable. */
#define GNW_PRESENT_STALL_THRESHOLD_NS  50000000ull  /*  50 ms */
#define GNW_PRESENT_RETRY_CADENCE_NS  5000000000ull  /*   5 s fallback */
static struct {
    bool throttled;
    uint32_t slow_streak;      /* consecutive slow presents */
    uint64_t last_retry_ns;    /* when the last throttled-mode probe ran */
} ui_throttle;
/* Set from poll_events when a visibility-suggesting event hits the main
 * window; consumed (cleared) by gl_render_frame to exit throttled mode. */
static bool m_main_window_resume_evt;

/*
 * Content gate (the reason the render loop no longer free-runs).
 *
 * The main loop is `while (!exiting) { poll_events(); gl_render_frame(); }`
 * with nothing pacing it but the blocking vsync inside the main window's
 * SDL_RenderPresent. That means the GUI rendered and presented at the
 * HOST refresh rate -- field-measured 117-121/s on a Pi 400 and 119.7/s
 * here -- for a guest producing 30 (Celeste) to 60 frames per second.
 * Up to 4x redundant presents, each one a full HUD rebuild plus a
 * present that costs real CPU on a GLES/vulkan driver that busy-waits
 * for the swap.
 *
 * So: render only when there is something new to show. "Something new"
 * deliberately is NOT "a fixed 30Hz cap" -- that would make the menus
 * feel awful. It is the union of:
 *
 *   - a new guest frame (m_fb_dirty, now a real dpy_gfx_update signal),
 *   - the settings window being visible (it is the interactive surface
 *     and always renders at full rate),
 *   - recent input/window activity (any SDL event refreshes a linger
 *     window, so drags, scrolls, hovers and scene transitions stay
 *     smooth for LINGER after the last event),
 *   - ImGui reporting that it wants the keyboard or mouse (a menu is
 *     open/hovered), and
 *   - a 10Hz idle heartbeat, so time-driven HUD animations (the
 *     menubar's 5s auto-hide fade in particular) still advance even
 *     with a completely idle guest and no input at all.
 *
 * Note what the last three are and are NOT: they raise the FLOOR, they
 * do not remove the ceiling. "UI is busy" renders at up to 60Hz, not at
 * whatever the host panel runs at. Letting activity mean "render freely"
 * would have quietly reinstated the whole bug the moment a user held a
 * key down, because SDL key-repeat would keep the activity window fresh
 * for the entire time they were playing. (The timeline harness injects
 * input on the guest side, so a scripted benchmark would never have
 * caught that -- only a human holding a direction key would.)
 *
 * When nothing is due, the loop sleeps 2ms and skips the frame entirely
 * -- it keeps polling events, so input latency is bounded by that 2ms,
 * not by the gate.
 *
 * This composes with (rather than duplicating) the occluded-window
 * throttle below: the gate runs first and is a superset -- a throttle
 * probe is explicitly allowed through it so the 5s failsafe still fires.
 *
 * This used to be followed by a second, content-based subtraction (a
 * memcmp of the staged framebuffer, dropping byte-identical republishes)
 * because the LTDC model published every guest frame twice. That is
 * fixed in the device model now -- see gwemu_update_fb_texture() -- so
 * one device publish means one genuinely new frame and the gate's
 * "content" arm needs no second opinion.
 */
#define GNW_UI_ACTIVITY_LINGER_NS   400000000ull   /* 400 ms */
#define GNW_UI_ACTIVE_INTERVAL_NS    16666666ull   /*  60 Hz */
#define GNW_UI_IDLE_HEARTBEAT_NS    100000000ull   /*  10 Hz */
#define GNW_UI_IDLE_SLEEP_NS          2000000ull   /*   2 ms */
/* Refreshed by poll_events()/event_watch_callback() on any SDL event. */
static uint64_t m_ui_activity_until_ns;
/* When gl_render_frame last drew. Every frame the gate admits now goes
 * on to actually render, so stamping it at admission time is the same
 * thing as stamping it at draw time. */
static uint64_t m_ui_last_frame_ns;

static void gwemu_ui_note_activity(void)
{
    m_ui_activity_until_ns = SDL_GetTicksNS() + GNW_UI_ACTIVITY_LINGER_NS;
}

static void gl_render_frame(struct gwemu_console *scon)
{
    static bool rendering;
    if (qatomic_xchg(&rendering, true) || qatomic_read(&qemu_exiting)) {
        return;
    }

    SDL_Texture *tex;

    bool settings_visible = m_settings_window && m_settings_renderer &&
        !(SDL_GetWindowFlags(m_settings_window) & SDL_WINDOW_HIDDEN);
    bool main_occluded = m_main_window_occluded ||
        (SDL_GetWindowFlags(m_window) & SDL_WINDOW_OCCLUDED);
    /*
     * The interactive settings window renders and presents FIRST, and
     * while the main window is occluded (compositor-throttled presents
     * can stall to a few fps under vulkan/XWayland) its present is
     * skipped entirely -- otherwise the settings window, chained behind
     * it in this single loop, went sluggish exactly when the user
     * covered the main window and worked in settings (confirmed field
     * repro). Single-threaded on purpose: SDL renderer APIs are not
     * thread-safe across windows.
     */
    uint64_t frame_now = SDL_GetTicksNS();
    bool trace = gnw_env_enabled("GNW_UI_FRAME_TRACE");
    if (ui_throttle.throttled && m_main_window_resume_evt) {
        /* Event-driven resume: cheap and stall-free, unlike a probe. */
        ui_throttle.throttled = false;
        ui_throttle.slow_streak = 0;
        if (trace) {
            fprintf(stderr, "UI trace: throttle exit resume=event\n");
        }
    }
    m_main_window_resume_evt = false;
    if (!settings_visible) {
        /* Never throttle without a settings window to serve -- an
         * alt-tabbed fullscreen user keeps normal vsync behavior, and
         * closing the wizard/settings always exits throttled mode. */
        ui_throttle.throttled = false;
        ui_throttle.slow_streak = 0;
    }
    bool throttle_retry = ui_throttle.throttled &&
        frame_now - ui_throttle.last_retry_ns >= GNW_PRESENT_RETRY_CADENCE_NS;
    bool skip_main = settings_visible &&
        (main_occluded || (ui_throttle.throttled && !throttle_retry));

    /*
     * Content gate -- see the block comment above GNW_UI_ACTIVITY_LINGER_NS.
     * Runs before any rendering, including the settings window's -- a
     * visible settings window counts as UI-busy, so it renders at the
     * 60Hz active rate and still presents FIRST when it does.
     */
    {
        bool dirty_now = qatomic_read(&m_fb_dirty);
        bool ui_busy = settings_visible ||
            frame_now < m_ui_activity_until_ns;
        if (!ui_busy) {
            /* A menu open/hovered under the cursor wants continuous
             * redraw even with no event traffic. Two atomic bool reads
             * off ImGuiIO -- cheap enough to evaluate every iteration. */
            int kbd = 0, mouse = 0;
            gwemu_hud_should_capture_kbd_mouse(&kbd, &mouse);
            ui_busy = kbd || mouse;
        }
        bool interval_due = frame_now - m_ui_last_frame_ns >=
            (ui_busy ? GNW_UI_ACTIVE_INTERVAL_NS
                     : GNW_UI_IDLE_HEARTBEAT_NS);
        bool due = throttle_retry || dirty_now || interval_due;
        if (trace && due) {
            if (throttle_retry) {
                ui_trace.gate_probe++;
            } else if (dirty_now) {
                ui_trace.gate_content++;
            } else if (ui_busy) {
                ui_trace.gate_active++;
            } else {
                ui_trace.gate_heartbeat++;
            }
        }
        if (!due) {
            if (trace) {
                ui_trace.iters++;
                ui_trace.idle_skipped++;
            }
            /* Nothing pacing the loop now that we're not presenting --
             * sleep, but short enough that input latency is unaffected. */
            SDL_DelayNS(GNW_UI_IDLE_SLEEP_NS);
            qatomic_set(&rendering, false);
            return;
        }
        m_ui_last_frame_ns = frame_now;
    }

    gwemu_main_loop_lock();
    gwemu_settings_hud_update();
    gwemu_main_loop_unlock();
    gwemu_settings_hud_render();
    if (settings_visible) {
        uint64_t t0 = trace ? SDL_GetTicksNS() : 0;
        SDL_RenderPresent(m_settings_renderer);
        if (trace) {
            uint64_t dt = SDL_GetTicksNS() - t0;
            ui_trace.settings_present_ns += dt;
            if (dt > ui_trace.settings_present_max_ns) {
                ui_trace.settings_present_max_ns = dt;
            }
        }
    }

    /* GNW_UI_FRAME_TRACE=1: 1/s pacing summary (field diagnosis of the
     * occluded-main settings sluggishness; off by default). Reports per
     * second: sum+max time inside each window's SDL_RenderPresent, the
     * live SDL_WINDOW_OCCLUDED flag vs the event-tracked occluded bool,
     * and how many frames rendered vs skipped the main window. */
    if (trace) {
        uint64_t now = SDL_GetTicksNS();
        ui_trace.iters++;
        if (skip_main) {
            ui_trace.main_skipped++;
        } else {
            ui_trace.main_rendered++;
        }
        if (now - ui_trace.last_report > 1000000000ull) {
            fprintf(stderr,
                    "UI trace: main present %.1fms sum/%.1fms max, "
                    "settings present %.1fms sum/%.1fms max, "
                    "occluded flag=%d evt=%d, main rendered=%u skipped=%u, "
                    "throttled=%d retries=%u iters=%u idle=%u, "
                    "fb wait=%.1fms held=%.1fms uploads=%u, hud=%.1fms\n",
                    ui_trace.main_present_ns / 1e6,
                    ui_trace.main_present_max_ns / 1e6,
                    ui_trace.settings_present_ns / 1e6,
                    ui_trace.settings_present_max_ns / 1e6,
                    !!(SDL_GetWindowFlags(m_window) & SDL_WINDOW_OCCLUDED),
                    m_main_window_occluded,
                    ui_trace.main_rendered, ui_trace.main_skipped,
                    ui_throttle.throttled, ui_trace.throttle_retries,
                    ui_trace.iters, ui_trace.idle_skipped,
                    ui_trace.fb_wait_ns / 1e6, ui_trace.fb_held_ns / 1e6,
                    ui_trace.fb_uploads, ui_trace.hud_held_ns / 1e6);
            fprintf(stderr,
                    "UI gate: publishes=%d, admitted by "
                    "content=%u probe=%u active=%u heartbeat=%u\n",
                    qatomic_xchg(&m_fb_dirty_sets, 0),
                    ui_trace.gate_content, ui_trace.gate_probe,
                    ui_trace.gate_active, ui_trace.gate_heartbeat);
            memset(&ui_trace, 0, sizeof(ui_trace));
            ui_trace.last_report = now;
        }
    }

    if (skip_main) {
        if (ui_throttle.throttled) {
            /* The settings renderer isn't vsynced and the (skipped) main
             * present was this loop's only pacer -- without a cap the
             * loop free-runs at thousands of iterations/s (field-traced
             * ~4200/s). Cap at ~120 Hz while throttled; normal mode
             * keeps vsync pacing from the main present. */
            SDL_DelayNS(8333333);
        }
        qatomic_set(&rendering, false);
        return;
    }

    SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
    SDL_RenderClear(m_renderer);

    uint64_t fb_t0 = SDL_GetTicksNS();
    if (!qatomic_xchg(&m_fb_dirty, 0) && m_fb_tex) {
        /* Nothing new from the guest since the last upload -- reuse the
         * texture and, crucially, don't take the BQL at all. */
        tex = m_fb_tex;
        goto fb_done;
    }
    gwemu_main_loop_lock();
    uint64_t fb_locked = SDL_GetTicksNS();
    if (!scon->surface) {
        /* No DisplaySurface means the console never attached -- the HUD
         * still draws (menus work) but the game area stays empty. One
         * line so a field log can tell this apart from a texture
         * failure inside gwemu_update_fb_texture(). */
        static bool warned;
        if (!warned) {
            warned = true;
            fprintf(stderr, "fb_texture: no DisplaySurface attached yet\n");
        }
        tex = NULL;
    } else {
        tex = gwemu_update_fb_texture(scon->surface);
    }
    gwemu_main_loop_unlock();
    if (trace) {
        uint64_t fb_end = SDL_GetTicksNS();
        ui_trace.fb_wait_ns += fb_locked - fb_t0;
        ui_trace.fb_held_ns += fb_end - fb_locked;
        ui_trace.fb_uploads++;
    }
    gwemu_upload_fb_texture();
fb_done:

    /* DisplaySurface data is top-down; no GL-style flip needed. */
    gwemu_hud_set_framebuffer_texture(tex, false);

    /* FIXME: Finer locking. Event handlers in segments of the code expect
     * to be running on the main thread with the BQL. For now, acquire the
     * lock and perform rendering, but release before present to avoid
     * possible lengthy blocking (for vsync).
     */
    uint64_t hud_t0 = SDL_GetTicksNS();
    gwemu_main_loop_lock();

    gwemu_hud_update();

    gwemu_main_loop_unlock();
    if (trace) {
        ui_trace.hud_held_ns += SDL_GetTicksNS() - hud_t0;
    }

    gwemu_hud_render();
    {
        /* Timed unconditionally (nanosecond-cheap): this measurement
         * drives the adaptive stall throttle; the tracer just reads it. */
        uint64_t t0 = SDL_GetTicksNS();
        SDL_RenderPresent(m_renderer);
        uint64_t dt = SDL_GetTicksNS() - t0;
        if (trace) {
            ui_trace.main_present_ns += dt;
            if (dt > ui_trace.main_present_max_ns) {
                ui_trace.main_present_max_ns = dt;
            }
            if (throttle_retry) {
                ui_trace.throttle_retries++;
            }
        }
        if (throttle_retry) {
            ui_throttle.last_retry_ns = SDL_GetTicksNS();
        }
        if (dt > GNW_PRESENT_STALL_THRESHOLD_NS) {
            /* Require 2 consecutive slow presents before throttling so a
             * one-off compositor hiccup doesn't trip it. */
            if (settings_visible && ++ui_throttle.slow_streak >= 2) {
                ui_throttle.throttled = true;
                ui_throttle.last_retry_ns = SDL_GetTicksNS();
            }
        } else {
            /* Fast present (incl. a fast fallback probe): the window is
             * visible again -- resume full-rate rendering. */
            if (ui_throttle.throttled && trace) {
                fprintf(stderr, "UI trace: throttle exit resume=probe\n");
            }
            ui_throttle.slow_streak = 0;
            ui_throttle.throttled = false;
        }
    }


    qatomic_set(&rendering, false);

#if DEBUG_GWEMU_C
    report_stats();
#endif
}

static bool event_watch_callback(void *userdata, SDL_Event *event)
{
    struct gwemu_console *scon = (struct gwemu_console *)userdata;

    if (event->type == SDL_EVENT_WINDOW_EXPOSED ||
        event->type == SDL_EVENT_WINDOW_RESIZED) {
        /* This path exists precisely because SDL_PollEvent can block for
         * the duration of a resize/drag -- it must never be content-gated
         * away, so mark activity before rendering. */
        gwemu_ui_note_activity();
        gl_render_frame(scon);
    }

    return true; // Ignored
}

static void poll_events(struct gwemu_console *scon)
{
    SDL_Event ev1, *ev = &ev1;
    bool allow_close = true;

    int kbd = 0, mouse = 0;
    gwemu_hud_should_capture_kbd_mouse(&kbd, &mouse);

    while (SDL_PollEvent(ev)) {
        /* Any event at all -- input, window, controller -- counts as UI
         * activity for the content gate in gl_render_frame(). */
        gwemu_ui_note_activity();

        if (m_settings_window && get_window_id_from_event(ev) == SDL_GetWindowID(m_settings_window)) {
            gwemu_settings_hud_process_sdl_events(ev);
            if (ev->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                SDL_HideWindow(m_settings_window);
            }
            continue;
        }
        gwemu_main_loop_lock();

        // HUD must process events first so that if a controller is detached,
        // a latent rebind request can cancel before the state is freed
        gwemu_hud_process_sdl_events(ev);
        gwemu_input_process_sdl_events(ev);

        // GNW-button remap layer: consumes keyboard/gamepad events bound to
        // a GNW button (synthesizing the emulated device's own fixed qcode
        // for it) or a pending rebind capture -- see gwemu-gnw-input.h. This
        // is the single call site for gnw_input_process_sdl_event();
        // MainMenuScene::ConsumeRebindEvent() (called above via
        // gwemu_hud_process_sdl_events) only reports whether a rebind is
        // currently pending, it does not itself invoke this, so a given
        // event is never processed by the GNW input layer twice. An event
        // consumed here must not also fall through to the raw keyboard
        // passthrough below, or a remapped key would double-fire (once as
        // its own literal keypress, once as the synthesized GNW button).
        bool gnw_consumed = gnw_input_process_sdl_event(ev);

        /* Main-window occlusion tracking (see gl_render_frame) -- kept
         * outside the switch: the window-event range case below would
         * overlap dedicated case labels. */
        if ((ev->type == SDL_EVENT_WINDOW_OCCLUDED ||
             ev->type == SDL_EVENT_WINDOW_EXPOSED) &&
            get_window_id_from_event(ev) == SDL_GetWindowID(m_window)) {
            m_main_window_occluded = ev->type == SDL_EVENT_WINDOW_OCCLUDED;
        }
        /* Adaptive-throttle resume signal (see ui_throttle): any event
         * suggesting the main window became visible again. */
        if ((ev->type == SDL_EVENT_WINDOW_EXPOSED ||
             ev->type == SDL_EVENT_WINDOW_FOCUS_GAINED ||
             ev->type == SDL_EVENT_WINDOW_MOUSE_ENTER ||
             ev->type == SDL_EVENT_WINDOW_RESTORED ||
             ev->type == SDL_EVENT_WINDOW_SHOWN) &&
            get_window_id_from_event(ev) == SDL_GetWindowID(m_window)) {
            m_main_window_resume_evt = true;
        }

        switch (ev->type) {
        case SDL_EVENT_KEY_DOWN:
            if (kbd || gnw_consumed) break;
            handle_keydown(ev);
            break;
        case SDL_EVENT_KEY_UP:
            if (kbd || gnw_consumed) break;
            handle_keyup(ev);
            break;
        case SDL_EVENT_QUIT:
            if (scon->opts->has_window_close && !scon->opts->window_close) {
                allow_close = false;
            }
            if (allow_close) {
                shutdown_action = SHUTDOWN_ACTION_POWEROFF;
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (mouse) break;
            handle_mousemotion(ev);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (mouse) break;
            handle_mousebutton(ev);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (mouse) break;
            handle_mousewheel(ev);
            break;
        case SDL_EVENT_WINDOW_FIRST ... SDL_EVENT_WINDOW_LAST:
            handle_windowevent(ev);
            break;
        default:
            break;
        }

        gwemu_main_loop_unlock();
    }

    gwemu_main_loop_lock();
    gwemu_input_update_controllers();
    gwemu_main_loop_unlock();
}

static void display_very_early_init(DisplayOptions *o)
{
#ifdef __linux__
    /* SDL3 falls back to XWayland on compositors without fifo-v1 [1],
     * breaking HiDPI. Swap may block when occluded, but the BQL is released
     * before swap so emulation is unaffected.
     *
     * [1] https://github.com/libsdl-org/SDL/pull/9383
     */
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland,x11");
#endif

    /* Some systems ship a libdecor Wayland client-side-decoration plugin
     * (libdecor-gtk, cairo/pixman-based) that segfaults when SDL3 dispatches
     * Wayland events through it -- confirmed via a real crash backtrace
     * (pixman_image_composite32 -> cairo -> libdecor-gtk.so ->
     * wl_display_dispatch_queue_pending -> SDL's Wayland_PumpEvents),
     * happening unconditionally on plain "run gwemu with no flags" on such
     * systems. Disable libdecor use; SDL3 then falls back to the
     * xdg-decoration protocol (server-side decorations) where available,
     * or an undecorated window otherwise -- either is preferable to a
     * guaranteed crash. Must be set before SDL_Init().
     */
#ifdef SDL_HINT_VIDEO_WAYLAND_ALLOW_LIBDECOR
    /*
     * Native-Wayland decorations (only relevant when SDL picks the
     * wayland driver, e.g. SDL_VIDEODRIVER=wayland). Facts, all
     * re-verified 2026-07-23 against libdecor 0.2.2 on GNOME:
     *
     * - libdecor's GTK plugin (the only plugin distros ship) crashes
     *   inside this process: GTK3 gets dlopen'd into QEMU's heavily
     *   multi-threaded GLib environment, GObject assertions fire and
     *   the heap corrupts ("malloc(): unaligned tcache chunk
     *   detected") within seconds of window creation.
     * - libdecor's built-in fallback plugin draws NOTHING: its
     *   frame_commit is an empty stub (src/libdecor-fallback.c). It is
     *   not a "plain titlebar" -- pointing LIBDECOR_PLUGIN_DIR at an
     *   empty/nonexistent path guarantees an undecorated window on
     *   GNOME, which never implemented server-side xdg-decoration.
     * - libdecor's CAIRO plugin (not GTK-based, not packaged by
     *   Ubuntu) is stable in-process and draws a real titlebar.
     *
     * So: keep libdecor enabled and point its search path (a
     * colon-separated dir list) at a gwemu-local plugin dir next to
     * the executable, where a build of libdecor's cairo plugin can be
     * dropped ("libdecor-plugins/libdecor-cairo.so"). The system
     * plugin dir is deliberately NOT on the path, so the crashing GTK
     * plugin can never load. If the local dir is absent, libdecor
     * falls back to its built-in no-op plugin: undecorated but
     * stable. Both knobs respect pre-set user overrides (setenv
     * overwrite=0 / hint only set if unset).
     */
#ifdef __linux__
    if (getenv("LIBDECOR_PLUGIN_DIR") == NULL) {
        char exe_path[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", exe_path,
                               sizeof(exe_path) - 1);
        if (len > 0) {
            char plugin_dir[PATH_MAX + 32];
            exe_path[len] = '\0';
            snprintf(plugin_dir, sizeof(plugin_dir), "%s/libdecor-plugins",
                     dirname(exe_path));
            setenv("LIBDECOR_PLUGIN_DIR", plugin_dir, 0);
        } else {
            setenv("LIBDECOR_PLUGIN_DIR",
                   "/nonexistent-gwemu-no-libdecor-plugins", 0);
        }
    }
#endif
    if (SDL_GetHint(SDL_HINT_VIDEO_WAYLAND_ALLOW_LIBDECOR) == NULL) {
        SDL_SetHint(SDL_HINT_VIDEO_WAYLAND_ALLOW_LIBDECOR, "1");
    }
#endif

#ifdef __linux__
    /*
     * Default to the x11 driver (XWayland on Wayland desktops) unless
     * the user explicitly chose one via SDL_VIDEODRIVER. Native Wayland
     * has cost this project three separate real-world breakages so far:
     * the libdecor crash above, ImGui's multi-viewport mouse-coordinate
     * whitelist (see gwemu_hud_init()), and (post-SDL_Renderer port)
     * wrong UI scaling/window sizing under fractional display scale --
     * confirmed side by side: identical build renders correctly under
     * x11 and mis-scaled under wayland. Revisit if SDL3's Wayland
     * fractional-scale reporting stabilizes; until then x11 is the
     * config that actually works everywhere. SDL_VIDEODRIVER=wayland
     * still forces native Wayland for testing.
     */
    if (getenv("SDL_VIDEODRIVER") == NULL) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
    }
#endif

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "Failed to initialize SDL video subsystem: %s\n",
                SDL_GetError());
        exit(1);
    }

#ifdef SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR /* only available since SDL 2.0.8 */
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");

    // No GL context needed: rendering goes through SDL_Renderer, which
    // picks the native accelerated backend per platform (D3D11 on
    // Windows, Metal on macOS, OpenGL/software elsewhere). The previous
    // hard GL 3.2 Core requirement was pure xemu inheritance and cost a
    // real "Unable to create OpenGL context" failure on a real Windows
    // machine.

    char *title = g_strdup_printf("GWemu | v%s"
#ifdef GWEMU_DEBUG_BUILD
                                  " Debug"
#endif
                                  , gwemu_version);

    // Decide window size. The window's size follows the emulated screen's
    // native resolution x an integer scale -- not the other way around
    // (a window-size-drives-content-fit model was tried twice and was
    // backwards per the user). GNW_NATIVE_WIDTH/HEIGHT match the real,
    // fixed G&W LCD panel (hw/display/gnw_h7b0_ltdc.c's own documented
    // 320x240 + its GNW_H7B0_LTDC_SCALE=2 "1:1 is uncomfortably small"
    // precedent) -- a real hardware fact, not a guess, even though LTDC's
    // registers are technically configurable (every real firmware image
    // configures this same panel size in practice). xemu's old res_table
    // here was a list of Xbox AV-pack output resolutions (720p, 1080p,
    // etc.) -- meaningless for a fixed small G&W panel, dropped entirely.
    #define GNW_NATIVE_WIDTH  320
    #define GNW_NATIVE_HEIGHT 240
    #define GNW_DEFAULT_SCALE 2

    int min_window_width = GNW_NATIVE_WIDTH;
    int min_window_height = GNW_NATIVE_HEIGHT;
    int window_width = GNW_NATIVE_WIDTH * GNW_DEFAULT_SCALE;
    int window_height = GNW_NATIVE_HEIGHT * GNW_DEFAULT_SCALE;

    if (g_config.display.window.startup_size == CONFIG_DISPLAY_WINDOW_STARTUP_SIZE_LAST_USED &&
        g_config.display.window.last_width > 0 && g_config.display.window.last_height > 0) {
        window_width  = g_config.display.window.last_width;
        window_height = g_config.display.window.last_height;
    }
    // Other CONFIG_DISPLAY_WINDOW_STARTUP_SIZE_* enum values (xemu's old
    // Xbox-resolution presets) are no longer meaningful here -- fall
    // through to the native x GNW_DEFAULT_SCALE default above for any of
    // them, same as "no saved size yet".

    if (window_width < min_window_width) {
        window_width = min_window_width;
    }
    if (window_height < min_window_height) {
        window_height = min_window_height;
    }

    // Native window decorations: a custom borderless titlebar (drawn via
    // SDL_SetWindowHitTest) was tried and reverted -- confirmed broken on
    // real-world Wayland (no decorations, non-functional menus), since
    // SDL's hit-test support isn't reliably honored by every compositor.
    // Depend on the host WM's own decorations instead.
    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);

    // Create main window
    m_window = SDL_CreateWindow(
        title, window_width, window_height,
        window_flags);
#ifdef __linux__
    if (m_window == NULL) {
        // Wayland always creates GL contexts via EGL (no GLX equivalent
        // exists there) -- on compositors/driver stacks where that EGL
        // path is broken (confirmed real: "Could not get EGL display"
        // on a real user's system), window creation fails outright even
        // though SDL_Init(SDL_INIT_VIDEO) itself already succeeded under
        // the wayland driver. X11 doesn't have this constraint (GLX is
        // far more broadly supported), so retry once, forcing x11
        // specifically, instead of leaving the user to discover the
        // SDL_VIDEODRIVER=x11 workaround (already documented in
        // CLAUDE.md for a different Wayland issue) manually.
        const char *cur_driver = SDL_GetCurrentVideoDriver();
        if (cur_driver && strcmp(cur_driver, "wayland") == 0) {
            fprintf(stderr,
                    "Failed to create main window under Wayland (%s) -- "
                    "retrying with the x11 driver.\n", SDL_GetError());
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
            if (SDL_InitSubSystem(SDL_INIT_VIDEO)) {
                m_window = SDL_CreateWindow(title, window_width,
                                             window_height, window_flags);
            }
        }
    }
#endif
    if (m_window == NULL) {
        fprintf(stderr, "Failed to create main window: %s\n", SDL_GetError());
        SDL_Quit();
        exit(1);
    }
    g_free(title);

    /*
     * window_width/height (and the saved last_width/last_height) are
     * PHYSICAL pixels, but SDL_CreateWindow/SDL_SetWindowSize take
     * POINTS. On Wayland with fractional scaling (density > 1, e.g.
     * 1.75x GNOME) the window just created is density-times too large
     * physically compared to the same numbers under x11 (where density
     * is 1 and points == pixels) -- the guest viewport then fills that
     * oversized window and looks ~2x too big while the HUD (which
     * scales by display_scale/density) still looks right. The density
     * is only queryable once a window exists, so correct the size
     * immediately after creation. No-op when density == 1.
     */
    float win_density = fmaxf(SDL_GetWindowPixelDensity(m_window), 1.0f);
    if (win_density > 1.0f) {
        gwemu_snap_window_points(m_window, window_width, window_height,
                                 &window_width, &window_height);
        gwemu_snap_window_points(m_window, min_window_width,
                                 min_window_height, &min_window_width,
                                 &min_window_height);
        SDL_SetWindowSize(m_window, window_width, window_height);
    }
    SDL_SetWindowMinimumSize(m_window, min_window_width, min_window_height);

    const SDL_DisplayMode *disp_mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(m_window));
    if (disp_mode && (disp_mode->w < window_width || disp_mode->h < window_height)) {
        SDL_SetWindowSize(m_window, min_window_width, min_window_height);
        SDL_SetWindowPosition(m_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }

#ifdef __linux__
    /* Prefer Vulkan over GL on Linux (project owner's call) -- the hint
     * only biases SDL's driver order; unavailable Vulkan still falls
     * back to GL and ultimately the software renderer below. */
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "vulkan");
#endif
    
    m_renderer = SDL_CreateRenderer(m_window, NULL);
    
    /* Comfortable default, but never larger than the desktop's usable
     * area -- a window that opens bigger than the screen leaves no
     * reachable edge to resize it with. */
    int set_w = 1100, set_h = 700;
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable)) {
        set_w = MIN(set_w, usable.w * 9 / 10);
        set_h = MIN(set_h, usable.h * 9 / 10);
    }
    m_settings_window = SDL_CreateWindow("Settings", set_w, set_h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (m_settings_window) {
        m_settings_renderer = SDL_CreateRenderer(m_settings_window, NULL);
    }

    if (m_renderer == NULL) {
        SDL_ResetHint(SDL_HINT_RENDER_DRIVER);
        m_renderer = SDL_CreateRenderer(m_window, NULL);
    }
    if (m_renderer == NULL) {
        /* Last resort: SDL's software renderer -- slow but universal. */
        fprintf(stderr, "Failed to create accelerated renderer (%s) -- "
                "falling back to software rendering.\n", SDL_GetError());
        m_renderer = SDL_CreateRenderer(m_window, SDL_SOFTWARE_RENDERER);
    }

    if (m_renderer == NULL) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
            "Unable to create renderer",
            "Unable to create any SDL renderer (accelerated or software).\r\n"
            "\r\n"
            "GWemu cannot continue and will now exit.",
            m_window);
        SDL_DestroyWindow(m_window);
        SDL_Quit();
        exit(1);
    }

    // xemu's own branded window icon (data/xemu_64x64.png.h) isn't
    // appropriate for this fork -- no gnw-h7b0 icon asset exists yet,
    // leave the platform default rather than ship the wrong branding.

    fprintf(stderr, "CPU: %s\n", gwemu_get_cpu_info());
    fprintf(stderr, "OS_Version: %s\n", gwemu_get_os_info());
    fprintf(stderr, "SDL_RENDERER: %s\n", SDL_GetRendererName(m_renderer));
}

SDL_Renderer *gwemu_get_renderer(void)
{
    return m_renderer;
}

/*
 * Copy the current guest framebuffer as tightly-packed RGBA8888,
 * top-down. Caller frees *pixels with free(). Used by the screenshot/
 * thumbnail path (ui/xui/gl-helpers.cc) so nothing ever needs to read
 * pixels back from the SDL renderer backend.
 */
bool gwemu_get_fb_pixels(uint8_t **pixels, int *w, int *h)
{
    bool ok = false;

    gwemu_main_loop_lock();
    DisplaySurface *surface = scon_list ? scon_list[0].surface : NULL;
    if (surface) {
        int sw = surface_width(surface), sh = surface_height(surface);
        int stride = surface_stride(surface);
        uint8_t *src = surface_data(surface);
        uint8_t *dst = malloc((size_t)sw * sh * 4);
        if (dst) {
            pixman_format_code_t fmt = surface_format(surface);
            for (int y = 0; y < sh; y++) {
                const uint8_t *row = src + (size_t)y * stride;
                uint8_t *out = dst + (size_t)y * sw * 4;
                for (int x = 0; x < sw; x++) {
                    uint8_t r, g, b;
                    if (fmt == PIXMAN_r5g6b5) {
                        uint16_t px = ((const uint16_t *)row)[x];
                        r = ((px >> 11) & 0x1f) << 3;
                        g = ((px >> 5) & 0x3f) << 2;
                        b = (px & 0x1f) << 3;
                    } else {
                        /* x8r8g8b8 little-endian: B G R X in memory */
                        r = row[x * 4 + 2];
                        g = row[x * 4 + 1];
                        b = row[x * 4 + 0];
                    }
                    out[x * 4 + 0] = r;
                    out[x * 4 + 1] = g;
                    out[x * 4 + 2] = b;
                    out[x * 4 + 3] = 0xff;
                }
            }
            *pixels = dst;
            *w = sw;
            *h = sh;
            ok = true;
        }
    }
    gwemu_main_loop_unlock();
    return ok;
}

static void display_early_init(DisplayOptions *o)
{
    assert(o->type == DISPLAY_TYPE_GWEMU);
    SDL_SetRenderVSync(m_renderer, g_config.display.window.vsync ?
                       1 : SDL_RENDERER_VSYNC_DISABLED);
    
    gwemu_hud_init(m_window, m_renderer);
    if (m_settings_window && m_settings_renderer) {
        gwemu_settings_hud_init(m_settings_window, m_settings_renderer);
    }

}

static const DisplayChangeListenerOps dcl_gl_ops = {
    .dpy_name                = "gwemu",
    .dpy_gfx_switch          = gl_switch,
    .dpy_gfx_update          = gl_update,
    .dpy_gfx_check_format    = xb_console_gl_check_format,
    .dpy_mouse_set           = mouse_warp,
    .dpy_cursor_define       = mouse_define,
};

static void display_init(DisplayState *ds, DisplayOptions *o)
{
    uint8_t data = 0;
    int i;

    assert(o->type == DISPLAY_TYPE_GWEMU);

    gui_fullscreen = o->has_full_screen && o->full_screen;
    gui_fullscreen |= g_config.display.window.fullscreen_on_startup;

    num_outputs = 1;
    scon_list = g_new0(struct gwemu_console, num_outputs);
    for (i = 0; i < num_outputs; i++) {
        QemuConsole *con = qemu_console_lookup_by_index(i);
        assert(con != NULL);
        if (!qemu_console_is_graphic(con) &&
            qemu_console_get_index(con) != 0) {
            scon_list[i].hidden = true;
        }
        scon_list[i].idx = i;
        scon_list[i].opts = o;
        scon_list[i].dcl.ops = &dcl_gl_ops;
        scon_list[i].dcl.con = con;
        scon_list[i].kbd = qkbd_state_init(con);
        register_displaychangelistener(&scon_list[i].dcl);

#if defined(SDL_VIDEO_DRIVER_WINDOWS)
        HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(scon_list[i].real_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        if (hwnd) {
            qemu_console_set_window_id(con, (uintptr_t)hwnd);
        }
#elif defined(SDL_VIDEO_DRIVER_X11)
        Window xwindow = (Window)SDL_GetNumberProperty(SDL_GetWindowProperties(scon_list[i].real_window), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        if (xwindow) {
            qemu_console_set_window_id(con, xwindow);
        }
#endif
    }

    scon_list[0].real_window = m_window;
    scon_list[0].renderer = m_renderer;

    mouse_mode_notifier.notify = mouse_mode_change;
    qemu_add_mouse_mode_change_notifier(&mouse_mode_notifier);

    sdl_cursor_hidden = SDL_CreateCursor(&data, &data, 8, 1, 0, 0);
    sdl_cursor_normal = SDL_GetCursor();

    // SDL_PollEvent may block during main window resize or drag operations.
    // Register event watch to handle rendering during these operations.
    SDL_AddEventWatch(event_watch_callback, &scon_list[0]);

    if (use_vblank_timer_thread) {
        qemu_thread_create(&vblank_thread, "vblank-timer", vblank_timer_thread,
                           &scon_list[0], QEMU_THREAD_JOINABLE);
    } else {
        vblank_timer = timer_new_ns(QEMU_CLOCK_REALTIME, vblank_timer_callback, &scon_list[0]);
        timer_mod_ns(vblank_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + vblank_interval_ns);
    }

    /* Tell main thread to go ahead and create the app and enter the run loop */
    qemu_sem_post(&display_init_sem);
}

static void display_finalize(void)
{
    if (use_vblank_timer_thread) {
        qemu_thread_join(&vblank_thread);
    }

    SDL_RemoveEventWatch(event_watch_callback, &scon_list[0]);
    if (m_fb_tex) {
        SDL_DestroyTexture(m_fb_tex);
        m_fb_tex = NULL;
    }
    SDL_DestroyRenderer(m_renderer);
    SDL_DestroyWindow(m_window);
    SDL_Quit();
}

static QemuDisplay qemu_display_gwemu = {
    .type       = DISPLAY_TYPE_GWEMU,
    .early_init = display_early_init,
    .init       = display_init,
};

static void register_gwemu_display(void)
{
    qemu_display_register(&qemu_display_gwemu);
}

type_init(register_gwemu_display);

int gArgc;
char **gArgv;

static void *qemu_main(void *opaque)
{
    qemu_init(gArgc, gArgv);
    exit_status = qemu_main_loop();
    qatomic_set(&qemu_exiting, true);
    bql_unlock();

    qemu_sem_wait(&display_shutdown_sem);
    bql_lock();
    qemu_cleanup(exit_status);
    bql_unlock();

    return NULL;
}

static void init_sdl_app_metadata(void)
{
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, "GWemu");
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_VERSION_STRING,
                               gwemu_version);
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_IDENTIFIER_STRING,
                               "app.gwemu.gwemu");
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_URL_STRING,
                               "https://github.com/slash-proc/gwemu");
}

static int g_orig_argc;
static char **g_orig_argv;

// True for a -global gnw-h7b0-soc.{bank1,bank2,extflash}-image=... argument
// (whole "-global x=y" pair, i.e. also skip the following argv entry).
static bool is_flash_image_global(const char *arg)
{
    return strncmp(arg, "gnw-h7b0-soc.bank1-image=", 25) == 0 ||
           strncmp(arg, "gnw-h7b0-soc.bank2-image=", 25) == 0 ||
           strncmp(arg, "gnw-h7b0-soc.extflash-image=", 28) == 0;
}

void gwemu_relaunch_with_flash_images(const char *bank1_image,
                                      const char *bank2_image,
                                      const char *extflash_image,
                                      const char *sdcard_qcow2)
{
    GPtrArray *new_argv = g_ptr_array_new();
    g_ptr_array_add(new_argv, g_orig_argv[0]);
    for (int i = 1; i < g_orig_argc; i++) {
        if (strcmp(g_orig_argv[i], "-global") == 0 && i + 1 < g_orig_argc &&
            is_flash_image_global(g_orig_argv[i + 1])) {
            i++; // also skip the "gnw-h7b0-soc.*-image=..." argument itself
            continue;
        }
        if (strcmp(g_orig_argv[i], "-drive") == 0 && i + 1 < g_orig_argc &&
            strstr(g_orig_argv[i + 1], "if=sd") != NULL) {
            i++; // skip a pre-existing SD -drive pair (mirrors -global)
            continue;
        }
        if (strcmp(g_orig_argv[i], "-S") == 0) {
            // scripts/boot_qemu.sh --gui starts halted (-S) because a
            // fully blank vector table is a real ARMv7-M lockup, not a
            // safe idle state -- once real images are configured we want
            // a normal running boot, so drop it on relaunch.
            continue;
        }
        g_ptr_array_add(new_argv, g_orig_argv[i]);
    }

    char *b1 = NULL, *b2 = NULL, *ef = NULL;
    if (bank1_image && bank1_image[0]) {
        b1 = g_strdup_printf("gnw-h7b0-soc.bank1-image=%s", bank1_image);
        g_ptr_array_add(new_argv, (char *)"-global");
        g_ptr_array_add(new_argv, b1);
    }
    if (bank2_image && bank2_image[0]) {
        b2 = g_strdup_printf("gnw-h7b0-soc.bank2-image=%s", bank2_image);
        g_ptr_array_add(new_argv, (char *)"-global");
        g_ptr_array_add(new_argv, b2);
    }
    if (extflash_image && extflash_image[0]) {
        ef = g_strdup_printf("gnw-h7b0-soc.extflash-image=%s", extflash_image);
        g_ptr_array_add(new_argv, (char *)"-global");
        g_ptr_array_add(new_argv, ef);
    }
    char *sd = NULL;
    if (sdcard_qcow2 && sdcard_qcow2[0]) {
        sd = g_strdup_printf("if=sd,format=qcow2,file=%s", sdcard_qcow2);
        g_ptr_array_add(new_argv, (char *)"-drive");
        g_ptr_array_add(new_argv, sd);
    }
    /* -config_path is gwemu-private and was compacted OUT of argv before
     * qemu_init() (and out of g_orig_argv, captured post-compaction) --
     * re-append it from the live settings path so a profile-scoped config
     * survives the relaunch. */
    const char *cfg_path = gwemu_settings_get_path();
    char *cfg = NULL;
    if (cfg_path && cfg_path[0]) {
        cfg = g_strdup(cfg_path);
        g_ptr_array_add(new_argv, (char *)"-config_path");
        g_ptr_array_add(new_argv, cfg);
    }
    g_ptr_array_add(new_argv, NULL);

    // execv() replaces this process image and does NOT run atexit handlers
    // -- gwemu_settings_save() is normally also registered via atexit() as
    // a safety net, but that never fires here. Flush explicitly so any
    // settings changed but not yet individually saved aren't silently lost
    // on relaunch.
    gwemu_settings_save();

    /* One-shot diagnostic: this path replaces the process image, so log
     * exactly what we are about to exec -- if the new process fails to
     * appear, this is the only record of why. */
    for (guint n = 0; n + 1 < new_argv->len; n++) {
        fprintf(stderr, "relaunch: argv[%u]=%s\n", n,
                (const char *)g_ptr_array_index(new_argv, n));
    }
    {
        char cwd_buf[PATH_MAX];
        fprintf(stderr, "relaunch: cwd=%s\n",
                getcwd(cwd_buf, sizeof(cwd_buf)) ? cwd_buf : "(unknown)");
    }

    execv(g_orig_argv[0], (char *const *)new_argv->pdata);
    /* execv only returns on failure. Be loud: a silent fall-through here
     * previously looked identical to "the app vanished". */
    fprintf(stderr, "relaunch: execv FAILED: %s\n", strerror(errno));
    abort();
}

// This fork only has one real machine (gnw-h7b0), unlike genuinely
// multi-machine upstream QEMU -- there's no reason to force explicit
// "-M gnw-h7b0 -display gwemu" on every invocation. If the user's own
// argv doesn't already specify a machine/display, inject sensible
// defaults; if it also doesn't configure any flash image, additionally
// start halted (-S) for the same reason scripts/boot_qemu.sh --gui
// already does: a fully blank vector table (SP=0, PC=0) is a genuine
// ARMv7-M lockup the instant the CPU runs, not a safe idle state, and
// QEMU treats that as fatal for the whole process. Returns a new,
// intentionally-leaked (process-lifetime) NULL-terminated argv -- same
// leak convention gwemu_relaunch_with_flash_images() already uses for
// its own argv construction, since this only ever runs once at startup.
static bool g_is_query_invocation = false;
/*
 * -display none: run as plain upstream QEMU -- no SDL, no window, no HUD.
 * The GUI bootstrap below unconditionally creates the SDL window BEFORE
 * qemu_init() ever parses -display (xemu inheritance: xemu is GUI-first
 * and never had a headless mode), which left every "headless" run with a
 * dead black window the WM flags as not-responding. Headless (timeline
 * capture, CI, gdb-only firmware bring-up) must be genuinely windowless.
 */
static bool g_is_headless_invocation = false;

static char **inject_default_args(int argc, char **argv, int *out_argc)
{
    bool have_machine = false, have_display = false, have_flash_image = false;
    bool is_query = false;
    for (int i = 1; i < argc; i++) {
        if (!argv[i]) continue;
        if (strcmp(argv[i], "-M") == 0 || strcmp(argv[i], "-machine") == 0) {
            have_machine = true;
            if (i + 1 < argc && argv[i + 1] && strcmp(argv[i + 1], "help") == 0) {
                is_query = true;
            }
        } else if (strcmp(argv[i], "-display") == 0) {
            have_display = true;
            if (i + 1 < argc && argv[i + 1] && strcmp(argv[i + 1], "help") == 0) {
                is_query = true;
            }
            if (i + 1 < argc && argv[i + 1] &&
                (strcmp(argv[i + 1], "none") == 0 ||
                 strncmp(argv[i + 1], "none,", 5) == 0)) {
                g_is_headless_invocation = true;
            }
        } else if (strcmp(argv[i], "-version") == 0 ||
                   strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "-help") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            is_query = true;
        } else if (strcmp(argv[i], "-global") == 0 && i + 1 < argc &&
                   argv[i + 1] && is_flash_image_global(argv[i + 1])) {
            have_flash_image = true;
        } else if (strcmp(argv[i], "-device") == 0 && i + 1 < argc &&
                   argv[i + 1] &&
                   strncmp(argv[i + 1], "loader,", 7) == 0 &&
                   strstr(argv[i + 1], "file=") != NULL) {
            /*
             * A generic-loader device with a backing file boots real code
             * just as much as a -global ...-image= binding does. Missing
             * this was a genuine trap: boot_qemu.sh's --ephemeral and --diag
             * paths (and any hand-written command line following them) load
             * firmware exclusively this way, so they were treated as
             * imageless, got -S appended below, and came up in
             * "paused (prelaunch)" with a black screen -- looking like a
             * hung emulator rather than a deliberately halted one, with
             * nothing on stderr to say why.
             */
            have_flash_image = true;
        }
    }

    // A query invocation (-M help, -version, etc.) wants plain stdout
    // output and a clean exit -- forcing a real SDL3 display/window
    // to initialize for it is wrong even interactively, and outright
    // breaks it on a headless host (CI runners, no display server),
    // where display init failing silently eats the help text this is
    // supposed to print. Pass such invocations through unmodified.
    if (is_query) {
        g_is_query_invocation = true;
        *out_argc = argc;
        return argv;
    }

    GPtrArray *new_argv = g_ptr_array_new();
    g_ptr_array_add(new_argv, argv[0]);
    if (!have_machine) {
        g_ptr_array_add(new_argv, (char *)"-M");
        g_ptr_array_add(new_argv, (char *)"gnw-h7b0");
    }
    if (!have_display) {
        g_ptr_array_add(new_argv, (char *)"-display");
        g_ptr_array_add(new_argv, (char *)"gwemu");
    }
    for (int i = 1; i < argc; i++) {
        g_ptr_array_add(new_argv, argv[i]);
    }
    if (!have_flash_image) {
        g_ptr_array_add(new_argv, (char *)"-S");
    }
    g_ptr_array_add(new_argv, NULL);

    *out_argc = (int)new_argv->len - 1;
    return (char **)g_ptr_array_free(new_argv, FALSE);
}

int main(int argc, char **argv)
{
    QemuThread thread;

    argv = inject_default_args(argc, argv, &argc);

    // A query invocation (-M help, -version, etc.) doesn't need any of
    // gwemu's own GUI bootstrap (SDL/display init, settings/config load)
    // -- qemu_init() handles these itself (prints and exit()s before
    // ever reaching display setup), same as the plain !CONFIG_GWEMU_GUI
    // build's qemu_default_main(). Skipping this bootstrap here matters
    // in practice: forcing a real SDL video init for these breaks on any
    // headless host (no display server), e.g. `-M help` in CI.
    if (g_is_query_invocation) {
        setlocale(LC_NUMERIC, "C");
        qemu_init(argc, argv);
        exit(0);
    }

    if (g_is_headless_invocation) {
        /*
         * Mirror system/main.c's non-GUI path (qemu_init +
         * qemu_default_main's body): main loop on this thread, holding
         * the locks qemu_init acquired. Nothing gwemu-specific runs --
         * no SDL, no settings GUI, no window; env-gated headless
         * features (GNW_TIMELINE/GNW_RECORD) live in device code and
         * work regardless of display backend.
         */
        int status;

        setlocale(LC_NUMERIC, "C");
        fprintf(stderr, "gwemu_version: %s (headless)\n", gwemu_version);
        qemu_init(argc, argv);
        status = qemu_main_loop();
        qemu_cleanup(status);
        bql_unlock();
        exit(status);
    }

    setlocale(LC_NUMERIC, "C");

#ifdef _WIN32
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        // Launched with a console. If stdout and stderr are not associated with
        // an output stream, redirect to parent console.
        if (_fileno(stdout) == -2) {
            freopen("CONOUT$", "w+", stdout);
        }
        if (_fileno(stderr) == -2) {
            freopen("CONOUT$", "w+", stderr);
        }
    } else {
        // Launched without a console. Redirect stdout and stderr to a log file.
        HANDLE logfile = CreateFileA("gwemu.log",
            GENERIC_WRITE, FILE_SHARE_WRITE|FILE_SHARE_READ,
            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (logfile != INVALID_HANDLE_VALUE) {
            freopen("gwemu.log", "a", stdout);
            freopen("gwemu.log", "a", stderr);
        }
    }

    _set_error_mode(_OUT_TO_STDERR);
#endif

    fprintf(stderr, "gwemu_version: %s\n", gwemu_version);
    fprintf(stderr, "gwemu_commit: %s\n", gwemu_commit);
    fprintf(stderr, "gwemu_date: %s\n", gwemu_date);

    init_sdl_app_metadata();

    for (int i = 1; i < argc; i++) {
        if (argv[i] && strcmp(argv[i], "-config_path") == 0) {
            int consumed = 1;
            if (i < argc - 1 && argv[i + 1]) {
                gwemu_settings_set_path(argv[i + 1]);
                consumed = 2;
            }
            /* Compact argv rather than NULLing the slots: qemu_init()'s
             * option walk dereferences argv[optind] unconditionally, so a
             * NULL hole here segfaulted the whole process the moment
             * -config_path was actually used. */
            for (int j = i; j + consumed <= argc; j++) {
                argv[j] = argv[j + consumed];
            }
            argc -= consumed;
            break;
        }
    }

    /* Capture AFTER the -config_path compaction above: capturing before it
     * left g_orig_argc at the pre-compaction count while the array had been
     * shifted (NULL hole at the new argc, stale duplicate at the end), so
     * gwemu_relaunch_with_flash_images() walked past the end and crashed on
     * strcmp(NULL, ...) -- the "Launch makes the app vanish" bug. The
     * relaunch path re-appends -config_path itself from the saved settings
     * path so the compaction doesn't lose it across an execv(). */
    g_orig_argc = argc;
    g_orig_argv = argv;

    gArgc = argc;
    gArgv = argv;

    if (!gwemu_settings_load()) {
        const char *err_msg = gwemu_settings_get_error_message();
        fprintf(stderr, "%s", err_msg);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
            "Failed to load gwemu config file", err_msg,
            m_window);
        SDL_Quit();
        exit(1);
    }
    atexit(gwemu_settings_save);
    gnw_input_load();
    atexit(gnw_input_save);

    bool has_flash_images = false;
    for (int i = 1; i < g_orig_argc; i++) {
        if (strcmp(g_orig_argv[i], "-global") == 0 && i + 1 < g_orig_argc &&
            is_flash_image_global(g_orig_argv[i + 1])) {
            has_flash_images = true;
            break;
        }
    }
    if (!has_flash_images) {
        gwemu_auto_launch_active_profile();
    }

    display_very_early_init(NULL);

    qemu_sem_init(&display_init_sem, 0);
    qemu_sem_init(&display_shutdown_sem, 0);
    qemu_thread_create(&thread, "qemu_main", qemu_main,
                       NULL, QEMU_THREAD_JOINABLE);
    qemu_sem_wait(&display_init_sem);

    gui_grab = 0;
    if (gui_fullscreen) {
        grab_start(0);
        set_full_screen(&scon_list[0], gui_fullscreen);
    }

    /*
     * FIXME: May want to create a callback mechanism for main QEMU thread
     * to just run functions to avoid TLS bugs and locking issues.
     *
     * xemu's own tcg_register_init_ctx() (a fork-specific tcg.c addition,
     * flagged FIXME even in their own source) isn't present in this
     * project's unmodified upstream tcg.c -- dropped for Phase 1. If
     * TCG-thread-local/locking issues show up at runtime on the GUI
     * thread, this is the first place to look.
     */
    qemu_set_current_aio_context(qemu_get_aio_context());

    gwemu_main_loop_lock();
    gwemu_input_init();
    gwemu_main_loop_unlock();

    struct gwemu_console *scon = &scon_list[0];
    while (!qatomic_read(&qemu_exiting)) {
        poll_events(scon);
        gl_render_frame(scon);
    }
    qemu_sem_post(&display_shutdown_sem);
    qemu_thread_join(&thread);
    display_finalize();
    return exit_status;
}

/*
 * xemu's xemu_eject_disc()/xemu_load_disc() (Xbox CD/DVD drive tray
 * emulation via hw/xbox/smbus.h's xbox_smc_*() calls, both this board
 * doesn't have) are dropped -- see actions.cc/popup-menu.cc.
 */
