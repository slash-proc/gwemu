/*
 * gnw-h7b0 GUI -- GNW-button input remapping implementation.
 * See gwemu-gnw-input.h.
 */
#include "qemu/osdep.h"
#include "qapi/qapi-types-ui.h"
#include "ui/input.h"
#include "gwemu-gnw-input.h"
#include "gwemu-input.h"
#include "gwemu-settings.h"

const char *const gnw_input_btn_names[GNW_INPUT_BTN__COUNT] = {
    [GNW_INPUT_BTN_PAUSE]  = "Pause",
    [GNW_INPUT_BTN_GAME]   = "Game",
    [GNW_INPUT_BTN_TIME]   = "Time",
    [GNW_INPUT_BTN_A]      = "A",
    [GNW_INPUT_BTN_B]      = "B",
    [GNW_INPUT_BTN_LEFT]   = "Left",
    [GNW_INPUT_BTN_DOWN]   = "Down",
    [GNW_INPUT_BTN_RIGHT]  = "Right",
    [GNW_INPUT_BTN_UP]     = "Up",
    [GNW_INPUT_BTN_PWR]    = "Power",
    [GNW_INPUT_BTN_START]  = "Start",
    [GNW_INPUT_BTN_SELECT] = "Select",
};

/*
 * The device's own FIXED default button->QKeyCode table (mirrors
 * hw/misc/gnw_h7b0_gpio.c's gnw_h7b0_key_map exactly -- duplicated here
 * because that array is file-local static in the device model and this
 * is what always gets synthesized on a matching physical input,
 * regardless of what the physical input currently bound to a given GNW
 * button actually is).
 */
static const int gnw_default_qcode[GNW_INPUT_BTN__COUNT] = {
    [GNW_INPUT_BTN_PAUSE]  = Q_KEY_CODE_ESC,
    [GNW_INPUT_BTN_GAME]   = Q_KEY_CODE_G,
    [GNW_INPUT_BTN_TIME]   = Q_KEY_CODE_T,
    [GNW_INPUT_BTN_A]      = Q_KEY_CODE_X,
    [GNW_INPUT_BTN_B]      = Q_KEY_CODE_Z,
    [GNW_INPUT_BTN_LEFT]   = Q_KEY_CODE_LEFT,
    [GNW_INPUT_BTN_DOWN]   = Q_KEY_CODE_DOWN,
    [GNW_INPUT_BTN_RIGHT]  = Q_KEY_CODE_RIGHT,
    [GNW_INPUT_BTN_UP]     = Q_KEY_CODE_UP,
    [GNW_INPUT_BTN_PWR]    = Q_KEY_CODE_P,
    [GNW_INPUT_BTN_START]  = Q_KEY_CODE_RET,
    [GNW_INPUT_BTN_SELECT] = Q_KEY_CODE_SHIFT_R,
};

/* GUI-side, live-editable: which physical key/gamepad button currently
 * triggers each GNW button. Keyboard bindings default to the same
 * QKeyCode as gnw_default_qcode (so an unconfigured GUI behaves exactly
 * like today's boot_qemu.sh launches); gamepad bindings default to the
 * obvious d-pad/face-button convention where one exists, -1 (unbound)
 * otherwise (TIME/GAME/PWR have no generic-gamepad convention). */
static int gnw_key_binding[GNW_INPUT_BTN__COUNT];
static int gnw_pad_binding[GNW_INPUT_BTN__COUNT];

static void set_defaults(void)
{
    for (int i = 0; i < GNW_INPUT_BTN__COUNT; i++) {
        gnw_key_binding[i] = gnw_default_qcode[i];
        gnw_pad_binding[i] = -1;
    }
    gnw_pad_binding[GNW_INPUT_BTN_UP]     = SDL_GAMEPAD_BUTTON_DPAD_UP;
    gnw_pad_binding[GNW_INPUT_BTN_DOWN]   = SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    gnw_pad_binding[GNW_INPUT_BTN_LEFT]   = SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    gnw_pad_binding[GNW_INPUT_BTN_RIGHT]  = SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    gnw_pad_binding[GNW_INPUT_BTN_B]      = SDL_GAMEPAD_BUTTON_SOUTH;
    gnw_pad_binding[GNW_INPUT_BTN_A]      = SDL_GAMEPAD_BUTTON_EAST;
    gnw_pad_binding[GNW_INPUT_BTN_START]  = SDL_GAMEPAD_BUTTON_START;
    gnw_pad_binding[GNW_INPUT_BTN_SELECT] = SDL_GAMEPAD_BUTTON_BACK;
    gnw_pad_binding[GNW_INPUT_BTN_PAUSE]  = SDL_GAMEPAD_BUTTON_GUIDE;
}

void gnw_input_reset_defaults(void)
{
    set_defaults();
}

static char *keyfile_path(void)
{
    return g_strdup_printf("%s/gnw-input.ini", gwemu_settings_get_base_path());
}

void gnw_input_load(void)
{
    set_defaults();

    g_autofree char *path = keyfile_path();
    g_autoptr(GKeyFile) kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        return; /* No saved bindings yet -- defaults stand. */
    }

    for (int i = 0; i < GNW_INPUT_BTN__COUNT; i++) {
        GError *err = NULL;
        int v = g_key_file_get_integer(kf, "keyboard", gnw_input_btn_names[i], &err);
        if (!err) {
            gnw_key_binding[i] = v;
        }
        g_clear_error(&err);

        v = g_key_file_get_integer(kf, "gamepad", gnw_input_btn_names[i], &err);
        if (!err) {
            gnw_pad_binding[i] = v;
        }
        g_clear_error(&err);
    }
}

void gnw_input_save(void)
{
    g_autoptr(GKeyFile) kf = g_key_file_new();
    for (int i = 0; i < GNW_INPUT_BTN__COUNT; i++) {
        g_key_file_set_integer(kf, "keyboard", gnw_input_btn_names[i], gnw_key_binding[i]);
        g_key_file_set_integer(kf, "gamepad", gnw_input_btn_names[i], gnw_pad_binding[i]);
    }
    g_autofree char *path = keyfile_path();
    g_autoptr(GError) err = NULL;
    g_key_file_save_to_file(kf, path, &err);
}

/* Pending rebind capture state. */
static bool rebinding;
static bool rebind_is_gamepad;
static int  rebind_btn = -1;

void gnw_input_begin_rebind(int gnw_btn, bool is_gamepad)
{
    rebinding = true;
    rebind_is_gamepad = is_gamepad;
    rebind_btn = gnw_btn;
}

void gnw_input_cancel_rebind(void)
{
    rebinding = false;
    rebind_btn = -1;
}

bool gnw_input_is_rebinding(void)
{
    return rebinding;
}

const char *gnw_input_get_binding_name(int gnw_btn, bool is_gamepad)
{
    static char buf[64];
    if (is_gamepad) {
        int b = gnw_pad_binding[gnw_btn];
        if (b < 0) {
            return "Unbound";
        }
        snprintf(buf, sizeof(buf), "Pad #%d", b);
        return buf;
    } else {
        int qcode = gnw_key_binding[gnw_btn];
        const char *name = QKeyCode_str((QKeyCode)qcode);
        return name ? name : "Unbound";
    }
}

static int sdl_scancode_to_qcode(SDL_Scancode sc)
{
    if ((unsigned)sc >= qemu_input_map_usb_to_qcode_len) {
        return -1;
    }
    return qemu_input_map_usb_to_qcode[sc];
}

bool gnw_input_process_sdl_event(const SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        if (ev->key.repeat) {
            return false;
        }
        int qcode = sdl_scancode_to_qcode(ev->key.scancode);
        if (qcode < 0) {
            return false;
        }

        if (rebinding && !rebind_is_gamepad && ev->type == SDL_EVENT_KEY_DOWN) {
            gnw_key_binding[rebind_btn] = qcode;
            gnw_input_cancel_rebind();
            gnw_input_save();
            return true;
        }

        for (int i = 0; i < GNW_INPUT_BTN__COUNT; i++) {
            if (gnw_key_binding[i] == qcode) {
                qemu_input_event_send_key_qcode(
                    NULL, (QKeyCode)gnw_default_qcode[i],
                    ev->type == SDL_EVENT_KEY_DOWN);
                return true;
            }
        }
        return false;
    }

    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        int button = ev->gbutton.button;

        if (rebinding && rebind_is_gamepad &&
            ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            gnw_pad_binding[rebind_btn] = button;
            gnw_input_cancel_rebind();
            gnw_input_save();
            return true;
        }

        for (int i = 0; i < GNW_INPUT_BTN__COUNT; i++) {
            if (gnw_pad_binding[i] == button) {
                qemu_input_event_send_key_qcode(
                    NULL, (QKeyCode)gnw_default_qcode[i],
                    ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                return true;
            }
        }
        return false;
    }

    default:
        return false;
    }
}

void gnw_input_synth_press(int gnw_btn)
{
    if (gnw_btn < 0 || gnw_btn >= GNW_INPUT_BTN__COUNT) {
        return;
    }
    QKeyCode qcode = (QKeyCode)gnw_default_qcode[gnw_btn];
    qemu_input_event_send_key_qcode(NULL, qcode, true);
    qemu_input_event_send_key_qcode(NULL, qcode, false);
}
