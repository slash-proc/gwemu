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
    // Natural content height (ImGui points) measured during the last
    // Draw() -- the host resizes the SDL settings window to this so the
    // wizard never scrolls or clips (see gwemu_settings_hud_update).
    float DesiredHeight() const { return m_desired_h; }
    // True exactly on frames where the layout genuinely changed
    // (accordion toggled, template/validation rows changed) -- the host
    // issues ONE resize request then and otherwise never touches the
    // window (accepting whatever size the WM granted; fighting the
    // compositor per-frame degraded the whole window over time).
    bool TakeContentChanged() { bool c = m_content_changed; m_content_changed = false; return c; }

private:
    enum Template { TplStockMario, TplStockZelda, TplCustom };
    // Two-stage flow (flow spec sect. 2): S1a prompt -> S1b status ->
    // S2 profile form. Re-entry with existing profiles starts at S2.
    enum Stage { StageFwPrompt, StageFwStatus, StageProfile };

    // Custom-template slot choices (content-blind: files are opaque).
    enum Bank1Choice { B1Blank, B1OfwMario, B1OfwZelda, B1File };
    enum Bank2Choice { B2Blank, B2File };
    enum ExtChoice { ExtBlank, ExtOfwMario, ExtOfwZelda, ExtFile };

    void DrawForm();
    void DrawFirmwareStage();
    void DrawFolderUnit();
    void DrawGameCard(int gi, float w);
    void DrawCardTooltip(int gi);
    void DrawFirmwareStrip();
    void PickFolder();
    int CompleteCount() const;
    void EnterProfileStage();
    void DrawBankAssignments();
    void DrawSdSection();
    bool DrawExtSizeStepper(); // returns true when the user changed it
    void DrawBuildView();
    void StartBuild();
    // Single shared predicate: is Zelda content involved in ANY current
    // selection (stock zelda template, zelda assets extflash, zelda OFW
    // bank1 -- in any template state)? Future zelda-touching contexts
    // must extend THIS, so the 4 MiB floor can't miss one.
    bool ZeldaInvolved() const;
    // Lowest legal extflash size for the current selection (Zelda content
    // needs at least 4 MiB -- owner-specified floor).
    int MinExtSizeMiB() const;
    void SyncStockReflection();
    bool BuildWorker(std::string &err);   // runs on m_thread
    bool ValidSources(std::string *why) const;

    GnwBackupLibrary m_library;

    int m_template = TplStockMario;
    int m_stage = StageFwPrompt;
    bool m_folder_chosen = false;
    bool m_focus_continue = false;         // one-shot keyboard focus
    // Card illumination: one-shot 250ms ease-in on completion (never
    // looping) -- previous shown state + transition timestamp per card.
    int m_card_prev_state[2] = { -1, -1 };
    uint64_t m_card_lit_ns[2] = { 0, 0 };
    bool m_s2_visited = false;             // template default applied once
    bool m_open_assignments_next = false;  // one-shot accordion auto-open
    bool m_close_assignments_next = false; // one-shot collapse (stock selected)
    char m_name[64] = "";
    std::string m_name_hint;      // generated placeholder

    // Unified "Patched" concept (one checkbox, lives next to Bank 1's
    // OFW selection; stock templates share it)
    bool m_patched = true;   // Patched is the assumed case (owner call)

    // Custom options
    int m_bank1_choice = B1OfwMario;
    std::string m_bank1_path;
    int m_bank2_choice = B2Blank;
    std::string m_bank2_path;
    int m_ext_choice = ExtBlank;
    std::string m_ext_path;
    int m_ext_size_mib = 64;

    // SD Card section (see DrawSdSection). SD state deliberately never
    // resets on template selection and never flips the template to
    // Custom -- it's orthogonal to the flash-slot template concept.
    enum SdMode { SdNone, SdNew, SdImport, SdShared };
    int m_sd_mode = SdNone;
    std::string m_sd_import_path;
    int m_sd_transfer = 0;          // 0=Copy 1=Move original
    int m_sd_storage = 0;           // 0=Bundled 1=Shared
    int m_sd_new_size_gib = 8;      // power-of-2, 4..32 (create gated off)
    std::string m_sd_shared_image;  // filename under sd-cards/
    bool m_sd_open = false;
    bool m_close_sd_next = false;   // one-shot collapse (stock selected)
    // 1s-TTL qcow2-magic check of m_sd_import_path (same pattern as
    // ResolvePatchBinary -- never per-frame file I/O). 0=ok 1=no file
    // 2=missing 3=not qcow2.
    int CheckSdImport() const;
    mutable std::string m_sd_chk_path;
    mutable int m_sd_chk_result = 1;
    mutable uint64_t m_sd_chk_ms = 0;

    // Build state (worker thread; UI polls)
    enum BuildState { BuildIdle, BuildRunning, BuildDone, BuildFailed };
    std::atomic<int> m_build_state{BuildIdle};
    std::atomic<int> m_build_step{0};
    std::string m_build_error;    // written before state flip
    std::string m_created_id;     // set by worker on success
    bool m_completed = false;
    float m_desired_h = 0.0f;
    bool m_content_changed = false;
    unsigned m_last_sig = ~0u;
    bool m_assignments_open = false;
    // Cached existence of the gnwmanager patch binary (per game),
    // rechecked at most once a second -- g_file_test ran every frame
    // from ValidSources()/DrawForm() before.
    mutable std::string m_patchbin_path[2];
    mutable uint64_t m_patchbin_check_ms[2] = { 0, 0 };
    // Resolution order: ../gnwmanager checkout, then <appdata>/cache.
    // Empty string = not available locally (a download may provide it).
    const std::string &ResolvePatchBinary(int gi) const;

    // Lazy per-game download of gnwmanager's patch binary (worker
    // thread; UI polls state). 0=idle 1=running 2=done 3=failed.
    std::atomic<int> m_dl_state[2] = { 0, 0 };
    std::string m_dl_error[2];       // written before the state flip
    std::thread m_dl_threads[2];
    void StartPatchDownload(int gi);
    int Bank1Game() const;           // 0/1 for OFW bank1 choice, -1 otherwise
    std::thread m_thread;
    void JoinWorker();
};

extern ProfileWizard g_profile_wizard;
