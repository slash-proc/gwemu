//
// gnw-h7b0 User Interface -- minimal main menu scene (Phase 1)
//
#include "common.hh"
#include "main-menu.hh"
#include "flash-storage-view.hh"
#include "sdcard-view.hh"
#include "snapshot-manager.hh"
#include "widgets.hh"
#include "monitor.hh"
#include "../gwemu-gnw-input.h"
extern "C" {
#include "qemu-version.h"
}

void MainMenuGeneralView::Draw()
{
    ImGui::TextWrapped("General settings placeholder.");
}

void MainMenuInputView::Draw()
{
    ImGui::TextWrapped(
        "Click Keyboard/Gamepad next to a button, then press the physical "
        "key or gamepad button to bind it. Rebindings take effect "
        "immediately and are saved automatically.");
    ImGui::Spacing();

    if (gnw_input_is_rebinding()) {
        ImGui::TextColored(ImVec4(1, 1, 0, 1),
                            "Waiting for input... (Esc to cancel)");
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            gnw_input_cancel_rebind();
        }
    }

    if (ImGui::Button("Reset to defaults")) {
        gnw_input_reset_defaults();
        gnw_input_save();
    }
    ImGui::Spacing();

    if (ImGui::BeginTable("gnw_input_tbl", 3,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Button");
        ImGui::TableSetupColumn("Keyboard");
        ImGui::TableSetupColumn("Gamepad");
        ImGui::TableHeadersRow();

        for (int i = 0; i < GNW_INPUT_BTN__COUNT; i++) {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(gnw_input_btn_names[i]);

            ImGui::TableSetColumnIndex(1);
            ImGui::PushID(i * 2);
            std::string kb_label = gnw_input_get_binding_name(i, false);
            if (ImGui::Button(kb_label.c_str(), ImVec2(-FLT_MIN, 0))) {
                gnw_input_begin_rebind(i, false);
            }
            ImGui::PopID();

            ImGui::TableSetColumnIndex(2);
            ImGui::PushID(i * 2 + 1);
            std::string pad_label = gnw_input_get_binding_name(i, true);
            if (ImGui::Button(pad_label.c_str(), ImVec2(-FLT_MIN, 0))) {
                gnw_input_begin_rebind(i, true);
            }
            ImGui::PopID();
        }

        ImGui::EndTable();
    }
}

void MainMenuSystemView::Draw()
{
    ImGui::TextWrapped("System settings placeholder.");

    ImGui::Spacing();
    SectionTitle("Debug");
    if (Toggle("Monitor console", &g_config.display.debug.enable_monitor_console,
                "Enable the ` (backtick) hotkey to open the QEMU HMP "
                "monitor console. Off by default.")) {
        gwemu_settings_save();
        if (!g_config.display.debug.enable_monitor_console) {
            monitor_window.is_open = false;
        }
    }
}

void MainMenuAboutView::Draw()
{
    ImGui::Text("gnw-h7b0 (gwemu)");
    ImGui::TextWrapped(
        "A QEMU machine model for the Nintendo Game & Watch's "
        "STM32H7B0 SoC.");
    ImGui::Spacing();
    ImGui::Text("Version: %s", QEMU_FULL_VERSION);
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Peripheral coverage: see docs/peripheral-coverage.md in the "
        "repository for the current per-peripheral real-model/stub/"
        "unmodeled status.");
    ImGui::Spacing();
    ImGui::TextWrapped(
        "gwemu is a hard fork of QEMU, licensed GPLv2 (see COPYING). "
        "Its GUI is ported from xemu (github.com/xemu-project/xemu), "
        "also GPLv2, using Dear ImGui (MIT) and other third-party "
        "components under their own licenses -- see subprojects/ and "
        "ui/thirdparty/ for details.");
}

void MainMenuSnapshotsView::Draw()
{
    g_snapshot_mgr.Draw();
}

bool MainMenuTabButton::Draw(bool selected)
{
    return ImGui::Selectable(m_text.c_str(), selected);
}

MainMenuScene::MainMenuScene()
    : m_current_view_index(0)
{
    m_flash_storage_view = new GnwFlashStorageView();
    m_sdcard_view = new GnwSdCardView();

    m_tabs.push_back(new MainMenuTabButton("General"));
    m_tabs.push_back(new MainMenuTabButton("System"));
    m_tabs.push_back(new MainMenuTabButton("Flash"));
    m_tabs.push_back(new MainMenuTabButton("SD Card"));
    m_tabs.push_back(new MainMenuTabButton("Input"));
    m_tabs.push_back(new MainMenuTabButton("Display"));
    m_tabs.push_back(new MainMenuTabButton("Audio"));
    m_tabs.push_back(new MainMenuTabButton("Snapshots"));
    m_tabs.push_back(new MainMenuTabButton("About"));
    m_views.push_back(&m_general_view);
    m_views.push_back(&m_system_view);
    m_views.push_back(m_flash_storage_view);
    m_views.push_back(m_sdcard_view);
    m_views.push_back(&m_input_view);
    m_views.push_back(&m_display_view);
    m_views.push_back(&m_audio_view);
    m_views.push_back(&m_snapshots_view);
    m_views.push_back(&m_about_view);
}

void MainMenuScene::ShowSettings() { m_current_view_index = 0; Show(); }
void MainMenuScene::ShowSystem() { m_current_view_index = 1; Show(); }
void MainMenuScene::ShowAbout() { m_current_view_index = 7; Show(); }
void MainMenuScene::ShowSnapshots() { m_current_view_index = 6; Show(); }

// gnw_input_process_sdl_event() is called exactly once per event, from
// ui/gwemu.c's poll_events() -- these only report/gate on pending-rebind
// state, they must not also invoke it (see gwemu.c's poll_events comment).
bool MainMenuScene::IsInputRebinding() { return gnw_input_is_rebinding(); }
bool MainMenuScene::ConsumeRebindEvent(SDL_Event *event) { return gnw_input_is_rebinding(); }

void MainMenuScene::Show()
{
    Scene::Show();
}

void MainMenuScene::Hide()
{
    Scene::Hide();
}

bool MainMenuScene::IsAnimating()
{
    return Scene::IsAnimating();
}

bool MainMenuScene::Draw()
{
    ImGuiIO &io = ImGui::GetIO();
    ImVec2 size(500 * g_viewport_mgr.m_scale, 350 * g_viewport_mgr.m_scale);
    ImVec2 pos((io.DisplaySize.x - size.x) / 2,
               (io.DisplaySize.y - size.y) / 2);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);

    bool is_open = true;
    if (!ImGui::Begin("Menu", &is_open,
                       ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return is_open;
    }

    ImGui::BeginChild("##tabs", ImVec2(120 * g_viewport_mgr.m_scale, 0),
                       true);
    for (size_t i = 0; i < m_tabs.size(); i++) {
        if (m_tabs[i]->Draw((int)i == m_current_view_index)) {
            m_current_view_index = (int)i;
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##view");
    if (m_current_view_index >= 0 &&
        m_current_view_index < (int)m_views.size()) {
        m_views[m_current_view_index]->Draw();
    }
    ImGui::EndChild();

    ImGui::End();

    if (!is_open) {
        Hide();
    }
    return is_open;
}

MainMenuScene g_main_menu;
