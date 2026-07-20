//
// gnw-h7b0 User Interface -- SD Card tab (Phase 2)
//
// Split out of the former combined "Flash/Storage" tab into its own tab --
// SD card content (retro-go/homebrew) is a conceptually separate pipeline
// from internal/external flash, and cramming both into one tab read as
// confusing. Content unchanged from the prior combined tab: a content
// folder, a size preset, and a build button that shells out to
// scripts/make_sdcard_image.py (native C FAT32 writer is real, separate
// follow-up work -- see that script's own header comment).
//
// Build() runs on a background thread (a multi-GB image write is minutes
// of I/O -- doing it inline on the render thread froze the whole app, a
// real bug hit by the user). m_state/m_output are the only fields the
// worker thread touches; guarded by m_mutex since Draw() reads them every
// frame from the render thread.
//
#pragma once
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include "main-menu.hh"

class GnwSdCardView : public virtual MainMenuTabView
{
protected:
    enum class BuildState { Idle, Running, Done, Failed };

    std::string m_content_dir;
    int m_size_choice = 0; // 0=8G,1=16G,2=32G

    std::atomic<BuildState> m_state{ BuildState::Idle };
    std::mutex m_output_mutex;
    std::string m_output; // guarded by m_output_mutex
    std::thread m_worker;

    void StartBuild();
    void JoinWorkerIfDone();

public:
    ~GnwSdCardView();
    void Draw() override;
};
