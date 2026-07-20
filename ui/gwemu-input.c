/*
 * gnw-h7b0 User Interface -- minimal input state (Phase 1)
 *
 * See gwemu-input.h for scope/rationale. Real host-gamepad polling (needed
 * for ImGui HUD navigation), no Xbox-USB-device binding.
 */

#include "qemu/osdep.h"
#include "gwemu-input.h"

ControllerStateList available_controllers =
    QTAILQ_HEAD_INITIALIZER(available_controllers);

static bool g_test_mode;

void gwemu_input_init(void)
{
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
}

void gwemu_input_set_test_mode(bool enabled)
{
    g_test_mode = enabled;
}

static ControllerState *find_or_add(SDL_JoystickID id)
{
    ControllerState *iter;
    QTAILQ_FOREACH (iter, &available_controllers, entry) {
        if (iter->sdl_joystick_id == id) {
            return iter;
        }
    }

    ControllerState *state = g_new0(ControllerState, 1);
    state->sdl_joystick_id = id;
    state->sdl_gamepad = SDL_OpenGamepad(id);
    state->type = INPUT_DEVICE_SDL_GAMEPAD;
    state->name = state->sdl_gamepad ? SDL_GetGamepadName(state->sdl_gamepad)
                                      : "Unknown";
    QTAILQ_INSERT_TAIL(&available_controllers, state, entry);
    return state;
}

void gwemu_input_process_sdl_events(const SDL_Event *event)
{
    if (event->type == SDL_EVENT_GAMEPAD_ADDED) {
        find_or_add(event->gdevice.which);
    } else if (event->type == SDL_EVENT_GAMEPAD_REMOVED) {
        ControllerState *iter, *next;
        QTAILQ_FOREACH_SAFE (iter, &available_controllers, entry, next) {
            if (iter->sdl_joystick_id == event->gdevice.which) {
                QTAILQ_REMOVE(&available_controllers, iter, entry);
                if (iter->sdl_gamepad) {
                    SDL_CloseGamepad(iter->sdl_gamepad);
                }
                g_free(iter);
            }
        }
    }
}

static int16_t axis_get(SDL_Gamepad *gp, SDL_GamepadAxis axis)
{
    return SDL_GetGamepadAxis(gp, axis);
}

void gwemu_input_update_controllers(void)
{
    ControllerState *state;
    QTAILQ_FOREACH (state, &available_controllers, entry) {
        if (!state->sdl_gamepad) {
            continue;
        }
        SDL_Gamepad *gp = state->sdl_gamepad;
        uint16_t buttons = 0;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_SOUTH)) buttons |= CONTROLLER_BUTTON_A;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_EAST))  buttons |= CONTROLLER_BUTTON_B;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_WEST))  buttons |= CONTROLLER_BUTTON_X;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_NORTH)) buttons |= CONTROLLER_BUTTON_Y;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_LEFT))  buttons |= CONTROLLER_BUTTON_DPAD_LEFT;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_UP))    buttons |= CONTROLLER_BUTTON_DPAD_UP;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) buttons |= CONTROLLER_BUTTON_DPAD_RIGHT;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_DOWN))  buttons |= CONTROLLER_BUTTON_DPAD_DOWN;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_BACK))  buttons |= CONTROLLER_BUTTON_BACK;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_START)) buttons |= CONTROLLER_BUTTON_START;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))  buttons |= CONTROLLER_BUTTON_WHITE;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) buttons |= CONTROLLER_BUTTON_BLACK;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_LEFT_STICK))  buttons |= CONTROLLER_BUTTON_LSTICK;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) buttons |= CONTROLLER_BUTTON_RSTICK;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_GUIDE)) buttons |= CONTROLLER_BUTTON_GUIDE;
        state->buttons = buttons;

        state->axis[CONTROLLER_AXIS_LTRIG] = axis_get(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        state->axis[CONTROLLER_AXIS_RTRIG] = axis_get(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        state->axis[CONTROLLER_AXIS_LSTICK_X] = axis_get(gp, SDL_GAMEPAD_AXIS_LEFTX);
        state->axis[CONTROLLER_AXIS_LSTICK_Y] = axis_get(gp, SDL_GAMEPAD_AXIS_LEFTY);
        state->axis[CONTROLLER_AXIS_RSTICK_X] = axis_get(gp, SDL_GAMEPAD_AXIS_RIGHTX);
        state->axis[CONTROLLER_AXIS_RSTICK_Y] = axis_get(gp, SDL_GAMEPAD_AXIS_RIGHTY);
    }
}
