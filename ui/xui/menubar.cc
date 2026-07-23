//
// gnw-h7b0 User Interface -- minimal top menu bar (Phase 1)
//
#include "common.hh"
#include "menubar.hh"
#include "actions.hh"
#include "main-menu.hh"
#include "scene-manager.hh"
#include "misc.hh"
#include "monitor.hh"
#include "gwemu-hud.h"
#include "../gwemu-settings.h"
#include "../gwemu-gnw-input.h"

/*
 * Trimmed from xemu's own ProcessKeyboardShortcuts() -- disc eject/load
 * shortcuts dropped (see actions.cc), everything else is generic.
 */
void ProcessKeyboardShortcuts(void)
{
    if (IsShortcutKeyPressed(ImGuiKey_P)) {
        ActionTogglePause();
    }
    if (IsShortcutKeyPressed(ImGuiKey_R)) {
        ActionReset();
    }
    if (IsShortcutKeyPressed(ImGuiKey_Q)) {
        ActionShutdown();
    }
    if (g_config.display.debug.enable_monitor_console &&
        ImGui::IsKeyPressed(ImGuiKey_GraveAccent)) {
        monitor_window.ToggleOpen();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F12)) {
        ActionScreenshot();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F11)) {
        gwemu_toggle_fullscreen();
    }
    // Tab toggles the main menu bar -- picked since backtick/F-keys above
    // are already bound (monitor console, screenshot, fullscreen) and Tab
    // is a common show/hide-HUD convention across other emulators. Without
    // this there was previously no way to dismiss the menu bar at all.
    if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
        g_config.display.ui.show_menubar = !g_config.display.ui.show_menubar;
        gwemu_settings_save();
    }
}

void ShowMainMenu()
{
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Pause/Resume")) {
                ActionTogglePause();
            }
            if (ImGui::MenuItem("Reset")) {
                ActionReset();
            }
            if (ImGui::MenuItem("Screenshot")) {
                ActionScreenshot();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) {
                ActionShutdown();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Display")) {
            // Quick-access mirror of Settings > Display -- same
            // g_config.display.ui.* state, just reachable without opening
            // the full Settings window. Window Scale (resolution x integer
            // multiple) is the primary control and comes first; Display
            // mode is the non-integer-window-size fallback, demoted below
            // a separator -- same ordering rationale as display-view.cc.
            int tw = 0, th = 0;
            bool have_size = gwemu_hud_get_framebuffer_size(&tw, &th);
            for (int mult = 1; mult <= 4; mult++) {
                char label[8];
                snprintf(label, sizeof(label), "%dx", mult);
                if (!have_size) ImGui::BeginDisabled();
                if (ImGui::MenuItem(label)) {
                    /* tw/th are guest-texture PIXELS; SetWindowSize takes
                     * POINTS -- snap so points*scale is integral, keeping
                     * fractional-scale Wayland presentation 1:1 (sharp) at
                     * the nearest size to N x native pixels. */
                    int pw, ph;
                    gwemu_snap_window_points(gwemu_get_window(), tw * mult,
                                             th * mult, &pw, &ph);
                    SDL_SetWindowSize(gwemu_get_window(), pw, ph);
                }
                if (!have_size) ImGui::EndDisabled();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Fullscreen", "F11", gwemu_is_fullscreen())) {
                gwemu_toggle_fullscreen();
            }
            ImGui::Separator();
            int fit_idx = (int)g_config.display.ui.fit;
            bool center = fit_idx == CONFIG_DISPLAY_UI_FIT_CENTER;
            bool scale_ar = fit_idx == CONFIG_DISPLAY_UI_FIT_SCALE;
            bool stretch = fit_idx == CONFIG_DISPLAY_UI_FIT_STRETCH;
            if (ImGui::BeginMenu("Content Fit (non-integer sizes)")) {
                if (ImGui::MenuItem("Center", NULL, center)) {
                    g_config.display.ui.fit = CONFIG_DISPLAY_UI_FIT_CENTER;
                    gwemu_settings_save();
                }
                if (ImGui::MenuItem("Scale", NULL, scale_ar)) {
                    g_config.display.ui.fit = CONFIG_DISPLAY_UI_FIT_SCALE;
                    gwemu_settings_save();
                }
                if (ImGui::MenuItem("Stretch", NULL, stretch)) {
                    g_config.display.ui.fit = CONFIG_DISPLAY_UI_FIT_STRETCH;
                    gwemu_settings_save();
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Settings")) {
            if (ImGui::MenuItem("System")) {
                g_main_menu.ShowSystem();
                gwemu_settings_hud_show();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About")) {
                g_main_menu.ShowAbout();
                gwemu_settings_hud_show();
            }
            ImGui::EndMenu();
        }

        // Dedicated, always-visible Reset/Power buttons (not buried in a
        // dropdown). Reset calls the same qemu_system_reset_request() every
        // device's reset() fires from -- QEMU's one true full-system reset,
        // equivalent to a real NRST pin, no process restart and no "softer"
        // reset path exists in QEMU core to confuse this with. Power
        // synthesizes a real GNW_BTN_PWR press/release through the normal
        // input path, so hw/misc/gnw_h7b0_gpio.c's/gnw_h7b0_pwr.c's real
        // PA0/EXTI0 standby-wake logic handles it exactly as a real
        // physical press, not a GUI-level shortcut.
        float btn_w = ImGui::CalcTextSize("Power").x + 2 * ImGui::GetStyle().FramePadding.x;
        ImGui::SameLine(ImGui::GetWindowWidth() - 2 * btn_w - 24);
        if (ImGui::Button("Reset")) {
            ActionReset();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Hard reset (NRST)");
        }
        if (ImGui::Button("Power")) {
            gnw_input_synth_press(GNW_INPUT_BTN_PWR);
        }

        g_main_menu_height = ImGui::GetWindowHeight();
        ImGui::EndMainMenuBar();
    } else {
        g_main_menu_height = 0;
    }
}
