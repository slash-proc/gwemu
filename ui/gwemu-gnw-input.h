/*
 * gnw-h7b0 GUI -- GNW-button input remapping (physical key/gamepad ->
 * QEMU keycode, GUI-owned, no changes to hw/misc/gnw_h7b0_gpio.c)
 *
 * See gnw_h7b0_gpio.c's gnw_h7b0_key_map for the emulated device's own
 * FIXED default button->QKeyCode table -- that table never changes at
 * runtime (it's a device property parsed once at realize). This module
 * owns a separate, GUI-side, live-editable table of physical inputs
 * (keyboard scancode or gamepad button) -> GNW button, and on a matching
 * physical event always synthesizes the device's own fixed default
 * qcode for that GNW button via qemu_input_event_send_key_qcode(). This
 * means remapping takes effect immediately with no relaunch, and the
 * emulated device never has to know remapping exists.
 *
 * Every raw keyboard event that matches an active binding is consumed
 * here (the caller should skip its own raw keyboard passthrough for a
 * consumed event) -- G&W has no text-entry use case competing for literal
 * keyboard passthrough, so there is no reason to double-deliver a key
 * both as a literal keypress and as a synthesized GNW button.
 */
#ifndef GWEMU_GNW_INPUT_H
#define GWEMU_GNW_INPUT_H

#include <SDL3/SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GNW_INPUT_BTN_PAUSE,
    GNW_INPUT_BTN_GAME,
    GNW_INPUT_BTN_TIME,
    GNW_INPUT_BTN_A,
    GNW_INPUT_BTN_B,
    GNW_INPUT_BTN_LEFT,
    GNW_INPUT_BTN_DOWN,
    GNW_INPUT_BTN_RIGHT,
    GNW_INPUT_BTN_UP,
    GNW_INPUT_BTN_PWR,
    GNW_INPUT_BTN_START,
    GNW_INPUT_BTN_SELECT,
    GNW_INPUT_BTN__COUNT,
};

extern const char *const gnw_input_btn_names[GNW_INPUT_BTN__COUNT];

/* Load/save bindings (GLib keyfile at gwemu_settings_get_base_path()/gnw-input.ini). */
void gnw_input_load(void);
void gnw_input_save(void);

/*
 * Feed every SDL event here (keyboard + gamepad button down/up). Returns
 * true if the event was consumed (either delivered as a synthesized GNW
 * button press/release, or captured to complete a pending rebind) -- the
 * caller should skip its own default handling for a consumed keyboard
 * event.
 */
bool gnw_input_process_sdl_event(const SDL_Event *ev);

/* True while waiting for the next physical input to complete a rebind. */
bool gnw_input_is_rebinding(void);

/* Start capturing the next keyboard press (is_gamepad=false) or gamepad
 * button press (is_gamepad=true) as the new binding for gnw_btn. */
void gnw_input_begin_rebind(int gnw_btn, bool is_gamepad);

/* Cancel a pending rebind capture without changing anything. */
void gnw_input_cancel_rebind(void);

/* Human-readable current binding, e.g. "G" or "A (gamepad)" or "Unbound". */
const char *gnw_input_get_binding_name(int gnw_btn, bool is_gamepad);

/* Reset every binding back to the device's own fixed defaults. */
void gnw_input_reset_defaults(void);

/*
 * Synthesize a press-then-release of gnw_btn's fixed default qcode
 * directly (GNW_INPUT_BTN_* index, not a physical input) -- for GUI
 * buttons (e.g. a Power button) that should look identical to the guest
 * as a real physical press, without needing an actual key/gamepad event.
 * Goes through the same qemu_input_event_send_key_qcode() path as a real
 * binding match, so hw/misc/gnw_h7b0_gpio.c's real PA0/EXTI0 logic
 * handles it exactly as it would a real press.
 */
void gnw_input_synth_press(int gnw_btn);

#ifdef __cplusplus
}
#endif

#endif
