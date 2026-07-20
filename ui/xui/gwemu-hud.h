/*
 * GWemu User Interface
 *
 * Subsystem handling primary graphical user interface, which can be controlled
 * via mouse and keyboard or through any attached gamepad.
 *
 * Copyright (C) 2020-2021 Matt Borgerson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef GWEMU_HUD_H
#define GWEMU_HUD_H

#include <SDL3/SDL.h>
#include <epoxy/gl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Implemented in gwemu.c
int gwemu_is_fullscreen(void);
void gwemu_toggle_fullscreen(void);
SDL_Window *gwemu_get_window(void);
void gwemu_main_loop_lock(void);
void gwemu_main_loop_unlock(void);

// Implemented in gwemu_hud.cc
void gwemu_hud_init(SDL_Window *window, void *sdl_gl_context);
void gwemu_hud_cleanup(void);
void gwemu_hud_update(void);
void gwemu_hud_render(void);
void gwemu_hud_process_sdl_events(SDL_Event *event);
void gwemu_hud_should_capture_kbd_mouse(int *kbd, int *mouse);
void gwemu_hud_set_framebuffer_texture(GLuint tex, bool flip);
bool gwemu_hud_get_framebuffer_size(int *w, int *h);

/*
 * Re-exec the current process with bank1_image/bank2_image/extflash_image
 * substituted for whatever -global gnw-h7b0-soc.{bank1,bank2,extflash}-image=
 * flags were on the original command line (added fresh if none were present),
 * every other original argument preserved as-is. Pass NULL for an image to
 * omit that -global flag entirely (falls back to blank RAM, matching a
 * default/no-image launch). Does not return on success (execv replaces the
 * process image) -- QEMU cannot hot-swap a real-ized RAM MemoryRegion's file
 * backing while running, so a full restart is the correct mechanism here,
 * not a workaround.
 */
void gwemu_relaunch_with_flash_images(const char *bank1_image,
                                      const char *bank2_image,
                                      const char *extflash_image);

#ifdef __cplusplus
}
#endif

#endif
