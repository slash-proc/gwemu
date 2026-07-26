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
    virtual float DesiredWidth() { return 800.0f * g_viewport_mgr.m_scale; }
    virtual float DesiredHeight() { return 600.0f * g_viewport_mgr.m_scale; }
};

// display-view.hh/audio-view.hh inherit from MainMenuTabView above, so they
// must be included after its definition, not before.
#include "display-view.hh"
#include "audio-view.hh"
#include "sdcard-view.hh"

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

class MainMenuProfilesView : public virtual MainMenuTabView
{
public:
    MainMenuProfilesView();
    void Draw() override;
    float DesiredWidth() override;
    float DesiredHeight() override;
private:
    std::string m_selected_profile_id;
};

class MainMenuInputView : public virtual MainMenuTabView
{
public:
    void Draw() override;
};

class MainMenuAboutView : public virtual MainMenuTabView
{
public:
    void Draw() override;
};

class MainMenuSnapshotsView : public virtual MainMenuTabView
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
    MainMenuProfilesView            m_profiles_view;
    GnwSdCardView                   m_sdcard_view;
    MainMenuInputView               m_input_view;
    MainMenuDisplayView             m_display_view;
    MainMenuAudioView               m_audio_view;
    MainMenuAboutView               m_about_view;
    MainMenuSnapshotsView            m_snapshots_view;

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
    
    bool TakeContentChanged() {
        bool res = m_content_changed_flag;
        m_content_changed_flag = false;
        return res;
    }
    
    float DesiredWidth();
    float DesiredHeight();

protected:
    bool m_content_changed_flag = true;
};

extern MainMenuScene g_main_menu;
