/*
 * gnw-h7b0 User Interface -- minimal input state (Phase 1)
 *
 * Structural reference: xemu's own ui/xemu-input.h. xemu's real
 * implementation binds a host gamepad to an emulated Xbox USB controller
 * device (xemu_input_bind, XMU peripheral emulation, rumble-to-device) --
 * none of that applies here; G&W input already goes through GPIO keycodes
 * (see ui/sdl2-gamepad.c, this project's existing mechanism). This only
 * keeps the generic host-gamepad polling used for ImGui HUD navigation
 * (input-manager.cc).
 */

#ifndef GWEMU_INPUT_H
#define GWEMU_INPUT_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include "qemu/queue.h"

enum controller_state_buttons_mask {
    CONTROLLER_BUTTON_A          = (1 << 0),
    CONTROLLER_BUTTON_B          = (1 << 1),
    CONTROLLER_BUTTON_X          = (1 << 2),
    CONTROLLER_BUTTON_Y          = (1 << 3),
    CONTROLLER_BUTTON_DPAD_LEFT  = (1 << 4),
    CONTROLLER_BUTTON_DPAD_UP    = (1 << 5),
    CONTROLLER_BUTTON_DPAD_RIGHT = (1 << 6),
    CONTROLLER_BUTTON_DPAD_DOWN  = (1 << 7),
    CONTROLLER_BUTTON_BACK       = (1 << 8),
    CONTROLLER_BUTTON_START      = (1 << 9),
    CONTROLLER_BUTTON_WHITE      = (1 << 10),
    CONTROLLER_BUTTON_BLACK      = (1 << 11),
    CONTROLLER_BUTTON_LSTICK     = (1 << 12),
    CONTROLLER_BUTTON_RSTICK     = (1 << 13),
    CONTROLLER_BUTTON_GUIDE      = (1 << 14),
};

enum controller_state_axis_index {
    CONTROLLER_AXIS_LTRIG,
    CONTROLLER_AXIS_RTRIG,
    CONTROLLER_AXIS_LSTICK_X,
    CONTROLLER_AXIS_LSTICK_Y,
    CONTROLLER_AXIS_RSTICK_X,
    CONTROLLER_AXIS_RSTICK_Y,
    CONTROLLER_AXIS__COUNT,
};

enum controller_input_device_type {
    INPUT_DEVICE_SDL_KEYBOARD,
    INPUT_DEVICE_SDL_GAMEPAD,
};

typedef struct ControllerState {
    QTAILQ_ENTRY(ControllerState) entry;

    uint16_t buttons;
    int16_t  axis[CONTROLLER_AXIS__COUNT];

    enum controller_input_device_type type;
    const char    *name;
    SDL_Gamepad   *sdl_gamepad;
    SDL_JoystickID sdl_joystick_id;
} ControllerState;

typedef QTAILQ_HEAD(, ControllerState) ControllerStateList;
extern ControllerStateList available_controllers;

#ifdef __cplusplus
extern "C" {
#endif

void gwemu_input_init(void);
void gwemu_input_process_sdl_events(const SDL_Event *event);
void gwemu_input_update_controllers(void);
void gwemu_input_set_test_mode(bool enabled);

#ifdef __cplusplus
}
#endif

#endif
