//
// gnw-h7b0 User Interface -- device-profile creation wizard.
//
// Replaces the xemu-inherited FirstBootWindow as the first-run
// experience (main.cc opens this when show_welcome is set and no
// profiles exist), and is reopened by the Profiles tab's "New" action.
//
// Three pages: Template (Stock Mario / Stock Zelda / Custom + optional
// name) -> Sources (per-template inputs, all content-blind) -> Build
// (worker thread copies/generates/patches into the new profile dir with
// progress; never blocks the draw handler). See gwemu-profiles.hh for
// the store, flash-backups.hh for the shared stock-dump verification.
//
#pragma once
#include <atomic>
#include <string>
#include <thread>
#include "flash-backups.hh"

class ProfileWizard {
public:
    bool is_open = false;

    ProfileWizard();
    ~ProfileWizard();

    // Reset to page 0 and rescan the backup library, then open.
    void Open();
    void Draw();
    // True once a profile was actually created this session (used by the
    // settings-window host to decide whether closing the wizard should
    // also close the window vs fall back to the settings menu).
    bool WasCompleted() const { return m_completed; }

private:
    enum Template { TplStockMario, TplStockZelda, TplCustom };

    // Custom-template slot choices (content-blind: files are opaque).
    enum Bank1Choice { B1Blank, B1OfwMario, B1OfwZelda, B1File };
    enum Bank2Choice { B2Blank, B2File };
    enum ExtChoice { ExtBlank, ExtOfwMario, ExtOfwZelda, ExtFile };

    void DrawForm();
    void DrawBackupFolderRow();
    void DrawBankAssignments();
    void DrawExtSizeStepper(bool disabled);
    void DrawBuildView();
    void StartBuild();
    // Lowest legal extflash size for the current selection (Zelda content
    // needs at least 4 MiB -- owner-specified floor).
    int MinExtSizeMiB() const;
    void SyncStockReflection();
    bool BuildWorker(std::string &err);   // runs on m_thread
    bool ValidSources(std::string *why) const;

    GnwBackupLibrary m_library;

    int m_template = TplStockMario;
    bool m_open_assignments_next = false; // one-shot accordion auto-open
    char m_name[64] = "";
    std::string m_name_hint;      // generated placeholder

    // Stock options
    bool m_stock_patched = false; // retro-go dual-boot hotkey patch

    // Custom options
    int m_bank1_choice = B1OfwMario;
    std::string m_bank1_path;
    int m_bank2_choice = B2Blank;
    std::string m_bank2_path;
    int m_ext_choice = ExtBlank;
    std::string m_ext_path;
    int m_ext_size_mib = 64;

    // Build state (worker thread; UI polls)
    enum BuildState { BuildIdle, BuildRunning, BuildDone, BuildFailed };
    std::atomic<int> m_build_state{BuildIdle};
    std::atomic<int> m_build_step{0};
    std::string m_build_error;    // written before state flip
    std::string m_created_id;     // set by worker on success
    bool m_completed = false;
    std::thread m_thread;
    void JoinWorker();
};

extern ProfileWizard g_profile_wizard;
