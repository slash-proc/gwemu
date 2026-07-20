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
    SDL_GLContext winctx;
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
static SDL_GLContext m_context;
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

static void window_resize(struct gwemu_console *scon)
{
    if (!scon->real_window) {
        return;
    }

    SDL_SetWindowSize(scon->real_window,
                      surface_width(scon->surface),
                      surface_height(scon->surface));
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
                g_config.display.window.last_width = ev->window.data1;
                g_config.display.window.last_height = ev->window.data2;
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
static void xb_surface_gl_create_texture(DisplaySurface *surface)
{
    assert(QEMU_IS_ALIGNED(surface_stride(surface), surface_bytes_per_pixel(surface)));

    GLenum glformat, gltype;
    switch (surface_format(surface)) {
    case PIXMAN_BE_b8g8r8x8:
    case PIXMAN_BE_b8g8r8a8:
        glformat = GL_BGRA_EXT;
        gltype = GL_UNSIGNED_BYTE;
        break;
    case PIXMAN_BE_x8r8g8b8:
    case PIXMAN_BE_a8r8g8b8:
        glformat = GL_RGBA;
        gltype = GL_UNSIGNED_BYTE;
        break;
    case PIXMAN_r5g6b5:
        glformat = GL_RGB;
        gltype = GL_UNSIGNED_SHORT_5_6_5;
        break;
    default:
        g_assert_not_reached();
    }

    if (!surface->texture) {
        glGenTextures(1, &surface->texture);
    }
    glBindTexture(GL_TEXTURE_2D, surface->texture);
    glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT,
                  surface_stride(surface) / surface_bytes_per_pixel(surface));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                 surface_width(surface),
                 surface_height(surface),
                 0, glformat, gltype,
                 surface_data(surface));
    glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
}

static void xb_surface_gl_destroy_texture(DisplaySurface *surface)
{
    if (!surface || !surface->texture) {
        return;
    }
    glDeleteTextures(1, &surface->texture);
    surface->texture = 0;
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

static void gl_switch(DisplayChangeListener *dcl,
                      DisplaySurface *new_surface)
{
    struct gwemu_console *scon = container_of(dcl, struct gwemu_console, dcl);
    scon->surface = new_surface;
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
            SDL_DelayPrecise(next_vblank - now);
        } else if (now > next_vblank + vblank_interval_ns) {
            // We've fallen behind by more than one frame, reset to avoid
            // rapid-fire catch-up
            next_vblank = now;
        }

        if (!qatomic_read(&qemu_exiting)) {
            gwemu_main_loop_lock();
            process_vblank(scon);
            gwemu_main_loop_unlock();
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
static void gl_render_frame(struct gwemu_console *scon)
{
    static bool rendering;
    if (qatomic_xchg(&rendering, true) || qatomic_read(&qemu_exiting)) {
        return;
    }

    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);

    bool flip_required = false;
    bool release_surface_texture = false;

    /*
     * xemu has an NV2A-GPU fast path here (nv2a_get_framebuffer_surface())
     * to grab an already-GPU-side texture directly, falling back to this
     * generic console-surface upload otherwise. gnw-h7b0 has no such GPU
     * device -- LTDC renders through the ordinary QemuConsole/DisplaySurface
     * path like any other QEMU display device, so always take the generic
     * path (this is exactly what every non-accelerated xemu guest already
     * exercises, not a new/untested path).
     */
    GLuint tex = 0;

    assert(glGetError() == GL_NO_ERROR);

    if (tex == 0) {
        gwemu_main_loop_lock();
        // FIXME: Don't upload if notdirty
        xb_surface_gl_create_texture(scon->surface);
        tex = scon->surface->texture;
        flip_required = true;
        release_surface_texture = true;
        gwemu_main_loop_unlock();
    }

    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    gwemu_hud_set_framebuffer_texture(tex, flip_required);

    /* FIXME: Finer locking. Event handlers in segments of the code expect
     * to be running on the main thread with the BQL. For now, acquire the
     * lock and perform rendering, but release before swap to avoid
     * possible lengthy blocking (for vsync).
     */
    gwemu_main_loop_lock();
    gwemu_hud_update();
    gwemu_main_loop_unlock();

    gwemu_hud_render();
    glFinish();

    if (release_surface_texture) {
        gwemu_main_loop_lock();
        xb_surface_gl_destroy_texture(scon->surface);
        gwemu_main_loop_unlock();
    }

    SDL_GL_SwapWindow(scon->real_window);
    assert(glGetError() == GL_NO_ERROR);

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
    SDL_SetHint(SDL_HINT_VIDEO_WAYLAND_ALLOW_LIBDECOR, "0");
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

    // Initialize rendering context
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    // GL 4.0 was inherited straight from xemu, which needs it for real
    // Xbox/NV2A framebuffer features (see ShaderType::BlitGamma in
    // gl-helpers.cc, its own "FIXME: Move to nv2a_get_framebuffer_surface"
    // comment). We don't use that shader anywhere reachable -- every
    // shader this GUI actually instantiates (Mask/Blit/Logo) is
    // "#version 150 core" (GLSL 1.50 = GL 3.2), matching what ImGui's
    // own OpenGL3 backend is initialized with ("#version 150" in
    // main.cc). GL 4.0 Core was needlessly failing to create a context
    // at all on real hardware that only supports GL 3.2/3.3 (confirmed:
    // "Unable to create OpenGL context" on a real Windows machine).
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_PROFILE_MASK,
        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

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
    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);

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
                // GL attributes are subsystem-lifetime state -- quitting
                // and reinitializing SDL_INIT_VIDEO can drop them back
                // to defaults, so the retry needs them set again before
                // the second SDL_CreateWindow() or GLX visual matching
                // can fail for a completely different reason than the
                // original EGL failure (real user report: "Couldn't
                // find matching GLX visual" on the retry, X11/GLX
                // otherwise works fine on that exact machine).
                SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
                SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
                SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
                SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
                SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
                SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                                     SDL_GL_CONTEXT_PROFILE_CORE);
                SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
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
    SDL_SetWindowMinimumSize(m_window, min_window_width, min_window_height);

    const SDL_DisplayMode *disp_mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(m_window));
    if (disp_mode && (disp_mode->w < window_width || disp_mode->h < window_height)) {
        SDL_SetWindowSize(m_window, min_window_width, min_window_height);
        SDL_SetWindowPosition(m_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }

    m_context = SDL_GL_CreateContext(m_window);

    if (m_context != NULL && epoxy_gl_version() < 32) {
        SDL_GL_MakeCurrent(NULL, NULL);
        SDL_GL_DestroyContext(m_context);
        m_context = NULL;
    }

    if (m_context == NULL) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
            "Unable to create OpenGL context",
            "Unable to create OpenGL context. This usually means the\r\n"
            "graphics device on this system does not support OpenGL 3.2.\r\n"
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
    fprintf(stderr, "GL_VENDOR: %s\n", glGetString(GL_VENDOR));
    fprintf(stderr, "GL_RENDERER: %s\n", glGetString(GL_RENDERER));
    fprintf(stderr, "GL_VERSION: %s\n", glGetString(GL_VERSION));
    fprintf(stderr, "GL_SHADING_LANGUAGE_VERSION: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));

    SDL_GL_MakeCurrent(NULL, NULL);
}

static void display_early_init(DisplayOptions *o)
{
    assert(o->type == DISPLAY_TYPE_GWEMU);
    display_opengl = 1;

    SDL_GL_MakeCurrent(m_window, m_context);
    SDL_GL_SetSwapInterval(g_config.display.window.vsync ? 1 : 0);
    gwemu_hud_init(m_window, m_context);
}

static const DisplayChangeListenerOps dcl_gl_ops = {
    .dpy_name                = "gwemu-gl",
    .dpy_gfx_switch          = gl_switch,
    .dpy_gfx_check_format    = xb_console_gl_check_format,
    .dpy_mouse_set           = mouse_warp,
    .dpy_cursor_define       = mouse_define,
};

static void display_init(DisplayState *ds, DisplayOptions *o)
{
    uint8_t data = 0;
    int i;

    assert(o->type == DISPLAY_TYPE_GWEMU);
    SDL_GL_MakeCurrent(m_window, m_context);

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
    scon_list[0].winctx = m_context;

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
    SDL_GL_MakeCurrent(NULL, NULL);
    qemu_sem_post(&display_init_sem);
}

static void display_finalize(void)
{
    if (use_vblank_timer_thread) {
        qemu_thread_join(&vblank_thread);
    }

    SDL_RemoveEventWatch(event_watch_callback, &scon_list[0]);
    SDL_GL_MakeCurrent(NULL, NULL);
    SDL_GL_DestroyContext(m_context);
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
                                      const char *extflash_image)
{
    GPtrArray *new_argv = g_ptr_array_new();
    g_ptr_array_add(new_argv, g_orig_argv[0]);
    for (int i = 1; i < g_orig_argc; i++) {
        if (strcmp(g_orig_argv[i], "-global") == 0 && i + 1 < g_orig_argc &&
            is_flash_image_global(g_orig_argv[i + 1])) {
            i++; // also skip the "gnw-h7b0-soc.*-image=..." argument itself
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
    g_ptr_array_add(new_argv, NULL);

    // execv() replaces this process image and does NOT run atexit handlers
    // -- gwemu_settings_save() is normally also registered via atexit() as
    // a safety net, but that never fires here. Flush explicitly so any
    // settings changed but not yet individually saved aren't silently lost
    // on relaunch.
    gwemu_settings_save();

    execv(g_orig_argv[0], (char *const *)new_argv->pdata);
    // execv only returns on failure -- nothing sane to do but report it and
    // keep running with the old configuration rather than exit silently.
    fprintf(stderr, "gwemu_relaunch_with_flash_images: execv failed: %s\n",
            strerror(errno));
    g_free(b1);
    g_free(b2);
    g_free(ef);
    g_ptr_array_free(new_argv, TRUE);
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
        } else if (strcmp(argv[i], "-version") == 0 ||
                   strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "-help") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            is_query = true;
        } else if (strcmp(argv[i], "-global") == 0 && i + 1 < argc &&
                   argv[i + 1] && is_flash_image_global(argv[i + 1])) {
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

    g_orig_argc = argc;
    g_orig_argv = argv;

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

    gArgc = argc;
    gArgv = argv;

    for (int i = 1; i < argc; i++) {
        if (argv[i] && strcmp(argv[i], "-config_path") == 0) {
            argv[i] = NULL;
            if (i < argc - 1 && argv[i+1]) {
                gwemu_settings_set_path(argv[i+1]);
                argv[i+1] = NULL;
            }
            break;
        }
    }

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
