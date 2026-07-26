//
// gnw-h7b0 User Interface -- SD Card tab
//
// One job: turn a folder into an SD card image whose root IS that folder's
// contents. The overwhelmingly common case is retro-go's sd_content/.
//
// Everything runs in-process: gwemu_sdcreate_qcow2_progress() (ui/gwemu-
// sdcreate.c) creates the qcow2 through QEMU's own block layer and formats
// it MBR+FAT32 via contrib/gnw-tools' gnw_sdimg builder. No Python, no
// popen(), and therefore no dependence on the process working directory --
// the old scripts/make_sdcard_image.py shell-out was broken for any
// installed/portable build.
//
// Async discipline (a real bug once, see CLAUDE.md): BOTH slow operations
// -- scanning the content folder and building the image -- run on worker
// threads. Draw() only ever reads atomics plus a mutex-guarded string, and
// joins a worker after it has already flipped its state, so the join is
// instant. Nothing here may block the render thread.
//
// Output lands in the shared-SD registry (<app data>/sd-cards/<name>.qcow2
// + a shareable=true .toml sidecar), which is exactly the shape the profile
// wizard's "Shared (other profiles can attach)" SD option enumerates -- so
// a card built here shows up as attachable in every profile. See
// ui/gwemu-profiles.hh.
//
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
// Deliberately does NOT #include "main-menu.hh" -- that header includes
// this one (after defining MainMenuTabView), so self-including here would
// create a cycle. MainMenuTabView must already be visible wherever this
// header is included from.

class GnwSdCardView : public virtual MainMenuTabView
{
protected:
    enum class JobState { Idle, Running, Done, Failed };

    // ---- content folder + its scan ------------------------------------
    std::string m_content_dir;
    char m_name_buf[128] = "";

    std::atomic<JobState> m_scan_state{ JobState::Idle };
    std::atomic<uint64_t> m_scan_bytes{ 0 };   // raw file bytes
    std::atomic<uint64_t> m_scan_files{ 0 };
    std::atomic<uint64_t> m_scan_dirs{ 0 };
    std::atomic<bool> m_scan_cancel{ false };
    std::mutex m_scan_mutex;
    std::string m_scan_error;                  // guarded by m_scan_mutex
    std::thread m_scan_worker;

    // ---- card size ----------------------------------------------------
    int m_size_choice = 0;

    // ---- build --------------------------------------------------------
    std::atomic<JobState> m_build_state{ JobState::Idle };
    std::atomic<uint64_t> m_build_bytes{ 0 };  // bytes written so far
    std::atomic<uint64_t> m_build_expect{ 0 }; // denominator for the bar
    std::mutex m_build_mutex;
    std::string m_build_message;               // guarded by m_build_mutex
    std::string m_build_path;                  // guarded by m_build_mutex
    std::string m_build_image;                 // registry filename, ditto
    std::thread m_build_worker;

    bool m_attach_to_active = false;

    void SetContentDir(const char *path);
    void StartScan();
    void StartBuild();
    void JoinFinishedWorkers();

    // Everything the fit check needs, derived from the scan + size choice.
    struct FitInfo {
        uint64_t card_bytes;
        uint64_t usable_bytes;
        uint64_t needed_bytes;
        bool known;    // a completed scan backs these numbers
        bool fits;
    };
    FitInfo ComputeFit() const;

    void DrawFolderCard();
    void DrawSizeRow(const FitInfo &fit);
    void DrawBuildRow(const FitInfo &fit);

public:
    ~GnwSdCardView();
    void Draw() override;
};
