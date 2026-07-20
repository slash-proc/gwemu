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

class GnwFlashStorageView; // flash-storage-view.hh, forward-declared to
                            // avoid a circular include (it includes this
                            // header for MainMenuTabView)
class GnwSdCardView;       // sdcard-view.hh, same reason

class MainMenuTabView
{
public:
    virtual ~MainMenuTabView() = default;
    virtual void Draw() = 0;
};

// display-view.hh/audio-view.hh inherit from MainMenuTabView above, so they
// must be included after its definition, not before (same circular-include
// hazard as GnwFlashStorageView, just resolved with include-order instead
// of a forward-declared pointer since these two are small/self-contained).
#include "display-view.hh"
#include "audio-view.hh"

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
    MainMenuInputView               m_input_view;
    MainMenuDisplayView             m_display_view;
    MainMenuAudioView               m_audio_view;
    MainMenuAboutView               m_about_view;
    MainMenuSnapshotsView            m_snapshots_view;
    GnwFlashStorageView            *m_flash_storage_view; // flash-storage-view.hh
    GnwSdCardView                  *m_sdcard_view;         // sdcard-view.hh

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
