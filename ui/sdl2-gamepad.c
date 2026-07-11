/*
 * SDL2 game-controller bridge for the gnw-h7b0 board
 *
 * Polls SDL2's SDL_GameController API (Xbox/xpad-style controllers show up
 * here via SDL's built-in gamecontrollerdb mapping) and synthesizes QEMU
 * keyboard InputEvents through qemu_input_event_send_key_qcode(), reusing
 * the exact keyboard qcodes gnw_h7b0_gpio.c's gnw_h7b0_key_map already
 * understands. This is a deliberate shortcut (see xemu, which does the
 * same thing against its own SDL/ImGui UI): this project's pinned QEMU
 * base (v9.2) has no real joystick/gamepad InputEvent kind in
 * qapi/ui.json, and adding one is upstream-scale work we're not taking on
 * here. Because events are synthesized as ordinary key events, zero
 * changes are required in gnw_h7b0_gpio.c -- they flow through the normal
 * input handler dispatch exactly like a real keypress.
 *
 * Only the first connected controller is used, with a fixed button
 * mapping (no rebinding, no analog stick support -- G&W games are
 * digital d-pad only). Inert when SDL2 game-controller support isn't
 * built or no controller is plugged in.
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
#include "ui/console.h"
#include "ui/input.h"
#include "ui/sdl2.h"
#include "ui/sdl2-gamepad.h"

/*
 * SDL_GameControllerButton -> qcode mapping. Mirrors the keyboard
 * convention documented in hw/misc/gnw_h7b0_gpio.c's gnw_h7b0_key_map:
 * arrow keys for the d-pad, X/Z for A/B (note SDL's controller A/B
 * labels are swapped vs G&W's physical layout in typical Xbox-pad
 * convention -- SDL button A, the "bottom" face button, is mapped to
 * GNW B/Z here, and SDL button B, the "right" face button, to GNW A/X,
 * to keep the physical left-to-right feel consistent with the keyboard
 * mapping's Z=B/X=A convention), Return for Start, Right-Shift for
 * Select/Back, and Escape (mapped from Guide/Xbox button, falling back
 * to the right shoulder button if Guide isn't reported by the driver)
 * for Pause.
 */
static const int gnw_h7b0_pad_map[SDL_CONTROLLER_BUTTON_MAX] = {
    [0 ... SDL_CONTROLLER_BUTTON_MAX - 1] = -1,
    [SDL_CONTROLLER_BUTTON_DPAD_UP]       = Q_KEY_CODE_UP,
    [SDL_CONTROLLER_BUTTON_DPAD_DOWN]     = Q_KEY_CODE_DOWN,
    [SDL_CONTROLLER_BUTTON_DPAD_LEFT]     = Q_KEY_CODE_LEFT,
    [SDL_CONTROLLER_BUTTON_DPAD_RIGHT]    = Q_KEY_CODE_RIGHT,
    [SDL_CONTROLLER_BUTTON_A]             = Q_KEY_CODE_Z,
    [SDL_CONTROLLER_BUTTON_B]             = Q_KEY_CODE_X,
    [SDL_CONTROLLER_BUTTON_START]         = Q_KEY_CODE_RET,
    [SDL_CONTROLLER_BUTTON_BACK]          = Q_KEY_CODE_SHIFT_R,
    [SDL_CONTROLLER_BUTTON_GUIDE]         = Q_KEY_CODE_ESC,
    [SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = Q_KEY_CODE_ESC,
};

static SDL_GameController *gnw_gamepad;

void sdl2_gamepad_init(void)
{
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        return;
    }

    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            gnw_gamepad = SDL_GameControllerOpen(i);
            break;
        }
    }
}

void sdl2_gamepad_fini(void)
{
    if (gnw_gamepad) {
        SDL_GameControllerClose(gnw_gamepad);
        gnw_gamepad = NULL;
    }
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

void sdl2_gamepad_handle_event(SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_CONTROLLERDEVICEADDED:
        if (!gnw_gamepad && SDL_IsGameController(ev->cdevice.which)) {
            gnw_gamepad = SDL_GameControllerOpen(ev->cdevice.which);
        }
        break;

    case SDL_CONTROLLERDEVICEREMOVED:
        if (gnw_gamepad &&
            ev->cdevice.which ==
                SDL_JoystickInstanceID(
                    SDL_GameControllerGetJoystick(gnw_gamepad))) {
            SDL_GameControllerClose(gnw_gamepad);
            gnw_gamepad = NULL;
        }
        break;

    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
        uint8_t button = ev->cbutton.button;

        if (button < SDL_CONTROLLER_BUTTON_MAX &&
            gnw_h7b0_pad_map[button] != -1) {
            qemu_input_event_send_key_qcode(
                NULL, gnw_h7b0_pad_map[button],
                ev->type == SDL_CONTROLLERBUTTONDOWN);
        }
        break;
    }

    default:
        break;
    }
}
