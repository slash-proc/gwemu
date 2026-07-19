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
    if (ImGui::IsKeyPressed(ImGuiKey_GraveAccent)) {
        monitor_window.ToggleOpen();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F12)) {
        ActionScreenshot();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F11)) {
        xemu_toggle_fullscreen();
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
        if (ImGui::BeginMenu("Settings")) {
            if (ImGui::MenuItem("System")) {
                g_main_menu.ShowSystem();
                g_scene_mgr.PushScene(g_main_menu);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About")) {
                g_main_menu.ShowAbout();
                g_scene_mgr.PushScene(g_main_menu);
            }
            ImGui::EndMenu();
        }
        g_main_menu_height = ImGui::GetWindowHeight();
        ImGui::EndMainMenuBar();
    } else {
        g_main_menu_height = 0;
    }
}
