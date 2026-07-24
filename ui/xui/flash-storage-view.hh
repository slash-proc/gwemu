//
// gnw-h7b0 User Interface -- Flash tab (Phase 2, presets iteration)
//
// Backup-folder SHA1 detection drives a Presets dropdown (Stock Mario/
// Stock Zelda/Mario+Retro-Go/Zelda+Retro-Go/Custom); Custom exposes the
// same unified bank1/bank2 widget and geometry-bar extflash editor as
// before. SD Card moved out to its own tab (sdcard-view.hh) -- storage for
// homebrew content is a conceptually separate pipeline from internal/
// external flash and cramming both into one tab read as confusing.
//
// Bank1/Bank2 share ONE widget (DrawBankSlot(), driven by GnwBankSlot) --
// previously bank1 had a rich SHA1-verified dropdown + Patch/Bootloader
// checkboxes while bank2 was a bare file-picker, which read as inconsistent.
// Both banks now use the identical dropdown-of-known-games-plus-Browse
// selector; the only real asymmetry left is that only bank1 exposes Patch/
// Bootloader, because gnw-make-cfw-images's patch pipeline is
// architecturally a bank1 concept (it patches the OFW a dual-boot
// bootloader then jumps *from*; there is no "patched OFW for bank2" concept
// in this project's backend). That asymmetry is content-driven and
// explained inline, not a leftover structural difference.
//
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include "main-menu.hh"
#include "flash-backups.hh"

struct GnwExtflashBlobRow {
    std::string path;
    uint32_t offset = 0;
    uint32_t size = 0;    // cached for geometry-bar layout; 0 until resolved
    bool implicit = false; // the OFW-at-0 row, not user-removable
};

// One bank's selection state -- shared by both Bank1 and Bank2's UI.
// choice: 0=None, 1..N=index into the verified-games list (Mario/Zelda,
// in that fixed order), N+1=Browse (custom_path holds the picked file).
struct GnwBankSlot {
    int choice = 0;
    std::string custom_path;
    bool patch = false;      // only meaningful/drawn when allow_patch=true
    bool bootloader = true;  // only meaningful/drawn when patch is also on
};

// Preset index -- see class comment / Draw() for what each one sets.
enum GnwFlashPreset {
    kPresetStockMario = 0,
    kPresetStockZelda,
    kPresetMarioRetroGo,
    kPresetZeldaRetroGo,
    kPresetCustom,
};

class GnwFlashStorageView : public virtual MainMenuTabView
{
protected:
    std::string m_backup_dir;
    GnwBackupLibrary m_library;      // shared scanner, see flash-backups.hh
    GnwBackupGameStatus m_status[2]; // 0=mario, 1=zelda (mirror of m_library)

    int m_preset = kPresetCustom;
    GnwBankSlot m_bank[2]; // 0=bank1 (allow_patch=true), 1=bank2 (allow_patch=false)
    bool m_ofw_at_zero = false; // explicit toggle, see class comment

    // Extflash size: preset index into kExtSizeMiB (see .cc), or -1 for the
    // "Custom" numeric override in m_ext_size_custom_mib.
    int m_ext_size_choice = 6; // default 64 MiB, see kExtSizeMiB
    int m_ext_size_custom_mib = 64;

    std::vector<GnwExtflashBlobRow> m_extflash_rows;

    // Scratch state for the "Add blob" modal (see DrawAddBlobModal()).
    std::string m_modal_path;
    int m_modal_offset_mib = 0;

    std::string m_status_msg;
    bool m_status_is_error = false;

    // Real patched-extflash byte size per game, computed eagerly (not
    // deferred to Apply) the first time it's needed -- see
    // GetOrComputePatchedExtflashSize()/SyncImplicitExtflashRow(). 0 means
    // "not computed yet", not a real possible output size.
    //
    // Computed on a background thread (gnw-make-cfw-images via popen() was
    // previously called inline from Draw()'s call chain, blocking the
    // render thread for its runtime -- short for a single patch op, but
    // the same freeze-prone pattern as the SD card build, fixed the same
    // way for consistency). m_size_threads[i] is joined either on the next
    // GetOrComputePatchedExtflashSize() call for that game or in the
    // destructor; m_size_compute_running[i] gates against starting a
    // second concurrent computation for the same game.
    std::atomic<uint32_t> m_patched_size_cache[2] = { 0, 0 };
    std::atomic<bool> m_size_compute_running[2] = { false, false };
    std::thread m_size_threads[2];

    void RescanBackupDir();
    void ApplyPreset(int preset);
    void SyncImplicitExtflashRow();
    void ResolveExtflashRowSizes();
    uint32_t GetOrComputePatchedExtflashSize(const char *game);
    uint32_t ExtflashSizeBytes() const;
    bool ApplyBank1(std::string *err);

    // Shared by both banks -- see class comment. allow_patch gates whether
    // the Patch/Bootloader checkboxes are drawn at all (bank1 only).
    void DrawBankSlot(const char *label, int bank_idx, bool allow_patch);
    void DrawExtflashEditor();
    void DrawExtflashGeometryBar();
    void DrawAddBlobModal();

    // Real bank1/bank2/extflash file paths the machine should boot with,
    // given the current preset/selections -- computed AFTER ApplyBank1()
    // has actually built the files (matches scripts/boot_qemu.sh's naming:
    // <game>-bank1[-patched].bin, <game>-bank2.bin, <game>-extflash[-patched].bin).
    // Bank2 may be the raw browsed/retro-go path directly (no copy needed,
    // -global can point at any file).
    void GetFinalImagePaths(std::string *bank1, std::string *bank2,
                             std::string *extflash) const;
    bool m_show_apply_confirm = false;

public:
    GnwFlashStorageView();
    ~GnwFlashStorageView();
    void Draw() override;
};
