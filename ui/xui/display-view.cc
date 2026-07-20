//
// gnw-h7b0 User Interface -- Display settings tab
//
#include "common.hh"
#include "main-menu.hh"
#include "widgets.hh"
#include "../gwemu-settings.h"
#include "gwemu-hud.h"

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
            SDL_SetWindowSize(gwemu_get_window(), tw * mult, th * mult);
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

    /*
     * Edge case, not the primary control: only matters if the window ends
     * up at a size that ISN'T an exact multiple of the native resolution
     * (e.g. manually dragged to an arbitrary size) -- governs how content
     * fits into that mismatched space. g_config.display.ui.fit and the
     * actual scaling math were already ported into gl-helpers.cc's
     * RenderFramebuffer() in Phase 1; this just exposes it, demoted below
     * Window Scale since it's the fallback case, not the main path.
     * Deliberately NOT exposing the ported aspect_ratio enum's 4:3/16:9
     * options as a picker: this board has one fixed real panel aspect
     * ratio, and those two options would just visibly distort it --
     * Native/Auto already produce identical (correct) output here since
     * GetDisplayAspectRatio() falls back to the real texture's own aspect
     * either way.
     */
    SectionTitle("Content Fit (non-integer window sizes)");
    ImGui::TextWrapped("Only matters if the window isn't sized to an exact "
                        "multiple above (e.g. manually resized).");
    int fit_idx = (int)g_config.display.ui.fit;
    if (ChevronCombo("Display mode", &fit_idx,
                     "Center\0"
                     "Scale (preserve aspect)\0"
                     "Stretch\0",
                     "How the emulated screen fits into a mismatched-size window")) {
        g_config.display.ui.fit = (CONFIG_DISPLAY_UI_FIT)fit_idx;
        gwemu_settings_save();
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
