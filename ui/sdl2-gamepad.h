/*
 * SDL2 game-controller bridge for the gnw-h7b0 board -- see sdl2-gamepad.c
 * for rationale.
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

#ifndef UI_SDL2_GAMEPAD_H
#define UI_SDL2_GAMEPAD_H

#include "ui/sdl2.h"

/* Init SDL's game-controller subsystem and open the first controller, if
 * any is already connected. Safe to call unconditionally. */
void sdl2_gamepad_init(void);

/* Close any open controller and quit the subsystem. Call once at UI
 * teardown. */
void sdl2_gamepad_fini(void);

/* Feed one SDL_Event through the gamepad bridge. Ignores event types it
 * doesn't care about, so it's safe to call for every event pumped out of
 * SDL_PollEvent() in sdl2_poll_events(). */
void sdl2_gamepad_handle_event(SDL_Event *ev);

#endif /* UI_SDL2_GAMEPAD_H */
