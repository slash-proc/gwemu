//
// gwemu User Interface -- custom window titlebar
//
// The main game window is created SDL_WINDOW_BORDERLESS (see ui/gwemu.c) so
// we're never at the mercy of whatever window-decoration library the host
// happens to have installed (a real, confirmed crash on this dev machine:
// the system's only available libdecor plugin, libdecor-gtk, segfaults --
// see ui/gwemu.c's display_very_early_init() for the SDL_HINT that avoids
// it -- and even once that's avoided, this compositor doesn't support
// SDL3's xdg-decoration server-side fallback either, leaving a genuinely
// undecorated, unmovable/unresizable window). Draw our own instead, using
// SDL_SetWindowHitTest() so SDL/the OS still does the real move/resize
// work -- this is not a simulated drag, it's the same mechanism a native
// decoration would use.
//
#ifndef GWEMU_TITLEBAR_H
#define GWEMU_TITLEBAR_H

#ifdef __cplusplus
#include <SDL3/SDL.h>

// Draws the custom titlebar, returns its height in (already-scaled) pixels
// so the caller can push the rest of the UI (menu bar, game view) down by
// that amount.
float DrawTitlebar();
#endif

#ifdef __cplusplus
extern "C" {
#endif

// C-callable from ui/gwemu.c: registers TitlebarHitTest on the given window
// via SDL_SetWindowHitTest(). Call once, right after window creation.
void titlebar_install_hittest(struct SDL_Window *window);

#ifdef __cplusplus
}
#endif

#endif
