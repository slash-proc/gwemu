//
// gnw-h7b0 User Interface -- minimal main menu scene (Phase 1)
//
#include "common.hh"
#include "main-menu.hh"

void MainMenuGeneralView::Draw()
{
    ImGui::TextWrapped("General settings placeholder.");
}

void MainMenuSystemView::Draw()
{
    ImGui::TextWrapped("System settings placeholder.");
}

void MainMenuAboutView::Draw()
{
    ImGui::Text("gnw-h7b0");
    ImGui::TextWrapped(
        "A QEMU machine model for the Nintendo Game & Watch's "
        "STM32H7B0 SoC.");
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
    m_tabs.push_back(new MainMenuTabButton("About"));
    m_views.push_back(&m_general_view);
    m_views.push_back(&m_system_view);
    m_views.push_back(&m_about_view);
}

void MainMenuScene::ShowSettings() { m_current_view_index = 0; Show(); }
void MainMenuScene::ShowSystem() { m_current_view_index = 1; Show(); }
void MainMenuScene::ShowAbout() { m_current_view_index = 2; Show(); }
void MainMenuScene::ShowSnapshots() { m_current_view_index = 1; Show(); }

bool MainMenuScene::IsInputRebinding() { return false; }
bool MainMenuScene::ConsumeRebindEvent(SDL_Event *event) { return false; }

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
