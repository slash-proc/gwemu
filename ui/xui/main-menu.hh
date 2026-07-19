//
// gnw-h7b0 User Interface -- minimal main menu scene (Phase 1)
//
// Structural reference: xemu's own ui/xui/main-menu.hh (MainMenuTabView
// subclass pattern), not ported wholesale -- its actual tab content
// (Network/Snapshots/Input rebinding UI) is all Xbox-domain-specific.
// This is a genuinely new, minimal placeholder shell: proves the
// Scene/tab-button rendering pipeline works, real G&W-appropriate
// settings content is a later phase.
//
#pragma once
#include <string>
#include <vector>
#include "common.hh"
#include "scene.hh"
#include "viewport-manager.hh"

class MainMenuTabView
{
public:
    virtual ~MainMenuTabView() = default;
    virtual void Draw() = 0;
};

class MainMenuGeneralView : public virtual MainMenuTabView
{
public:
    void Draw() override;
};

class MainMenuSystemView : public virtual MainMenuTabView
{
public:
    void Draw() override;
};

class MainMenuAboutView : public virtual MainMenuTabView
{
public:
    void Draw() override;
};

class MainMenuTabButton
{
protected:
    std::string m_text;

public:
    MainMenuTabButton(std::string text) : m_text(text) {}
    bool Draw(bool selected);
};

class MainMenuScene : public virtual Scene {
protected:
    int m_current_view_index;
    std::vector<MainMenuTabButton*> m_tabs;
    std::vector<MainMenuTabView*>   m_views;
    MainMenuGeneralView             m_general_view;
    MainMenuSystemView              m_system_view;
    MainMenuAboutView               m_about_view;

public:
    MainMenuScene();
    void ShowSettings();
    void ShowSystem();
    void ShowAbout();
    void ShowSnapshots();
    bool IsInputRebinding();
    bool ConsumeRebindEvent(SDL_Event *event);
    void Show() override;
    void Hide() override;
    bool IsAnimating() override;
    bool Draw() override;
};

extern MainMenuScene g_main_menu;
