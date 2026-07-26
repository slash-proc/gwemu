//
// gnw-h7b0 User Interface -- minimal main menu scene (Phase 1)
//
#include "common.hh"
#include "main-menu.hh"
#include "../gwemu-profiles.hh"
#include "profile-wizard.hh"
#include "snapshot-manager.hh"
#include "font-manager.hh"
#include "widgets.hh"
#include "monitor.hh"
#include "gdb-view.hh"
#include "../gwemu-gnw-input.h"
#include "gwemu-hud.h"
extern "C" {
#include "qemu-version.h"
void gnw_h7b0_rtc_set_sync_host(bool sync_host);
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
    SectionTitle("Time & Clock");
    if (Toggle("Sync RTC to Host Time", &g_config.sys.rtc_sync_host,
                "When enabled, the RTC becomes read-only to perfectly mirror "
                "the host PC's time (prevents stock firmware from resetting it "
                "to 12:00 on boot). Disable this to manually set the time "
                "using the Game & Watch UI.")) {
        gwemu_settings_save();
        gnw_h7b0_rtc_set_sync_host(g_config.sys.rtc_sync_host);
    }

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

    ImGui::Spacing();
    DrawGdbSettings();
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

MainMenuProfilesView::MainMenuProfilesView()
{
}

void MainMenuProfilesView::Draw()
{
    float pad = 16.0f * g_viewport_mgr.m_scale;
    
    ImGui::Columns(2, "ProfilesColumns", false);
    ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.35f);
    
    // LEFT PANE: List of profiles
    ImGui::BeginChild("ProfileListPane", ImVec2(0, 0), true);
    
    const auto &profiles = g_profile_store.Profiles();
    for (const auto &p : profiles) {
        std::string label = p.display_name;
        if (p.id == g_config.general.active_profile) {
            label += " (Active)";
        }
        
        if (ImGui::Selectable(label.c_str(), m_selected_profile_id == p.id)) {
            m_selected_profile_id = p.id;
        }
    }
    
    ImGui::Dummy(ImVec2(0, pad));
    if (ImGui::Button("+ Create New Profile", ImVec2(-1, 0))) {
        g_profile_wizard.Open();
    }
    
    ImGui::EndChild();
    
    // RIGHT PANE: Details & Actions
    ImGui::NextColumn();
    
    ImGui::BeginChild("ProfileDetailPane", ImVec2(0, 0), true);
    if (!m_selected_profile_id.empty()) {
        GwProfile *p = g_profile_store.Find(m_selected_profile_id);
        if (p) {
            ImGui::PushFont(g_font_mgr.m_menu_font_medium);
            ImGui::TextUnformatted(p->display_name.c_str());
            ImGui::PopFont();
            ImGui::Spacing();
            
            // Read-only info
            ImGui::Text("Created: %s", p->created.c_str());
            ImGui::Text("Storage: %.1f MiB", p->disk_bytes / 1048576.0f);
            if (p->sd.mode != GwSdMode::None) {
                ImGui::Text("SD Card: %s", p->sd.mode == GwSdMode::Bundled ? "Bundled" : "Shared");
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            
            // Action Row
            if (ImGui::Button("Launch", ImVec2(120 * g_viewport_mgr.m_scale, 40 * g_viewport_mgr.m_scale))) {
                gwemu_settings_set_string(&g_config.general.active_profile, p->id.c_str());
                gwemu_settings_save();
                std::string sdp = p->SdPath();
                gwemu_relaunch_with_flash_images(p->Bank1Path().c_str(),
                                                 p->Bank2Path().c_str(),
                                                 p->ExtflashPath().c_str(),
                                                 sdp.empty() ? NULL : sdp.c_str());
            }
            ImGui::SameLine();
            if (ImGui::Button("Modify", ImVec2(0, 40 * g_viewport_mgr.m_scale))) {
                g_profile_wizard.OpenForEdit(p->id);
            }
            ImGui::SameLine();
            if (ImGui::Button("Duplicate", ImVec2(0, 40 * g_viewport_mgr.m_scale))) {
                std::string err;
                std::string new_id = g_profile_store.Duplicate(p->id, err);
                if (!new_id.empty()) {
                    m_selected_profile_id = new_id;
                } else {
                    fprintf(stderr, "Duplicate failed: %s\n", err.c_str());
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Delete", ImVec2(0, 40 * g_viewport_mgr.m_scale))) {
                ImGui::OpenPopup("Delete Profile?");
            }
            
            if (ImGui::BeginPopupModal("Delete Profile?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("Are you sure you want to delete profile '%s'?", p->display_name.c_str());
                ImGui::Separator();
                if (ImGui::Button("Yes, Delete", ImVec2(120, 0))) {
                    std::string err;
                    if (g_profile_store.Delete(p->id, err)) {
                        m_selected_profile_id.clear();
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SetItemDefaultFocus();
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        } else {
            m_selected_profile_id.clear();
        }
    } else {
        ImGui::TextDisabled("Select a profile from the left.");
    }
    ImGui::EndChild();
    
    ImGui::Columns(1);
}

float MainMenuProfilesView::DesiredWidth()
{
    float pad = 20 * g_viewport_mgr.m_scale;
    float max_left = 300 * g_viewport_mgr.m_scale;
    ImGui::PushFont(g_font_mgr.m_menu_font);
    for (const auto& p : g_profile_store.Profiles()) {
        max_left = std::max(max_left, ImGui::CalcTextSize(p.display_name.c_str()).x + pad * 2);
    }
    ImGui::PopFont();
    
    float max_right = 450 * g_viewport_mgr.m_scale; // action buttons inline space
    if (!m_selected_profile_id.empty()) {
        GwProfile* p = g_profile_store.Find(m_selected_profile_id);
        if (p) {
            ImGui::PushFont(g_font_mgr.m_menu_font_medium);
            max_right = std::max(max_right, ImGui::CalcTextSize(p->display_name.c_str()).x + pad * 2);
            ImGui::PopFont();
        }
    }
    return max_left + max_right;
}

float MainMenuProfilesView::DesiredHeight()
{
    return 650.0f * g_viewport_mgr.m_scale;
}

bool MainMenuTabButton::Draw(bool selected)
{
    return ImGui::Selectable(m_text.c_str(), selected);
}

MainMenuScene::MainMenuScene()
    : m_current_view_index(0)
{
    m_tabs.push_back(new MainMenuTabButton("General"));
    m_tabs.push_back(new MainMenuTabButton("System"));
    m_tabs.push_back(new MainMenuTabButton("Profiles"));
    m_tabs.push_back(new MainMenuTabButton("SD Card"));
    m_tabs.push_back(new MainMenuTabButton("Input"));
    m_tabs.push_back(new MainMenuTabButton("Display"));
    m_tabs.push_back(new MainMenuTabButton("Audio"));
    m_tabs.push_back(new MainMenuTabButton("Snapshots"));
    m_tabs.push_back(new MainMenuTabButton("About"));
    
    m_views.push_back(&m_general_view);
    m_views.push_back(&m_system_view);
    m_views.push_back(&m_profiles_view);
    m_views.push_back(&m_sdcard_view);
    m_views.push_back(&m_input_view);
    m_views.push_back(&m_display_view);
    m_views.push_back(&m_audio_view);
    m_views.push_back(&m_snapshots_view);
    m_views.push_back(&m_about_view);
}

void MainMenuScene::ShowSettings() { m_current_view_index = 0; Show(); }
void MainMenuScene::ShowSystem() { m_current_view_index = 1; Show(); }
void MainMenuScene::ShowAbout() { m_current_view_index = 8; Show(); }
void MainMenuScene::ShowSnapshots() { m_current_view_index = 7; Show(); }

// gnw_input_process_sdl_event() is called exactly once per event, from
// ui/gwemu.c's poll_events() -- these only report/gate on pending-rebind
// state, they must not also invoke it (see gwemu.c's poll_events comment).
bool MainMenuScene::IsInputRebinding() { return gnw_input_is_rebinding(); }
bool MainMenuScene::ConsumeRebindEvent(SDL_Event *event) { return gnw_input_is_rebinding(); }

void MainMenuScene::Show()
{
    m_content_changed_flag = true;
    Scene::Show();
}

float MainMenuScene::DesiredWidth()
{
    float tabs_w = 120.0f * g_viewport_mgr.m_scale;
    float view_w = 800.0f * g_viewport_mgr.m_scale;
    if (m_current_view_index >= 0 && m_current_view_index < (int)m_views.size()) {
        view_w = m_views[m_current_view_index]->DesiredWidth();
    }
    return tabs_w + view_w + 32.0f * g_viewport_mgr.m_scale; // window padding
}

float MainMenuScene::DesiredHeight()
{
    float view_h = 600.0f * g_viewport_mgr.m_scale;
    if (m_current_view_index >= 0 && m_current_view_index < (int)m_views.size()) {
        view_h = m_views[m_current_view_index]->DesiredHeight();
    }
    return view_h + 32.0f * g_viewport_mgr.m_scale; // window padding
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
    ImVec2 size = io.DisplaySize;
    ImVec2 pos(0, 0);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);

    bool is_open = true;
    if (!ImGui::Begin("Menu", &is_open,
                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::End();
        return is_open;
    }

    ImGui::BeginChild("##tabs", ImVec2(120 * g_viewport_mgr.m_scale, 0),
                       true);
    for (size_t i = 0; i < m_tabs.size(); i++) {
        if (m_tabs[i]->Draw((int)i == m_current_view_index)) {
            if (m_current_view_index != (int)i) {
                m_content_changed_flag = true;
            }
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
