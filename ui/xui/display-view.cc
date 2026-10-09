//
// gnw-h7b0 User Interface -- Display settings tab
//
#include "common.hh"
#include "main-menu.hh"
#include "widgets.hh"
#include "../gwemu-settings.h"
#include "gwemu-hud.h"
#include "gl-helpers.hh"

void MainMenuDisplayView::Draw()
{
    SectionTitle("Window");
    if (Toggle("Fullscreen on startup", &g_config.display.window.fullscreen_on_startup,
                "Start in fullscreen instead of windowed")) {
        gwemu_settings_save();
    }
    if (Toggle("VSync", &g_config.display.window.vsync,
                "Sync frame presentation to the display's refresh rate "
                "(applied on next launch)")) {
        gwemu_settings_save();
    }
    if (Toggle("Show menu bar", &g_config.display.ui.show_menubar,
                "Show the File/Settings/Help menu bar (Tab also toggles "
                "this at any time)")) {
        gwemu_settings_save();
    }
    int menu_behavior = (int)g_config.display.ui.menubar_behavior;
    if (ChevronCombo("Menu bar behavior", &menu_behavior,
                     "Auto-hide after inactivity\0"
                     "Keep fixed above the display\0",
                     "Choose whether the menu overlays the display or reserves space above it")) {
        g_config.display.ui.menubar_behavior =
            (CONFIG_DISPLAY_UI_MENUBAR_BEHAVIOR)menu_behavior;
        gwemu_settings_save();
    }
    int bezel = (int)g_config.display.ui.bezel;
    if (ChevronCombo("Device bezel", &bezel,
                     "None\0Mario\0Zelda\0",
                     "Show the virtual screen inside device artwork when it fits")) {
        int display_scale = gwemu_hud_get_current_display_scale();
        g_config.display.ui.bezel = (CONFIG_DISPLAY_UI_BEZEL)bezel;
        gwemu_settings_save();
        gwemu_hud_resize_for_bezel(display_scale);
    }

    // Primary control: the window follows the emulated screen's real
    // resolution x an integer scale -- this is the main, expected way to
    // size the window, not an alternative sitting next to "Display mode"
    // below (that ordering was tried twice and read as backwards -- the
    // window's size should be driven by game resolution, not the other
    // way around).
    SectionTitle("Window Scale");
    ImGui::TextWrapped("Resize the window to an exact multiple of the "
                        "emulated screen's native resolution:");
    int tw = 0, th = 0;
    bool have_size = gwemu_hud_get_framebuffer_size(&tw, &th);
    for (int mult = 1; mult <= 4; mult++) {
        if (mult > 1) ImGui::SameLine();
        char label[8];
        snprintf(label, sizeof(label), "%dx", mult);
        ImGui::BeginDisabled(!have_size);
        if (ImGui::Button(label)) {
            /* tw/th are guest-texture PIXELS; SetWindowSize takes POINTS --
             * snap so points*scale is integral, keeping fractional-scale
             * Wayland presentation 1:1 (sharp) at the nearest size to
             * N x native pixels. */
            int target_w, target_h;
            gwemu_hud_get_display_window_size(mult, &target_w, &target_h);
            gwemu_hud_set_window_aspect_ratio_for_size(target_w, target_h);
            gwemu_set_window_size_pixels(gwemu_get_window(), target_w,
                                         target_h);
        }
        ImGui::EndDisabled();
    }
    if (have_size) {
        ImGui::Text("Native size: %dx%d", tw, th);
    } else {
        ImGui::TextDisabled("(native size not available yet)");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Unrelated to the above: this is the ImGui interface's own widget/
    // font scale (menu text and button size), not game content scale --
    // kept clearly separate so the two aren't confused for each other.
    SectionTitle("Interface Scale (menu text/buttons, not the game screen)");
    if (Toggle("Auto-scale UI", &g_config.display.ui.auto_scale,
                "Automatically scale the menu/HUD to match the display's "
                "DPI")) {
        gwemu_settings_save();
    }
    if (!g_config.display.ui.auto_scale) {
        if (ImGui::SliderFloat("Manual scale", &g_config.display.ui.scale, 0.5f, 3.0f)) {
            gwemu_settings_save();
        }
    }
}
