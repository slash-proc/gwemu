//
// gnw-h7b0 User Interface -- device-profile creation wizard, see
// profile-wizard.hh.
//
#include "profile-wizard.hh"
#include "common.hh"
#include "imgui.h"
#include "viewport-manager.hh"
#include "font-manager.hh"
#include "widgets.hh"
#include "misc.hh"
#include "gnw-style-tokens.hh"
#include "gwemu-hud.h"
#include "../gwemu-profiles.hh"
#include "../gwemu-settings.h"
#include "../gwemu-sdcreate.h"
#include "../gwemu-http.h"
#include "hw/misc/gnw_h7b0_nor.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <algorithm>
#include <cstring>
#include <inttypes.h>

extern "C" {
#include "../../contrib/gnw-tools/gnw_cfw_build.h"
}

ProfileWizard g_profile_wizard;

static const uint32_t kBankSize = 0x40000u;      // 256K flash bank

static const GnwNorChip *NorChip(const std::string &key)
{
    const GnwNorChip *chip = gnw_nor_chip_by_key(key.c_str());
    return chip ? chip : gnw_nor_chip_by_key(GNW_NOR_DEFAULT_CHIP_KEY);
}

static const std::vector<const GnwNorChip *> &NorChipChoices()
{
    static const std::vector<const GnwNorChip *> choices = [] {
        std::vector<const GnwNorChip *> result;
        for (unsigned i = 0; i < GNW_NOR_CHIP_COUNT; i++) {
            const GnwNorChip *chip = &gnw_nor_chips[i];
            bool duplicate_model = false;
            for (const GnwNorChip *prior : result) {
                if (prior->size_bytes == chip->size_bytes &&
                    prior->profile == chip->profile &&
                    strcmp(prior->name, chip->name) == 0) {
                    duplicate_model = true;
                    break;
                }
            }
            if (!duplicate_model) {
                result.push_back(chip);
            }
        }
        std::sort(result.begin(), result.end(), [](const GnwNorChip *a,
                                                   const GnwNorChip *b) {
            if (a->size_bytes != b->size_bytes) {
                return a->size_bytes < b->size_bytes;
            }
            int by_name = strcmp(a->name, b->name);
            if (by_name != 0) {
                return by_name < 0;
            }
            return a->jedec[0] != b->jedec[0] ? a->jedec[0] < b->jedec[0] :
                   a->jedec[1] != b->jedec[1] ? a->jedec[1] < b->jedec[1] :
                                                 a->jedec[2] < b->jedec[2];
        });
        return result;
    }();
    return choices;
}

static bool PadFileToFF(const std::string &path, uint64_t capacity,
                        std::string &err)
{
    GStatBuf st;
    if (g_stat(path.c_str(), &st) != 0 || st.st_size < 0) {
        err = "cannot stat " + path;
        return false;
    }
    if ((uint64_t)st.st_size > capacity) {
        err = path + " is larger than the selected NOR chip";
        return false;
    }
    if ((uint64_t)st.st_size == capacity) return true;
    FILE *f = g_fopen(path.c_str(), "ab");
    if (!f) {
        err = "cannot extend " + path;
        return false;
    }
    std::vector<uint8_t> fill(1 << 20, 0xFF);
    uint64_t left = capacity - (uint64_t)st.st_size;
    bool ok = true;
    while (left) {
        size_t n = (size_t)std::min<uint64_t>(left, fill.size());
        if (fwrite(fill.data(), 1, n, f) != n) {
            err = "short 0xFF padding write to " + path;
            ok = false;
            break;
        }
        left -= n;
    }
    if (fclose(f) != 0 && ok) {
        err = "close failed on " + path;
        ok = false;
    }
    return ok;
}

static const SDL_DialogFileFilter kBinFilter[] = {
    { "Firmware images (*.bin)", "bin" },
    { "All files", "*" },
};

static const SDL_DialogFileFilter kQcow2Filter[] = {
    { "SD card images (*.qcow2)", "qcow2" },
    { "All files", "*" },
};

// "New card" creates a fresh MBR+FAT32 qcow2 in-process via
// gwemu_sdcreate_qcow2() (ui/gwemu-sdcreate.c).
static const bool kSdCreateEnabled = true;

// Shareable subset of the shared-SD registry (only shareable=true cards
// are attachable).
static std::vector<const GwSharedSd *> ShareableSds()
{
    std::vector<const GwSharedSd *> v;
    for (const GwSharedSd &s : g_profile_store.SharedSds()) {
        if (s.shareable) {
            v.push_back(&s);
        }
    }
    return v;
}

// gnwmanager's pre-built novel-code binary. Local dev override: a
// checkout next to this repo (same convention the CLI tool documents).
static std::string CheckoutPatchPath(const char *game)
{
    return std::string("../gnwmanager/gnwmanager/cli/gnw_patch/binaries/") +
           game + "/0x08032000.bin";
}

static std::string CachePatchDir(const char *game)
{
    return std::string(gwemu_settings_get_base_path()) + "cache/gnw-patch/" + game;
}

// Raw single-file fetch from BrianPugh/gnwmanager@main -- path verified
// byte-identical (sha1) against a real checkout for both games
// (2026-07-24). NEVER clones/fetches the repo itself (owner rule);
// worst-case fallback if raw URLs ever break: pull the repo zip and
// extract just these blobs.
static std::string PatchBinaryUrl(const char *game)
{
    return std::string("https://raw.githubusercontent.com/BrianPugh/gnwmanager/"
                       "main/gnwmanager/cli/gnw_patch/binaries/") +
           game + "/0x08032000.bin";
}

ProfileWizard::ProfileWizard() = default;

const std::string &ProfileWizard::ResolvePatchBinary(int gi) const
{
    // 1s TTL: g_file_test every frame is exactly the kind of per-frame
    // filesystem work that got purged in batch 3.
    uint64_t now = SDL_GetTicks();
    if (now - m_patchbin_check_ms[gi] > 1000 || m_patchbin_check_ms[gi] == 0) {
        m_patchbin_check_ms[gi] = now;
        const char *game = GnwBackupLibrary::GameName(gi);
        std::string p = CheckoutPatchPath(game);
        if (!g_file_test(p.c_str(), G_FILE_TEST_EXISTS)) {
            p = CachePatchDir(game) + "/0x08032000.bin";
            if (!g_file_test(p.c_str(), G_FILE_TEST_EXISTS)) {
                p.clear();
            }
        }
        m_patchbin_path[gi] = p;
    }
    return m_patchbin_path[gi];
}

int ProfileWizard::Bank1Game() const
{
    if (m_bank1_choice == B1OfwMario) return 0;
    if (m_bank1_choice == B1OfwZelda) return 1;
    return -1;
}

void ProfileWizard::StartPatchDownload(int gi)
{
    int expect = 0;
    if (!m_dl_state[gi].compare_exchange_strong(expect, 1)) {
        return; // already running/done/failed
    }
    if (m_dl_threads[gi].joinable()) {
        m_dl_threads[gi].join();
    }
    m_dl_error[gi].clear();
    m_dl_threads[gi] = std::thread([this, gi]() {
        const char *game = GnwBackupLibrary::GameName(gi);
        std::string dst = CachePatchDir(game) + "/0x08032000.bin";
        std::string url = PatchBinaryUrl(game);
        // gwemu_http_download() is in-process, not a popen("curl ...").
        // On Windows the shell-out both failed outright (POSIX quoting,
        // /dev/null, no wget) and flashed a cmd.exe console window over
        // the GUI -- see the header comment in ui/gwemu-http.c.
        char *err = nullptr;
        bool ok = gwemu_http_download(url.c_str(), dst.c_str(), &err);
        if (!ok) {
            m_dl_error[gi] = std::string("download failed: ") +
                             (err ? err : "unknown error") + "; url: " + url;
        }
        g_free(err);
        m_patchbin_check_ms[gi] = 0; // force re-resolution
        m_dl_state[gi].store(ok ? 2 : 3);
    });
}

ProfileWizard::~ProfileWizard()
{
    JoinWorker();
    for (auto &t : m_dl_threads) {
        if (t.joinable()) {
            t.join();
        }
    }
}

void ProfileWizard::JoinWorker()
{
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void ProfileWizard::Open()
{
    m_height_fit.Reset();
    if (g_config.general.firmware_backup_dir &&
        g_config.general.firmware_backup_dir[0]) {
        m_library.dir = g_config.general.firmware_backup_dir;
        m_folder_chosen = true;
    } else {
        m_library.dir = "backup";
        m_folder_chosen = false;
    }
    m_library.Rescan();
    g_profile_store.Scan();   // shared-SD registry may have changed
    if (m_sd_mode == SdShared) {
        // Auto-fallback: an armed Shared mode whose registry emptied (or
        // whose card vanished) reverts to None on re-open.
        bool found = false;
        for (const GwSharedSd *s : ShareableSds()) {
            found |= s->image == m_sd_shared_image;
        }
        if (!found) {
            m_sd_mode = SdNone;
            m_sd_shared_image.clear();
        }
    }
    SetNorChip(g_config.general.extflash_chip && g_config.general.extflash_chip[0]
                   ? g_config.general.extflash_chip
                   : GNW_NOR_DEFAULT_CHIP_KEY);
    if ((m_template == TplStockMario && !m_library.status[0].Available()) ||
        (m_template == TplStockZelda && !m_library.status[1].Available())) {
        m_template = TplCustom;
        m_open_assignments_next = true;
    } else if (m_template != TplCustom) {
        SyncStockReflection();
    }
    m_build_state.store(BuildIdle);
    m_edit_profile_id.clear();
    m_rdp_locked = false;
    m_created_id.clear();
    m_completed = false;
    m_name_hint = GwProfileStore::GenerateName();
    // Stage entry keys off FIRMWARE status, not profile existence (owner
    // decision: the backup-folder question is the absolute first thing
    // whenever firmware isn't set up yet -- existing profiles don't
    // change that). Any complete game set (or an explicitly chosen
    // folder this session) skips straight to the profile form.
    bool any_complete = false;
    for (int i = 0; i < GnwBackupLibrary::kGameCount; i++) {
        any_complete |= GnwCardStateOf(m_library.status[i]) == kCardComplete;
    }
    if (any_complete) {
        m_s2_visited = false;      // run EnterProfileStage's defaults
        EnterProfileStage();
    } else {
        m_stage = m_folder_chosen ? StageFwStatus : StageFwPrompt;
    }
    m_focus_continue = false;
    m_card_prev_state[0] = m_card_prev_state[1] = -1;
    m_card_lit_ns[0] = m_card_lit_ns[1] = 0;
    is_open = true;
}

void ProfileWizard::OpenForEdit(const std::string &profile_id)
{
    GwProfile *p = g_profile_store.Find(profile_id);
    if (!p) return;

    if (g_config.general.firmware_backup_dir &&
        g_config.general.firmware_backup_dir[0]) {
        m_library.dir = g_config.general.firmware_backup_dir;
    } else {
        m_library.dir = "backup";
    }
    m_library.Rescan();
    g_profile_store.Scan();
    m_stage = StageProfile;
    m_height_fit.Reset();
    m_folder_chosen = false;
    m_completed = false;
    m_edit_profile_id = profile_id;
    m_rdp_locked = p->rdp_locked;
    m_s2_visited = true; // prevent EnterProfileStage defaults from overriding
    
    g_strlcpy(m_name, p->display_name.c_str(), sizeof(m_name));
    m_name_hint = p->display_name;
    
    // We always set Custom template because otherwise we can't represent arbitrary paths
    m_template = TplCustom;
    m_patched = (p->prov_bank1.find("patched-") == 0);
    
    if (p->prov_bank1 == "blank") m_bank1_choice = B1Blank;
    else if (p->prov_bank1 == "ofw-mario") m_bank1_choice = B1OfwMario;
    else if (p->prov_bank1 == "ofw-zelda") m_bank1_choice = B1OfwZelda;
    else { m_bank1_choice = B1File; m_bank1_path = p->Bank1Path(); }
    
    if (p->prov_bank2 == "blank") m_bank2_choice = B2Blank;
    else { m_bank2_choice = B2File; m_bank2_path = p->Bank2Path(); }
    
    if (p->prov_extflash == "blank") m_ext_choice = ExtBlank;
    else if (p->prov_extflash == "ofw-mario") m_ext_choice = ExtOfwMario;
    else if (p->prov_extflash == "ofw-zelda") m_ext_choice = ExtOfwZelda;
    else { m_ext_choice = ExtFile; m_ext_path = p->ExtflashPath(); }
    
    SetNorChip(p->nor_chip);
    
    m_sd_mode = SdNone;
    if (p->sd.mode == GwSdMode::Bundled) {
        m_sd_mode = SdImport;
        m_sd_import_path = p->SdPath();
        m_sd_storage = 0;
        m_sd_transfer = 0;
    } else if (p->sd.mode == GwSdMode::Shared) {
        m_sd_mode = SdShared;
        m_sd_shared_image = p->sd.image;
    }
    
    m_assignments_open = true; // show assignments
    
    is_open = true;
}


/* ------------------------------------------------------------------ */
/* Build worker                                                        */

/*
 * Copy `src` to `dst`, 0xFF-padding out to `pad_to` bytes.
 *
 * Writes to `dst`.tmp and renames into place only on full success: a failed
 * or partial copy must never destroy the image already bound to the profile.
 * The profile store holds copies, not references, so clobbering the
 * destination in place (or unlinking it on error, as this used to do) loses
 * the user's data permanently -- and a missing image is worse than useless,
 * because QEMU's file-backed RAM silently (re)creates it as a 256K hole of
 * zeros, which locks the guest CPU up on the next boot.
 */
static bool copy_padded(const std::string &src, const std::string &dst,
                        uint64_t pad_to, std::string &err)
{
    if (src == dst) {
        // Editing a profile may point back at its current image. Keep it
        // intact, while still enforcing the selected flash capacity and
        // explicit erased-state padding.
        return pad_to ? PadFileToFF(dst, pad_to, err) : true;
    }

    FILE *in = g_fopen(src.c_str(), "rb");
    if (!in) {
        err = "cannot open " + src;
        return false;
    }
    std::string tmp = dst + ".tmp";
    FILE *out = g_fopen(tmp.c_str(), "wb");
    if (!out) {
        fclose(in);
        err = "cannot create " + tmp;
        return false;
    }
    const size_t kChunk = 1 << 20;
    std::vector<uint8_t> buf(kChunk);
    uint64_t written = 0;
    bool ok = true;
    size_t n;
    while ((n = fread(buf.data(), 1, kChunk, in)) > 0) {
        if (fwrite(buf.data(), 1, n, out) != n) {
            err = "short write to " + tmp;
            ok = false;
            break;
        }
        written += n;
    }
    if (ok && ferror(in)) {
        err = "read error on " + src;
        ok = false;
    }
    fclose(in);
    if (ok && written == 0) {
        /* An empty source would leave a pad-only image that cannot boot. */
        err = "source is empty: " + src;
        ok = false;
    }
    if (ok && pad_to && written > pad_to) {
        err = "source is larger than the selected flash region: " + src;
        ok = false;
    }
    if (ok && written < pad_to) {
        std::vector<uint8_t> pad(kChunk, 0xFF);
        uint64_t left = pad_to - written;
        while (left > 0 && ok) {
            size_t c = left < kChunk ? (size_t)left : kChunk;
            ok = fwrite(pad.data(), 1, c, out) == c;
            left -= c;
        }
        if (!ok) {
            err = "short pad write to " + tmp;
        }
    }
    if (fclose(out) != 0 && ok) {
        err = "close failed on " + tmp;
        ok = false;
    }
    if (ok && g_rename(tmp.c_str(), dst.c_str()) != 0) {
        err = "cannot replace " + dst;
        ok = false;
    }
    if (!ok) {
        g_unlink(tmp.c_str()); /* never touch the existing dst on failure */
    }
    return ok;
}

/* Same temp-then-rename discipline as copy_padded(). */
static bool write_blank(const std::string &dst, uint64_t size, std::string &err)
{
    std::string tmp = dst + ".tmp";
    FILE *out = g_fopen(tmp.c_str(), "wb");
    if (!out) {
        err = "cannot create " + tmp;
        return false;
    }
    const size_t kChunk = 1 << 20;
    std::vector<uint8_t> buf(kChunk, 0xFF);
    uint64_t left = size;
    bool ok = true;
    while (left > 0) {
        size_t n = left < kChunk ? (size_t)left : kChunk;
        if (fwrite(buf.data(), 1, n, out) != n) {
            err = "short write to " + tmp;
            ok = false;
            break;
        }
        left -= n;
    }
    if (fclose(out) != 0 && ok) {
        err = "close failed on " + tmp;
        ok = false;
    }
    if (ok && g_rename(tmp.c_str(), dst.c_str()) != 0) {
        err = "cannot replace " + dst;
        ok = false;
    }
    if (!ok) {
        g_unlink(tmp.c_str());
    }
    return ok;
}

bool ProfileWizard::BuildWorker(std::string &err)
{
    // Step 0: create the profile dir + toml
    m_build_step.store(0);
    std::string name = m_name[0] ? m_name : m_name_hint;
    std::string id;
    if (m_edit_profile_id.empty()) {
        id = g_profile_store.Create(name, err);
        if (id.empty()) {
            return false;
        }
    } else {
        id = m_edit_profile_id;
        GwProfile *p = g_profile_store.Find(id);
        if (p) {
            p->display_name = name;
            p->rdp_locked = m_rdp_locked;
            g_profile_store.Save(*p, err);
        } else {
            err = "editing profile not found";
            return false;
        }
    }
    GwProfile *p = g_profile_store.Find(id);
    if (p) {
        p->rdp_locked = m_rdp_locked;
        p->nor_chip = m_nor_chip_key;
    }

    bool ok = true;
    bool patched_pair = m_patched && Bank1Game() >= 0;
    if (patched_pair) {
        // Patched OFW (stock template or custom with OFW bank1 +
        // matching assets -- ValidSources guarantees the pairing):
        // gnw_cfw_build writes bank1+extflash directly into the profile
        // (in-process -- no popen, see Phase 1).
        int gi = Bank1Game();
        const char *game = GnwBackupLibrary::GameName(gi);
        m_build_step.store(1);
        char *cerr = NULL;
        ok = gnw_cfw_build_images(game,
                                  m_library.InternalPath(gi).c_str(),
                                  m_library.ExternalPath(gi).c_str(),
                                  ResolvePatchBinary(gi).c_str(),
                                  p->Bank1Path().c_str(),
                                  p->ExtflashPath().c_str(),
                                  NorChip(m_nor_chip_key)->size_bytes,
                                  NULL, &cerr);
        if (!ok) {
            err = cerr ? cerr : "CFW patch failed";
            free(cerr);
        }
        p->prov_bank1 = std::string("patched-") + game;
        p->prov_extflash = std::string("patched-") + game;
        if (ok) {
            m_build_step.store(2);
            if (m_template != TplCustom || m_bank2_choice == B2Blank) {
                ok = write_blank(p->Bank2Path(), kBankSize, err);
                p->prov_bank2 = "blank";
            } else {
                ok = copy_padded(m_bank2_path, p->Bank2Path(), kBankSize, err);
                p->prov_bank2 = "user-file";
            }
        }
    } else if (m_template == TplStockMario || m_template == TplStockZelda) {
        int gi = m_template == TplStockMario ? 0 : 1;
        const char *game = GnwBackupLibrary::GameName(gi);
        {
            m_build_step.store(1);
            ok = copy_padded(m_library.InternalPath(gi), p->Bank1Path(), kBankSize, err);
            if (ok) {
                m_build_step.store(2);
                ok = copy_padded(m_library.ExternalPath(gi), p->ExtflashPath(),
                                 NorChip(m_nor_chip_key)->size_bytes, err);
            }
            p->prov_bank1 = std::string("ofw-") + game;
            p->prov_extflash = std::string("ofw-") + game;
        }
        if (ok) {
            m_build_step.store(3);
            ok = write_blank(p->Bank2Path(), kBankSize, err);
            p->prov_bank2 = "blank";
        }
    } else {
        // Custom -- every slot independently sourced; all content-blind.
        m_build_step.store(1);
        switch (m_bank1_choice) {
        case B1Blank:
            ok = write_blank(p->Bank1Path(), kBankSize, err);
            p->prov_bank1 = "blank";
            break;
        case B1OfwMario:
        case B1OfwZelda: {
            int gi = m_bank1_choice == B1OfwMario ? 0 : 1;
            ok = copy_padded(m_library.InternalPath(gi), p->Bank1Path(), kBankSize, err);
            p->prov_bank1 = std::string("ofw-") + GnwBackupLibrary::GameName(gi);
            break;
        }
        case B1File:
            ok = copy_padded(m_bank1_path, p->Bank1Path(), kBankSize, err);
            p->prov_bank1 = "user-file";
            break;
        }
        if (ok) {
            m_build_step.store(2);
            if (m_bank2_choice == B2Blank) {
                ok = write_blank(p->Bank2Path(), kBankSize, err);
                p->prov_bank2 = "blank";
            } else {
                ok = copy_padded(m_bank2_path, p->Bank2Path(), kBankSize, err);
                p->prov_bank2 = "user-file";
            }
        }
        if (ok) {
            m_build_step.store(3);
            uint64_t blank_size = (uint64_t)m_ext_size_mib << 20;
            switch (m_ext_choice) {
            case ExtBlank:
                ok = write_blank(p->ExtflashPath(), blank_size, err);
                p->prov_extflash = "blank";
                break;
            case ExtOfwMario:
            case ExtOfwZelda: {
                // "Blank + OFW assets" = plain byte-copy of the user's OFW
                // extflash dump, 0xFF-padded to the chosen size. No offset
                // awareness -- the dump's own layout IS the layout.
                int gi = m_ext_choice == ExtOfwMario ? 0 : 1;
                ok = copy_padded(m_library.ExternalPath(gi), p->ExtflashPath(),
                                 blank_size, err);
                p->prov_extflash = std::string("ofw-") + GnwBackupLibrary::GameName(gi);
                break;
            }
            case ExtFile:
                ok = copy_padded(m_ext_path, p->ExtflashPath(), blank_size, err);
                p->prov_extflash = "user-file";
                break;
            }
        }
    }

    if (ok) {
        m_build_step.store(3);
        ok = PadFileToFF(p->ExtflashPath(), NorChip(m_nor_chip_key)->size_bytes,
                         err);
    }

    // SD card (step 4).
    if (ok && m_sd_mode == SdNew && kSdCreateEnabled) {
        m_build_step.store(4);
        std::string dst;
        if (m_sd_storage == 0) {
            // Bundled: into the profile dir under the fixed name.
            dst = p->dir + "/sdcard.qcow2";
            p->sd.mode = GwSdMode::Bundled;
            p->sd.image = "sdcard.qcow2";
        } else {
            // Shared storage: into the sd-cards/ registry, named after the
            // profile, deduped by numeric suffix, shareable=true sidecar
            // (same conventions as Import below).
            std::string stem = name;
            for (char &c : stem) {
                if (c == '/' || c == '\\') {
                    c = '_';
                }
            }
            std::string root = GwProfileStore::SdCardsRoot();
            g_mkdir_with_parents(root.c_str(), 0755);
            std::string sd_name = stem + ".qcow2";
            for (int n = 2; g_file_test((root + "/" + sd_name).c_str(),
                                        G_FILE_TEST_EXISTS); n++) {
                sd_name = stem + "-" + std::to_string(n) + ".qcow2";
            }
            dst = root + "/" + sd_name;
            p->sd.mode = GwSdMode::Shared;
            p->sd.image = sd_name;
        }
        char *cerr = NULL;
        ok = gwemu_sdcreate_qcow2(dst.c_str(),
                                  (uint64_t)m_sd_new_size_gib << 30,
                                  NULL, &cerr);
        if (!ok) {
            err = cerr ? cerr : "SD card creation failed";
        }
        free(cerr);
        if (ok && p->sd.mode == GwSdMode::Shared) {
            std::string sidecar = dst.substr(0, dst.size() - 6) + ".toml";
            FILE *f = g_fopen(sidecar.c_str(), "wb");
            if (f) {
                char *b2 = g_path_get_basename(dst.c_str());
                fprintf(f, "shareable = true\ndisplay_name = \"%s\"\n", b2);
                g_free(b2);
                fclose(f);
            } else {
                err = "cannot create " + sidecar;
                ok = false;
            }
        }
        if (!ok) {
            p->sd = GwProfileSd();
        }
    } else if (ok && m_sd_mode == SdImport) {
        m_build_step.store(4);
        std::string dst;
        if (m_sd_storage == 0) {
            // Bundled: into the profile dir under the fixed name.
            dst = p->dir + "/sdcard.qcow2";
            p->sd.mode = GwSdMode::Bundled;
            p->sd.image = "sdcard.qcow2";
        } else {
            // Shared storage: into the sd-cards/ registry under the
            // original basename, deduped by numeric suffix, with a
            // shareable=true sidecar.
            char *b = g_path_get_basename(m_sd_import_path.c_str());
            std::string base = b;
            g_free(b);
            std::string stem = base, ext = "";
            size_t dot = base.rfind(".qcow2");
            if (dot != std::string::npos && dot == base.size() - 6) {
                stem = base.substr(0, dot);
            }
            std::string root = GwProfileStore::SdCardsRoot();
            g_mkdir_with_parents(root.c_str(), 0755);
            std::string sd_name = stem + ".qcow2";
            for (int n = 2; g_file_test((root + "/" + sd_name).c_str(),
                                        G_FILE_TEST_EXISTS); n++) {
                sd_name = stem + "-" + std::to_string(n) + ".qcow2";
            }
            dst = root + "/" + sd_name;
            p->sd.mode = GwSdMode::Shared;
            p->sd.image = sd_name;
        }
        if (m_sd_import_path != dst) {
            if (m_sd_transfer == 1) {
                // Move: rename when possible, copy+unlink across filesystems.
                if (g_rename(m_sd_import_path.c_str(), dst.c_str()) != 0) {
                    ok = copy_padded(m_sd_import_path, dst, 0, err);
                    if (ok) {
                        g_unlink(m_sd_import_path.c_str());
                    }
                }
            } else {
                ok = copy_padded(m_sd_import_path, dst, 0, err);
            }
        }
        if (ok && p->sd.mode == GwSdMode::Shared) {
            std::string sidecar = dst.substr(0, dst.size() - 6) + ".toml";
            FILE *f = g_fopen(sidecar.c_str(), "wb");
            if (f) {
                char *b2 = g_path_get_basename(dst.c_str());
                fprintf(f, "shareable = true\ndisplay_name = \"%s\"\n", b2);
                g_free(b2);
                fclose(f);
            } else {
                err = "cannot create " + sidecar;
                ok = false;
            }
        }
        if (!ok) {
            p->sd = GwProfileSd();
        }
    } else if (ok && m_sd_mode == SdShared) {
        m_build_step.store(4);
        p->sd.mode = GwSdMode::Shared;
        p->sd.image = m_sd_shared_image;   // no file ops -- just attach
    }

    if (!ok) {
        std::string derr;
        g_profile_store.Delete(id, derr); // best-effort cleanup
        return false;
    }

    m_build_step.store(5);
    if (!g_profile_store.Save(*p, err)) {
        return false;
    }
    GwProfileStore::RecalcDiskUsage(*p);
    m_created_id = id;
    return true;
}

void ProfileWizard::StartBuild()
{
    if (m_build_state.load() == BuildRunning) {
        return;
    }
    JoinWorker();
    m_build_error.clear();
    m_build_state.store(BuildRunning);
    m_thread = std::thread([this]() {
        std::string err;
        bool ok = BuildWorker(err);
        if (!ok) {
            m_build_error = err;   // before the state flip
        }
        m_build_state.store(ok ? BuildDone : BuildFailed);
    });
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */

int ProfileWizard::CheckSdImport() const
{
    // 1s TTL, same reasoning as ResolvePatchBinary: this runs from
    // ValidSources() every frame and must not hit the filesystem per
    // frame.
    if (m_sd_import_path.empty()) {
        return 1;
    }
    uint64_t now = SDL_GetTicks();
    if (m_sd_import_path != m_sd_chk_path ||
        now - m_sd_chk_ms > 1000 || m_sd_chk_ms == 0) {
        m_sd_chk_path = m_sd_import_path;
        m_sd_chk_ms = now;
        FILE *f = g_fopen(m_sd_import_path.c_str(), "rb");
        if (!f) {
            m_sd_chk_result = 2;
        } else {
            unsigned char magic[4] = { 0 };
            size_t n = fread(magic, 1, 4, f);
            fclose(f);
            m_sd_chk_result =
                (n == 4 && memcmp(magic, "QFI\xfb", 4) == 0) ? 0 : 3;
        }
    }
    return m_sd_chk_result;
}

// SD validity is independent of the flash-slot template rules; None/New
// are always valid.
static const char *SdInvalidReason(int chk)
{
    switch (chk) {
    case 1: return "sd: no file selected";
    case 2: return "sd: file not found";
    case 3: return "sd: not a qcow2 image";
    }
    return NULL;
}

bool ProfileWizard::ValidSources(std::string *why) const
{
    if (m_sd_mode == SdImport) {
        const char *r = SdInvalidReason(CheckSdImport());
        if (r) {
            if (why) *why = r;
            return false;
        }
    } else if (m_sd_mode == SdShared && m_sd_shared_image.empty()) {
        if (why) *why = "sd: no shared card selected";
        return false;
    }
    // Unified Patched rules (one concept for stock and custom): needs an
    // OFW bank1, a matching-game assets extflash (the patch transforms
    // both), and the gnwmanager patch binary (auto-downloaded).
    if (m_patched) {
        int gi = Bank1Game();
        if (gi < 0) {
            if (why) *why = "Patched needs an OFW in bank 1";
            return false;
        }
        if (!((gi == 0 && m_ext_choice == ExtOfwMario) ||
              (gi == 1 && m_ext_choice == ExtOfwZelda))) {
            if (why) *why = std::string("Patched needs matching ") +
                            GnwBackupLibrary::GameName(gi) + " assets in extflash";
            return false;
        }
        if (ResolvePatchBinary(gi).empty()) {
            int dl = m_dl_state[gi].load();
            if (why) {
                *why = dl == 1 ? "downloading patch binary..."
                     : dl == 3 ? "patch binary download failed"
                               : "patch binary not available yet";
            }
            return false;
        }
    }
    if (NorChip(m_nor_chip_key)->size_bytes < RequiredExtflashBytes()) {
        if (why) *why = "extflash: selected NOR chip is smaller than the image requirement";
        return false;
    }
    if (m_template == TplStockMario || m_template == TplStockZelda) {
        int gi = m_template == TplStockMario ? 0 : 1;
        if (!m_library.status[gi].Available() || !m_library.status[gi].external_found) {
            if (why) *why = "verified OFW dumps not found in the backup folder";
            return false;
        }
        return true;
    }
    if (m_bank1_choice == B1File && m_bank1_path.empty()) {
        if (why) *why = "bank1: no file selected";
        return false;
    }
    if ((m_bank1_choice == B1OfwMario && !m_library.status[0].Available()) ||
        (m_bank1_choice == B1OfwZelda && !m_library.status[1].Available())) {
        if (why) *why = "bank1: OFW dump not available/verified";
        return false;
    }
    if (m_bank2_choice == B2File && m_bank2_path.empty()) {
        if (why) *why = "bank2: no file selected";
        return false;
    }
    if ((m_ext_choice == ExtOfwMario && !m_library.status[0].external_found) ||
        (m_ext_choice == ExtOfwZelda && !m_library.status[1].external_found)) {
        if (why) *why = "extflash: OFW dump not found";
        return false;
    }
    if (m_ext_choice == ExtFile && m_ext_path.empty()) {
        if (why) *why = "extflash: no file selected";
        return false;
    }
    // Check uniqueness among file selections
    const std::string *paths[3] = {nullptr, nullptr, nullptr};
    if (m_bank1_choice == B1File) paths[0] = &m_bank1_path;
    if (m_bank2_choice == B2File) paths[1] = &m_bank2_path;
    if (m_ext_choice == ExtFile) paths[2] = &m_ext_path;

    for (int i = 0; i < 3; i++) {
        if (!paths[i]) continue;
        for (int j = i + 1; j < 3; j++) {
            if (paths[j] && *paths[i] == *paths[j]) {
                if (why) *why = "All selected custom files must be unique";
                return false;
            }
        }
    }
    
    return true;
}

void ProfileWizard::SetNorChip(const std::string &key)
{
    const GnwNorChip *chip = NorChip(key);
    m_nor_chip_key = chip->key;
    m_ext_size_mib = (int)(chip->size_bytes >> 20);
}

void ProfileWizard::EnsureNorChipFitsExtflash()
{
    uint64_t required = RequiredExtflashBytes();
    if (NorChip(m_nor_chip_key)->size_bytes >= required) {
        return;
    }

    // The choices are sorted by capacity, so this picks the smallest
    // supported chip that can preserve the selected source blob.
    for (const GnwNorChip *chip : NorChipChoices()) {
        if (chip->size_bytes >= required) {
            SetNorChip(chip->key);
            return;
        }
    }
}

uint64_t ProfileWizard::RequiredExtflashBytes() const
{
    std::string path;
    if (m_ext_choice == ExtOfwMario) path = m_library.ExternalPath(0);
    else if (m_ext_choice == ExtOfwZelda) path = m_library.ExternalPath(1);
    else if (m_ext_choice == ExtFile) path = m_ext_path;

    uint64_t now = SDL_GetTicks();
    if (path == m_required_extflash_path &&
        now - m_required_extflash_check_ms < 1000) {
        return m_required_extflash_bytes;
    }

    m_required_extflash_path = path;
    m_required_extflash_check_ms = now;
    m_required_extflash_bytes = 0;
    if (!path.empty()) {
        GStatBuf st;
        if (g_stat(path.c_str(), &st) == 0 && st.st_size > 0) {
            m_required_extflash_bytes = (uint64_t)st.st_size;
        }
    }
    return m_required_extflash_bytes;
}

// While a stock template is active the Bank Assignments accordion is a
// read-only reflection of what the template will build -- keep the slot
// state mirroring it so opening the accordion always shows the truth.
void ProfileWizard::SyncStockReflection()
{
    if (m_template == TplStockMario) {
        SetNorChip("c2-25-34"); // MX25U8035F, 1 MiB
        m_bank1_choice = B1OfwMario;
        m_bank2_choice = B2Blank;
        m_ext_choice = ExtOfwMario;
        m_patched = true;   // assumed case, incl. Stock Mario (owner call)
        EnsureNorChipFitsExtflash();
    } else if (m_template == TplStockZelda) {
        SetNorChip("c2-25-36"); // MX25U3232F, 4 MiB
        m_bank1_choice = B1OfwZelda;
        m_bank2_choice = B2Blank;
        m_ext_choice = ExtOfwZelda;
        m_patched = true;   // assumed case (owner call)
        EnsureNorChipFitsExtflash();
    }
}

/* ------------------------------------------------------------------ */
/* Firmware stage (S1a prompt / S1b status) + the game-card motif       */

static const char *kGameDisplay[2] = { "Mario", "Zelda" };



static void PushAccentButton()
{
    ImGui::PushStyleColor(ImGuiCol_Button, GNW_COL_ACCENT);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GNW_COL_ACCENT_HOVER);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, GNW_COL_ACCENT_ACTIVE);
}

static void PopAccentButton()
{
    ImGui::PopStyleColor(3);
}

int ProfileWizard::CompleteCount() const
{
    int n = 0;
    for (int i = 0; i < 2; i++) {
        if (GnwCardStateOf(m_library.status[i]) == kCardComplete) {
            n++;
        }
    }
    return n;
}

void ProfileWizard::PickFolder()
{
    // Default-location convention: only pass it if it exists as an
    // absolute path, else NULL (SDL rejects relative paths).
    const char *def = NULL;
    if (g_path_is_absolute(m_library.dir.c_str()) &&
        g_file_test(m_library.dir.c_str(), G_FILE_TEST_IS_DIR)) {
        def = m_library.dir.c_str();
    }
    ShowOpenFolderDialog(def, [this](const char *path) {
        m_library.dir = path;
        gwemu_settings_set_string(&g_config.general.firmware_backup_dir,
                                  path);
        gwemu_settings_save();
        m_library.Rescan();
        m_folder_chosen = true;
        if (m_stage == StageFwPrompt) {
            m_stage = StageFwStatus;
        }
        if ((m_template == TplStockMario && !m_library.status[0].Available()) ||
            (m_template == TplStockZelda && !m_library.status[1].Available())) {
            m_template = TplCustom;
        }
    });
}

void ProfileWizard::EnterProfileStage()
{
    if (!m_s2_visited) {
        m_s2_visited = true;
        // Template default on first S2 entry: first Complete stock game
        // if any, else Custom.
        int first = -1;
        for (int i = 0; i < 2; i++) {
            if (GnwCardStateOf(m_library.status[i]) == kCardComplete) {
                first = i;
                break;
            }
        }
        if (first >= 0) {
            m_template = first;
            SyncStockReflection();
            m_close_assignments_next = true; // stock preselect starts closed
        } else {
            m_template = TplCustom;
            m_open_assignments_next = true;
        }
    }
    m_stage = StageProfile;
}

void ProfileWizard::DrawCardTooltip(int gi)
{
    const GnwBackupGameStatus &st = m_library.status[gi];
    GnwGameCardState cs = GnwCardStateOf(st);
    const char *game = GnwBackupLibrary::GameName(gi);
    ImGui::BeginTooltip();
    ImGui::Text("%s firmware", kGameDisplay[gi]);
    ImGui::Separator();
    // Per-blob rows (filenames live here, never on the card face).
    ImGui::TextUnformatted("intflash ");
    ImGui::SameLine();
    if (st.internal_found && st.internal_verified) {
        ImGui::TextColored(GNW_COL_ACCENT, ICON_FA_CHECK " verified");
    } else if (st.internal_found) {
        ImGui::TextColored(GNW_COL_ERR, ICON_FA_XMARK " wrong dump");
    } else {
        ImGui::TextDisabled("missing");
    }
    ImGui::TextUnformatted("extflash ");
    ImGui::SameLine();
    if (st.external_found) {
        ImGui::TextColored(GNW_COL_ACCENT, ICON_FA_CHECK " present");
    } else {
        ImGui::TextDisabled("missing");
    }
    if (cs == kCardPartial) {
        ImGui::Spacing();
        std::string want = !st.internal_found
            ? std::string("internal_flash_backup_") + game + ".bin"
            : std::string("flash_backup_") + game + ".bin";
        ImGui::TextDisabled("Expected %s in this folder.", want.c_str());
    } else if (cs == kCardMismatch) {
        ImGui::Spacing();
        ImGui::TextUnformatted("SHA1 doesn't match a stock dump.");
        ImGui::PushFont(g_font_mgr.m_fixed_width_font);
        ImGui::Text("expected %s", GnwStockInternalSha1(gi));
        ImGui::Text("found    %s", st.internal_sha1.c_str());
        ImGui::PopFont();
    }
    ImGui::EndTooltip();
}

void ProfileWizard::DrawGameCard(int gi, float w)
{
    float sc = g_viewport_mgr.m_scale;
    GnwGameCardState st = GnwCardStateOf(m_library.status[gi]);

    // Transition bookkeeping: one-shot 250ms ease-in when a card becomes
    // Complete mid-flow (never on first show, never looping).
    if (m_card_prev_state[gi] != (int)st) {
        if (st == kCardComplete && m_card_prev_state[gi] != -1) {
            m_card_lit_ns[gi] = SDL_GetTicks();
            m_focus_continue = true;
        }
        m_card_prev_state[gi] = (int)st;
    }
    float lit = 1.0f;
    if (st == kCardComplete && m_card_lit_ns[gi] != 0) {
        float t = (SDL_GetTicks() - m_card_lit_ns[gi]) / 250.0f;
        lit = t >= 1.0f ? 1.0f : t * t; // ease-in
    }

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 size(w, 76 * sc);
    ImVec2 end(pos.x + size.x, pos.y + size.y);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float r = 10 * sc;
    float pad = 14 * sc;

    ImVec4 card_bg(0.125f, 0.14f, 0.135f, 1.0f);
    ImVec4 border(0.25f, 0.29f, 0.27f, 0.9f);
    border.w = 0.5f;
    if (st == kCardComplete) {
        border = GNW_COL_ACCENT_DIM;
        border.w *= lit;
    } else if (st == kCardPartial) {
        border = GNW_COL_WARN;
        border.w = 0.4f;
    }
    dl->AddRectFilled(pos, end, ImGui::GetColorU32(card_bg), r);
    if (st == kCardComplete) {
        ImVec4 wash = GNW_COL_ACCENT_WASH;
        wash.w *= lit;
        dl->AddRectFilled(pos, end, ImGui::GetColorU32(wash), r);
    }
    dl->AddRect(pos, end, ImGui::GetColorU32(border), r, 0, 1.0f);

    // Face text: max two lines, one status glyph max.
    ImU32 dis = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    ImU32 txt = st == kCardAbsent ? dis : ImGui::GetColorU32(ImGuiCol_Text);
    float lh = ImGui::GetTextLineHeight();
    float y0 = pos.y + (size.y - 2 * lh - 4 * sc) * 0.5f;
    std::string l1 = std::string(ICON_FA_MICROCHIP "  ") + kGameDisplay[gi];
    dl->AddText(ImVec2(pos.x + pad, y0), txt, l1.c_str());
    ImVec2 l2p(pos.x + pad, y0 + lh + 4 * sc);
    switch (st) {
    case kCardAbsent:
        dl->AddText(l2p, dis, "not added");
        break;
    case kCardComplete: {
        dl->AddText(l2p, dis, "firmware ready");
        ImVec4 c = GNW_COL_ACCENT;
        c.w *= lit;
        ImVec2 cs = ImGui::CalcTextSize(ICON_FA_CHECK);
        dl->AddText(ImVec2(end.x - pad - cs.x, pos.y + pad * 0.75f),
                    ImGui::GetColorU32(c), ICON_FA_CHECK);
        break;
    }
    case kCardPartial:
        dl->AddText(l2p, ImGui::GetColorU32(GNW_COL_WARN),
                    !m_library.status[gi].internal_found
                        ? ICON_FA_CIRCLE_INFO " intflash missing"
                        : ICON_FA_CIRCLE_INFO " extflash missing");
        break;
    case kCardMismatch:
        dl->AddText(l2p, ImGui::GetColorU32(GNW_COL_ERR),
                    ICON_FA_XMARK " wrong dump");
        break;
    }

    ImGui::PushID(gi);
    ImGui::InvisibleButton("##card", size);
    ImGui::PopID();
    if (ImGui::IsItemHovered()) {
        DrawCardTooltip(gi);
    }
}

void ProfileWizard::DrawFolderUnit()
{
    float sc = g_viewport_mgr.m_scale;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled(ICON_FA_FOLDER);
    ImGui::SameLine();
    float reserve = ImGui::CalcTextSize("Change...").x + 40 * sc;
    std::string p =
        EllipsizeMiddle(m_library.dir, ImGui::GetContentRegionAvail().x - reserve);
    ImGui::TextDisabled("%s", p.c_str());
    if (ImGui::IsItemHovered() && p != m_library.dir) {
        ImGui::SetTooltip("%s", m_library.dir.c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Change...##fwdir")) {
        PickFolder();
    }
}

void ProfileWizard::DrawFirmwareStage()
{
    float sc = g_viewport_mgr.m_scale;
    float win_w = ImGui::GetWindowWidth();

    ImGui::PushFont(g_font_mgr.m_menu_font_medium);
    const char *title = "Add official firmware";
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 6 * sc));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, GNW_GAP_SECTION * sc));

    if (m_stage == StageFwPrompt) {
        const char *hint =
            "GWemu can use firmware dumped from your own Game & Watch devices.";
        // Center only when it fits inside the content margin; otherwise
        // start at the margin and wrap before the opposite one -- naive
        // (win-text)/2 centering lands inside the margins when the line
        // is wider than the window (owner report: butted against both
        // sides).
        float margin = 28 * sc;
        float text_w = ImGui::CalcTextSize(hint).x;
        float x = (win_w - text_w) * 0.5f;
        ImGui::SetCursorPosX(x > margin ? x : margin);
        ImGui::PushTextWrapPos(win_w - margin);
        ImGui::TextDisabled("%s", hint);
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, GNW_GAP_SECTION * sc));
    }

    float gutter = GNW_GAP_GUTTER * sc;
    float cw = (ImGui::GetContentRegionAvail().x - gutter) * 0.5f;
    DrawGameCard(0, cw);
    ImGui::SameLine(0, gutter);
    DrawGameCard(1, cw);

    if (m_stage == StageFwStatus) {
        ImGui::Dummy(ImVec2(0, 12 * sc));
        DrawFolderUnit();
    }
}

void ProfileWizard::DrawFirmwareStrip()
{
    float sc = g_viewport_mgr.m_scale;
    bool any_files = false;
    for (int i = 0; i < 2; i++) {
        any_files |= m_library.status[i].internal_found ||
                     m_library.status[i].external_found;
    }
    if (!m_folder_chosen && !any_files) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Firmware: none");
        ImGui::SameLine();
        if (ImGui::SmallButton("Add...##fwstrip")) {
            m_stage = StageFwPrompt;
        }
        return;
    }

    ImGui::AlignTextToFramePadding();
    ImGui::BeginGroup();
    for (int gi = 0; gi < 2; gi++) {
        GnwGameCardState st = GnwCardStateOf(m_library.status[gi]);
        ImVec4 chip = st == kCardComplete ? GNW_COL_ACCENT
                    : st == kCardPartial  ? GNW_COL_WARN
                    : st == kCardMismatch ? GNW_COL_ERR
                    : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        ImGui::TextColored(chip, ICON_FA_MICROCHIP);
        ImGui::SameLine(0, 4 * sc);
        if (st == kCardAbsent) {
            ImGui::TextDisabled("%s", kGameDisplay[gi]);
        } else {
            ImGui::TextUnformatted(kGameDisplay[gi]);
        }
        ImGui::SameLine(0, 4 * sc);
        switch (st) {
        case kCardComplete:
            ImGui::TextColored(GNW_COL_ACCENT, ICON_FA_CHECK);
            break;
        case kCardPartial:
            ImGui::TextColored(GNW_COL_WARN, ICON_FA_CIRCLE_INFO);
            break;
        case kCardMismatch:
            ImGui::TextColored(GNW_COL_ERR, ICON_FA_XMARK);
            break;
        default:
            ImGui::Dummy(ImVec2(0, 0));
            break;
        }
        ImGui::SameLine(0, 14 * sc);
    }
    ImGui::TextDisabled(ICON_FA_FOLDER);
    ImGui::SameLine(0, 4 * sc);
    float reserve = ImGui::CalcTextSize("Change...").x + 30 * sc;
    ImGui::TextDisabled("%s",
        EllipsizeMiddle(m_library.dir,
                        ImGui::GetContentRegionAvail().x - reserve).c_str());
    ImGui::EndGroup();
    if (ImGui::IsItemHovered()) {
        // The full card pair, as a tooltip.
        ImGui::BeginTooltip();
        float cw = 220 * sc;
        DrawGameCard(0, cw);
        ImGui::SameLine(0, GNW_GAP_GUTTER * sc);
        DrawGameCard(1, cw);
        ImGui::EndTooltip();
    }
    if (ImGui::IsItemClicked()) {
        // Non-destructive: all S2 selections are kept; Continue returns.
        m_stage = StageFwStatus;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Change...##fwstrip")) {
        PickFolder();
    }
}


// Width reserved at the right of Bank Assignments for the Patched
// checkbox; the bank selectors share the same left edge and width.
// Source rows use a fixed-width selector and let the path consume the
// remaining space. This keeps the action button beside a readable path
// without allowing the source combo to dominate the row.
static void DrawWizardFileField(const char *str_id, const char *current_path,
                                const SDL_DialogFileFilter *filters,
                                int nfilters, float width,
                                std::function<void(const char *)> on_select)
{
    float sc = g_viewport_mgr.m_scale;
    ImGui::PushID(str_id);
    ImGui::BeginGroup();
    ImGui::TextDisabled(ICON_FA_FILE);
    ImGui::SameLine(0, 4 * sc);

    bool has_path = current_path && *current_path;
    const char *path = has_path ? current_path : "(no file)";
    float button_w = ImGui::CalcTextSize("Change...").x +
                     ImGui::GetStyle().FramePadding.x * 2 + 2 * sc;
    float text_w = width - ImGui::CalcTextSize(ICON_FA_FILE).x -
                   4 * sc - button_w - ImGui::GetStyle().ItemSpacing.x;
    std::string display = EllipsizeMiddle(path, std::max(24 * sc, text_w));
    ImGui::TextDisabled("%s", display.c_str());
    if (ImGui::IsItemHovered() && has_path) {
        ImGui::SetTooltip("%s", path);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Change...")) {
        ShowOpenFileDialog(filters, nfilters, current_path, on_select);
    }
    ImGui::EndGroup();
    ImGui::PopID();
}

void ProfileWizard::DrawNorChipSelector(float width)
{
    const GnwNorChip *selected = NorChip(m_nor_chip_key);
    char preview[128];
    snprintf(preview, sizeof(preview), "%" PRIu64 " MiB (%s)",
             selected->size_bytes >> 20, selected->name);

    const auto &choices = NorChipChoices();
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("##nor_chip", preview,
                          ImGuiComboFlags_HeightLargest)) {
        uint64_t required = RequiredExtflashBytes();
        for (const GnwNorChip *chip : choices) {
            char label[128];
            snprintf(label, sizeof(label), "%" PRIu64 " MiB (%s)",
                     chip->size_bytes >> 20, chip->name);
            ImGui::BeginDisabled(chip->size_bytes < required);
            bool same_part = selected->size_bytes == chip->size_bytes &&
                             selected->profile == chip->profile &&
                             strcmp(selected->name, chip->name) == 0;
            if (ImGui::Selectable(label, same_part)) {
                SetNorChip(chip->key);
            }
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
}

void ProfileWizard::DrawBankAssignments()
{
    if (m_open_assignments_next) {
        ImGui::SetNextItemOpen(true);
        m_open_assignments_next = false;
    }
    if (m_close_assignments_next) {
        ImGui::SetNextItemOpen(false);
        m_close_assignments_next = false;
    }
    ImGui::PushFont(g_font_mgr.m_menu_font_medium);
    m_assignments_open = ImGui::CollapsingHeader("Flash");
    ImGui::PopFont();
    if (!m_assignments_open) {
        return;
    }

    // Always editable: under a stock template these show the reflected
    // stock values (synced at selection time); the first edit here
    // switches the Template to Custom, keeping every current value --
    // including the one just changed (owner call, reverses the earlier
    // read-only-reflection behavior).
    bool changed = false;

    ImGui::Indent();

    const float scale = g_viewport_mgr.m_scale;
    const float label_width = 120 * scale;
    const float source_width = 180 * scale;

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Bank 1");
    ImGui::SameLine(label_width);
    // OFW entries appear only when found+verified in the backup library
    // -- mirror of the hidden-until-available stock templates.
    struct B1Item { const char *label; int choice; };
    B1Item b1_items[4];
    int b1_n = 0;
    b1_items[b1_n++] = { "Empty", B1Blank };
    if (m_library.status[0].Available()) b1_items[b1_n++] = { "Mario OFW", B1OfwMario };
    if (m_library.status[1].Available()) b1_items[b1_n++] = { "Zelda OFW", B1OfwZelda };
    b1_items[b1_n++] = { "File...", B1File };
    int b1_sel = 0;
    for (int i = 0; i < b1_n; i++) {
        if (b1_items[i].choice == m_bank1_choice) b1_sel = i;
    }
    bool b1_show_patch = Bank1Game() >= 0;
    ImGui::SetNextItemWidth(source_width);
    if (ImGui::BeginCombo("##b1", b1_items[b1_sel].label)) {
        for (int i = 0; i < b1_n; i++) {
            if (ImGui::Selectable(b1_items[i].label, i == b1_sel)) {
                if (m_bank1_choice != b1_items[i].choice) {
                    m_bank1_choice = b1_items[i].choice;
                    changed = true;
                    if (m_bank1_choice == B1File) {
                        ShowOpenFileDialog(kBinFilter, 2, m_bank1_path.c_str(),
                                           [this](const char *p) { m_bank1_path = p; });
                    }
                    if (Bank1Game() < 0) {
                        m_patched = false; // hidden checkbox never lingers on
                    } else {
                        // Assets follow the OFW game: if extflash held the
                        // other game's assets (or Patched demands matching
                        // ones), switch it rather than leaving a mismatch
                        // for validation to complain about -- confirmed
                        // real misfire: picking Zelda OFW here while
                        // extflash still said Mario Assets read as "your
                        // zelda assets aren't valid" to the owner.
                        int gi = Bank1Game();
                        int want = gi == 0 ? ExtOfwMario : ExtOfwZelda;
                        bool have = m_library.status[gi].external_found;
                        bool was_assets = m_ext_choice == ExtOfwMario ||
                                          m_ext_choice == ExtOfwZelda;
                        if (have && (was_assets || m_patched)) {
                            m_ext_choice = want;
                            EnsureNorChipFitsExtflash();
                        }
                    }
                }
            }
        }
        ImGui::EndCombo();
    }
    if (b1_show_patch) {
        ImGui::SameLine(0, 12 * scale);
        // Deliberately NOT part of `changed`: toggling Patched on a
        // stock template is still that stock template (patched stock is
        // a first-class stock flavor), it must not flip to Custom.
        if (ImGui::Checkbox("Patched", &m_patched) && m_patched) {
            // Patched hard-requires the matching game's assets -- align
            // extflash automatically when they're available instead of
            // failing validation.
            int g = Bank1Game();
            if (g >= 0 && m_library.status[g].external_found) {
                m_ext_choice = g == 0 ? ExtOfwMario : ExtOfwZelda;
                EnsureNorChipFitsExtflash();
            }
        }
        int gi = Bank1Game();
        if (m_patched && gi >= 0 && ResolvePatchBinary(gi).empty()) {
            if (m_dl_state[gi].load() == 0) {
                StartPatchDownload(gi);
            }
            if (m_dl_state[gi].load() == 1) {
                ImGui::TextDisabled("downloading patch binary...");
            } else if (m_dl_state[gi].load() == 3) {
                ImGui::TextDisabled("%s", m_dl_error[gi].c_str());
            }
        }
    }
    if (m_bank1_choice == B1File) {
        ImGui::SameLine(0, 10 * scale);
        DrawWizardFileField("##b1file", m_bank1_path.c_str(), kBinFilter, 2,
                            ImGui::GetContentRegionAvail().x,
                            [this](const char *p) { m_bank1_path = p; });
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Bank 2");
    ImGui::SameLine(label_width);
    const char *b2_items[] = { "Empty", "File..." };
    ImGui::SetNextItemWidth(source_width);
    if (ImGui::BeginCombo("##b2", b2_items[m_bank2_choice])) {
        for (int i = 0; i < 2; i++) {
            if (ImGui::Selectable(b2_items[i], i == m_bank2_choice)) {
                if (m_bank2_choice != i) {
                    m_bank2_choice = i;
                    changed = true;
                    if (m_bank2_choice == B2File) {
                        ShowOpenFileDialog(kBinFilter, 2, m_bank2_path.c_str(),
                                           [this](const char *p) { m_bank2_path = p; });
                    }
                }
            }
        }
        ImGui::EndCombo();
    }
    if (m_bank2_choice == B2File) {
        ImGui::SameLine(0, 10 * scale);
        DrawWizardFileField("##b2file", m_bank2_path.c_str(), kBinFilter, 2,
                            ImGui::GetContentRegionAvail().x,
                            [this](const char *p) { m_bank2_path = p; });
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Extflash");
    ImGui::SameLine(label_width);
    // Enum-keyed filtered list, same pattern as Bank 1 -- game-assets
    // entries appear only when that game's extflash dump exists.
    struct ExtItem { const char *label; int choice; };
    ExtItem ext_items[4];
    int ext_n = 0;
    ext_items[ext_n++] = { "Empty", ExtBlank };
    if (m_library.status[0].external_found) ext_items[ext_n++] = { "Mario Assets", ExtOfwMario };
    if (m_library.status[1].external_found) ext_items[ext_n++] = { "Zelda Assets", ExtOfwZelda };
    ext_items[ext_n++] = { "File...", ExtFile };
    int ext_sel = 0;
    for (int i = 0; i < ext_n; i++) {
        if (ext_items[i].choice == m_ext_choice) ext_sel = i;
    }
    float combo_gap = 10 * scale;
    float row_start = ImGui::GetCursorPosX() - label_width;
    float row_width = ImGui::GetWindowContentRegionMax().x - row_start;
    float nor_width = std::min(270 * scale, row_width * 0.34f);
    float nor_x = row_start + row_width - nor_width;
    float ext_width = 175 * scale;
    ImGui::SetNextItemWidth(ext_width);
    if (ImGui::BeginCombo("##ext", ext_items[ext_sel].label)) {
        for (int i = 0; i < ext_n; i++) {
            if (ImGui::Selectable(ext_items[i].label, i == ext_sel)) {
                if (m_ext_choice != ext_items[i].choice) {
                    m_ext_choice = ext_items[i].choice;
                    changed = true;
                    EnsureNorChipFitsExtflash();
                    if (m_ext_choice == ExtFile) {
                        ShowOpenFileDialog(kBinFilter, 2, m_ext_path.c_str(),
                                           [this](const char *p) {
                                               m_ext_path = p;
                                               EnsureNorChipFitsExtflash();
                                           });
                    }
                }
            }
        }
        ImGui::EndCombo();
    }
    if (m_ext_choice == ExtFile) {
        ImGui::SameLine(0, combo_gap);
        float path_width = std::max(80 * scale,
            nor_x - ImGui::GetCursorPosX() - combo_gap);
        DrawWizardFileField("##extfile", m_ext_path.c_str(), kBinFilter, 2,
                            path_width,
                            [this](const char *p) { m_ext_path = p; });
    }
    ImGui::SameLine(nor_x);
    DrawNorChipSelector(nor_width);
    ImGui::Unindent();

    if (changed && m_template != TplCustom) {
        m_template = TplCustom;
    }
}

void ProfileWizard::DrawSdSection()
{
    float sc = g_viewport_mgr.m_scale;
    std::vector<const GwSharedSd *> shareable = ShareableSds();

    if (m_close_sd_next) {
        ImGui::SetNextItemOpen(false);
        m_close_sd_next = false;
    }

    // Live summary for the header line, computed before the header so the
    // right edge can be measured. TextDisabled only for "No SD card".
    std::string summary;
    bool summary_dim = false;
    bool warn_glyph = false;
    switch (m_sd_mode) {
    case SdNone:
        summary = "No SD card";
        summary_dim = true;
        break;
    case SdNew:
        summary = std::string("New ") + std::to_string(m_sd_new_size_gib) +
                  " GiB card";
        break;
    case SdImport: {
        std::string base = "(no file)";
        if (!m_sd_import_path.empty()) {
            char *b = g_path_get_basename(m_sd_import_path.c_str());
            base = b;
            g_free(b);
        }
        summary = "Import: " + base;
        warn_glyph = m_sd_transfer == 1; // armed Move must not hide
        break;
    }
    case SdShared:
        summary = m_sd_shared_image.empty()
            ? "Shared: (none)"
            : "Shared: \"" + m_sd_shared_image + "\"";
        break;
    }

    ImGui::PushFont(g_font_mgr.m_menu_font_medium);
    m_sd_open = ImGui::CollapsingHeader("SD Card");
    ImGui::PopFont();

    // Right-aligned summary on the header line itself.
    {
        float right = ImGui::GetWindowContentRegionMax().x;
        float max_w = right - ImGui::GetItemRectMax().x + ImGui::GetWindowPos().x
                    - ImGui::CalcTextSize("SD Card").x - 60 * sc;
        std::string s = EllipsizeMiddle(summary, max_w > 40 * sc ? max_w : 40 * sc);
        float w = ImGui::CalcTextSize(s.c_str()).x;
        float glyph_w = 0;
        if (warn_glyph && !m_sd_open) {
            glyph_w = ImGui::CalcTextSize(ICON_FA_CIRCLE_INFO).x + 5 * sc;
        }
        ImGui::SameLine(right - w - glyph_w - ImGui::GetStyle().FramePadding.x);
        if (warn_glyph && !m_sd_open) {
            ImGui::TextColored(GNW_COL_WARN, ICON_FA_CIRCLE_INFO);
            ImGui::SameLine(0, 5 * sc);
        }
        if (summary_dim) {
            ImGui::TextDisabled("%s", s.c_str());
        } else {
            ImGui::TextUnformatted(s.c_str());
        }
    }
    if (!m_sd_open) {
        return;
    }

    ImGui::Indent();

    // Row 1: mode combo. "Shared card..." is present-but-disabled when
    // nothing is attachable (never hidden); "New card" is gated behind
    // kSdCreateEnabled (backend not landed).
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Card");
    ImGui::SameLine(120 * sc);
    const char *mode_label =
        m_sd_mode == SdNone   ? "None" :
        m_sd_mode == SdNew    ? "New card" :
        m_sd_mode == SdImport ? "File..." : "Shared card...";
    ImGui::SetNextItemWidth(190 * sc);
    if (ImGui::BeginCombo("##sdmode", mode_label)) {
        if (ImGui::Selectable("None", m_sd_mode == SdNone)) {
            m_sd_mode = SdNone;
        }
        if (kSdCreateEnabled) {
            if (ImGui::Selectable("New card", m_sd_mode == SdNew)) {
                m_sd_mode = SdNew;
            }
        }
        if (ImGui::Selectable("File...", m_sd_mode == SdImport)) {
            if (m_sd_mode != SdImport) {
                m_sd_mode = SdImport;
                ShowOpenFileDialog(kQcow2Filter, 2, m_sd_import_path.c_str(),
                                   [this](const char *path) {
                                       m_sd_import_path = path;
                                   });
            }
        }
        if (shareable.empty()) {
            ImGui::Selectable("Shared card... -- no shared cards yet", false,
                              ImGuiSelectableFlags_Disabled);
        } else if (ImGui::Selectable("Shared card...", m_sd_mode == SdShared)) {
            m_sd_mode = SdShared;
            if (m_sd_shared_image.empty() && shareable.size() == 1) {
                m_sd_shared_image = shareable[0]->image; // preselect
            }
        }
        ImGui::EndCombo();
    }

    auto storage_radios = [&]() {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Storage");
        ImGui::SameLine(120 * sc);
        float radio_x = ImGui::GetCursorPosX();
        ImGui::RadioButton("Bundled with profile", &m_sd_storage, 0);
        ImGui::SetCursorPosX(radio_x);
        ImGui::RadioButton("Shared (other profiles can attach)", &m_sd_storage, 1);
    };

    if (m_sd_mode == SdNew && kSdCreateEnabled) {
        // Size stepper in the right column, on the combo's row.
        ImGui::SameLine(0, 10 * sc);
        ImGui::BeginDisabled(m_sd_new_size_gib <= 4);
        if (ImGui::Button("-##sdsize")) {
            m_sd_new_size_gib = std::max(4, m_sd_new_size_gib / 2);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Text("%d GiB", m_sd_new_size_gib);
        ImGui::SameLine();
        ImGui::BeginDisabled(m_sd_new_size_gib >= 32);
        if (ImGui::Button("+##sdsize")) {
            m_sd_new_size_gib = std::min(32, m_sd_new_size_gib * 2);
        }
        ImGui::EndDisabled();

        storage_radios();
        ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
    } else if (m_sd_mode == SdImport) {
        ImGui::SameLine(0, 10 * sc);
        DrawWizardFileField("##sdimport", m_sd_import_path.c_str(),
                            kQcow2Filter, 2,
                            ImGui::GetContentRegionAvail().x,
                            [this](const char *p) { m_sd_import_path = p; });

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Transfer");
        ImGui::SameLine(120 * sc);
        ImGui::RadioButton("Copy", &m_sd_transfer, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Move original", &m_sd_transfer, 1);

        storage_radios();

        if (m_sd_transfer == 1) {
            ImGui::TextColored(GNW_COL_WARN, ICON_FA_CIRCLE_INFO
                " Move relocates the original file into this profile.");
        } else {
            ImGui::TextDisabled(
                "Copies the image; the original file is untouched.");
        }
    } else if (m_sd_mode == SdShared) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("From");
        ImGui::SameLine(120 * sc);
        if (shareable.empty()) {
            // Mode armed but registry emptied mid-session: disabled combo
            // + hint; Open() falls back to None next time.
            ImGui::BeginDisabled();
            ImGui::SetNextItemWidth(360 * sc);
            if (ImGui::BeginCombo("##sdshared", "(no shared cards)")) {
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            ImGui::TextDisabled("Mark a card as shareable to attach it here.");
        } else {
            if (m_sd_shared_image.empty() && shareable.size() == 1) {
                m_sd_shared_image = shareable[0]->image;
            }
            auto entry = [](const GwSharedSd *s) {
                double gib = (double)s->disk_bytes / (1024.0 * 1024.0 * 1024.0);
                char buf[192];
                snprintf(buf, sizeof(buf), "%s  --  %.1f GiB",
                         s->display_name.c_str(), gib);
                return std::string(buf);
            };
            std::string cur = "(select a card)";
            for (const GwSharedSd *s : shareable) {
                if (s->image == m_sd_shared_image) {
                    cur = entry(s);
                }
            }
            ImGui::SetNextItemWidth(360 * sc);
            if (ImGui::BeginCombo("##sdshared", cur.c_str())) {
                for (const GwSharedSd *s : shareable) {
                    if (ImGui::Selectable(entry(s).c_str(),
                                          s->image == m_sd_shared_image)) {
                        m_sd_shared_image = s->image;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled(
                "Shared with other profiles; changes are visible everywhere.");
        }
    }

    ImGui::Unindent();
}

void ProfileWizard::DrawForm()
{
    // Keep identity controls together: the profile name gets the flexible
    // space, while the lock toggle stays visible at the right.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Name");
    ImGui::SameLine();
    float lock_width = 100 * g_viewport_mgr.m_scale;
    ImGui::SetNextItemWidth(std::max(120 * g_viewport_mgr.m_scale,
        ImGui::GetContentRegionAvail().x - lock_width));
    ImGui::InputTextWithHint("##name", m_name_hint.c_str(), m_name, sizeof(m_name));
    ImGui::SameLine();
    ImGui::Checkbox("Locked", &m_rdp_locked);

    ImGui::Spacing();
    DrawFirmwareStrip();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(g_font_mgr.m_menu_font_medium);
    ImGui::TextUnformatted("Template");
    ImGui::PopFont();
    ImGui::SameLine(120 * g_viewport_mgr.m_scale);
    // Order (owner call): Mario, Zelda, Custom last. Stock entries are
    // hidden entirely (not disabled) until that game's dumps are
    // found+verified -- the backup-folder row above is how users make
    // them appear.
    const char *stock_names[2] = { "Stock Mario", "Stock Zelda" };
    int stock_template_selected = -1;
    for (int i = 0; i < 2; i++) {
        bool avail = m_library.status[i].Available() && m_library.status[i].external_found;
        if (!avail) {
            continue;
        }
        bool selected = m_template == i;
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.34f, 0.17f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.42f, 0.21f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, GNW_COL_ACCENT_ACTIVE);
        }
        bool clicked = ImGui::Button(stock_names[i]);
        if (selected) {
            ImGui::PopStyleColor(3);
        }
        if (clicked) {
            bool template_changed = m_template != i;
            if (template_changed) {
                m_close_assignments_next = true;
                // Also collapse the SD section (but NEVER reset SD state
                // -- it survives template switches by design).
                m_close_sd_next = true;
            }
            m_template = i;
            if (template_changed) {
                stock_template_selected = i;
                SyncStockReflection();
            }
        }
        ImGui::SameLine(0, 8 * g_viewport_mgr.m_scale);
    }
    bool custom_selected = m_template == TplCustom;
    if (custom_selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.34f, 0.17f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.42f, 0.21f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, GNW_COL_ACCENT_ACTIVE);
    }
    bool custom_clicked = ImGui::Button("Custom");
    if (custom_selected) {
        ImGui::PopStyleColor(3);
    }
    if (custom_clicked) {
        if (m_template != TplCustom) {
            m_open_assignments_next = true;
        }
        m_template = TplCustom;
    }

    DrawBankAssignments();
    DrawSdSection();

    // Apply the preset capacity after drawing the dependent flash controls,
    // so any same-frame widget state cannot leave the previous preset's
    // chip selected.
    if (m_template == stock_template_selected) {
        SetNorChip(stock_template_selected == TplStockMario
                       ? "c2-25-34" : "c2-25-36");
        EnsureNorChipFitsExtflash();
    }
}

void ProfileWizard::DrawBuildView()
{
    static const char *kSteps[] = {
        "Creating profile", "Building bank 1", "Building bank 2",
        "Building external flash", "Preparing SD card", "Finishing",
    };
    int state = m_build_state.load();
    int step = m_build_step.load();

    if (state == BuildRunning) {
        ImGui::Text("%s...", kSteps[step < 6 ? step : 5]);
        ImGui::ProgressBar((step + 1) / 6.0f, ImVec2(-1, 0));
    } else if (state == BuildDone) {
        JoinWorker();
        ImGui::TextUnformatted("Profile created.");
        ImGui::Spacing();
        if (ImGui::Button("Launch now", ImVec2(140 * g_viewport_mgr.m_scale, 0))) {
            GwProfile *p = g_profile_store.Find(m_created_id);
            if (p) {
                gwemu_settings_set_string(&g_config.general.active_profile,
                                          m_created_id.c_str());
                m_completed = true;
                is_open = false;
                std::string sdp = p->SdPath();
                gwemu_relaunch_with_flash_images(p->Bank1Path().c_str(),
                                                 p->Bank2Path().c_str(),
                                                 p->ExtflashPath().c_str(),
                                                 sdp.empty() ? NULL : sdp.c_str(),
                                                 p->rdp_locked,
                                                 p->nor_chip.c_str());
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Done", ImVec2(100 * g_viewport_mgr.m_scale, 0))) {
            gwemu_settings_set_string(&g_config.general.active_profile,
                                      m_created_id.c_str());
            gwemu_settings_save();
            m_completed = true;
            is_open = false;
        }
    } else if (state == BuildFailed) {
        JoinWorker();
        ImGui::TextUnformatted("Profile creation failed:");
        ImGui::TextWrapped("%s", m_build_error.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Back")) {
            m_build_state.store(BuildIdle);
        }
    }
}

void ProfileWizard::Draw()
{
    if (!is_open) {
        return;
    }

    // Hosted inside the settings window's own ImGui context (see
    // gwemu_settings_hud_update) -- fill that window edge to edge, with a
    // healthy uniform content margin (owner call: left-aligned content,
    // never hugging the edges).
    float sc = g_viewport_mgr.m_scale;
    float pad = 30 * sc;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
    bool began = ImGui::Begin("New Device Profile", nullptr,
                              ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                              ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDecoration |
                              ImGuiWindowFlags_NoBringToFrontOnFocus |
                              ImGuiWindowFlags_NoScrollbar |
                              ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!began) {
        ImGui::End();
        return;
    }

    // A wizard-local theme: softer surfaces, outlined controls, roomier
    // hit targets and a restrained green accent. The rest of the settings
    // UI keeps its existing palette.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9 * sc, 6 * sc));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10 * sc, 6 * sc));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(7 * sc, 5 * sc));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7 * sc);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 7 * sc);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.75f * sc);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.72f, 0.76f, 0.73f, 0.72f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.28f, 0.32f, 0.29f, 0.72f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.145f, 0.155f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.19f, 0.22f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.18f, 0.29f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.17f, 0.16f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.31f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, GNW_COL_ACCENT_ACTIVE);
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.145f, 0.19f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.18f, 0.27f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.20f, 0.34f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.36f, 0.40f, 0.37f, 0.42f));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, GNW_COL_ACCENT);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogramHovered, GNW_COL_ACCENT_HOVER);

    bool building = m_build_state.load() != BuildIdle;
    bool fw_stage = !building && m_stage != StageProfile;
    if (fw_stage) {
        DrawFirmwareStage(); // draws its own centered title
    } else {
        ImGui::PushFont(g_font_mgr.m_menu_font_medium);
        ImGui::TextUnformatted("New Device Profile");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 8 * sc));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 4 * sc));
        if (building) {
            DrawBuildView();
        } else {
            DrawForm();
        }
    }

    // Natural height of everything above the bottom row, before the footer
    // is pinned. Measuring the rendered cursor catches every conditional row
    // (including SD Card modes) without duplicating their state in a layout
    // signature.
    float content_bottom = ImGui::GetCursorPosY();
    float reason_h = ImGui::GetTextLineHeightWithSpacing();
    float btn_h = ImGui::GetFrameHeightWithSpacing();
    float footer_gap = 8 * sc + ImGui::GetStyle().ItemSpacing.y;
    float required_height = content_bottom + footer_gap + reason_h + btn_h + pad;
    m_height_fit.Observe(required_height, SDL_GetTicks());

    // Bottom row: Cancel (left) / Create (right), pinned to the window
    // bottom. Cancel = skip into the normal settings menu (the host
    // falls back there because WasCompleted() stays false).
    if (fw_stage) {
        // Firmware-stage bottom row: [Skip] left; right = the single
        // accent element (Select folder... on S1a, Continue on S1b once
        // at least one card is Complete).
        float y = ImGui::GetWindowHeight() - btn_h - pad;
        if (ImGui::GetCursorPosY() < y) {
            ImGui::SetCursorPosY(y - 8 * sc);
            ImGui::Separator();
            ImGui::SetCursorPosY(y);
        }
        if (ImGui::Button("Skip", ImVec2(110 * sc, 0))) {
            EnterProfileStage();
        }
        float bw = 160 * sc;
        ImGui::SameLine(ImGui::GetWindowWidth() - bw - pad);
        if (m_stage == StageFwPrompt) {
            m_focus_continue = false;
            PushAccentButton();
            if (ImGui::Button("Select folder...", ImVec2(bw, 0))) {
                PickFolder();
            }
            PopAccentButton();
        } else {
            bool lit = CompleteCount() >= 1;
            if (lit) {
                PushAccentButton();
                if (m_focus_continue) {
                    ImGui::SetKeyboardFocusHere();
                }
            }
            m_focus_continue = false;
            if (ImGui::Button("Continue", ImVec2(bw, 0))) {
                EnterProfileStage();
            }
            if (lit) {
                PopAccentButton();
            }
        }
    } else if (!building) {
        float y = ImGui::GetWindowHeight() - btn_h - pad;
        if (ImGui::GetCursorPosY() < y) {
            ImGui::SetCursorPosY(y - 8 * sc);
            ImGui::Separator();
            ImGui::SetCursorPosY(y);
        }
        if (ImGui::Button("Cancel", ImVec2(110 * g_viewport_mgr.m_scale, 0))) {
            is_open = false;
        }
        std::string why;
        bool can_create = ValidSources(&why);
        float create_w = 120 * sc;
        ImGui::SameLine(ImGui::GetWindowWidth() - create_w - pad);
        ImGui::BeginDisabled(!can_create);
        if (can_create) {
            PushAccentButton();
        }
        if (ImGui::Button("Create", ImVec2(create_w, 0))) {
            StartBuild();
        }
        if (can_create) {
            PopAccentButton();
        }
        ImGui::EndDisabled();
        if (!can_create) {
            // Reason, subtle, right-aligned above the button row.
            ImVec2 sz = ImGui::CalcTextSize(why.c_str());
            ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - sz.x - pad,
                                       y - ImGui::GetTextLineHeightWithSpacing()));
            ImGui::TextDisabled("%s", why.c_str());
        }
    }

    ImGui::PopStyleColor(14);
    ImGui::PopStyleVar(6);
    ImGui::End();
}
