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

#include <glib.h>
#include <glib/gstdio.h>
#include <algorithm>
#include <cfloat>
#include <cstring>

extern "C" {
#include "../../contrib/gnw-tools/gnw_cfw_build.h"
}

ProfileWizard g_profile_wizard;

static const uint32_t kBankSize = 0x40000u;      // 256K flash bank
static const uint64_t kExtPadOfw = 0x4000000u;   // 64MiB, matches the CFW tool

static const SDL_DialogFileFilter kBinFilter[] = {
    { "Firmware images (*.bin)", "bin" },
    { "All files", "*" },
};

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
        std::string dir = CachePatchDir(game);
        g_mkdir_with_parents(dir.c_str(), 0755);
        std::string dst = dir + "/0x08032000.bin";
        std::string tmp = dst + ".part";
        std::string url = PatchBinaryUrl(game);
        // TODO: replace the popen curl/wget chain with a proper
        // cross-platform in-process fetch (owner accepts this jank on
        // Linux for now; Windows static build has no shell tools).
        std::string cmd = "curl -fsSL -o '" + tmp + "' '" + url +
                          "' 2>/dev/null || wget -qO '" + tmp + "' '" + url + "'";
        int rc = -1;
        FILE *pf = popen(cmd.c_str(), "r");
        if (pf) {
            rc = pclose(pf);
        }
        GStatBuf st;
        bool ok = rc == 0 && g_stat(tmp.c_str(), &st) == 0 && st.st_size > 0;
        if (ok) {
            ok = g_rename(tmp.c_str(), dst.c_str()) == 0;
        }
        if (!ok) {
            g_unlink(tmp.c_str());
            m_dl_error[gi] = "download failed (check network); "
                             "url: " + url;
        }
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
    m_library.Rescan();
    if ((m_template == TplStockMario && !m_library.status[0].Available()) ||
        (m_template == TplStockZelda && !m_library.status[1].Available())) {
        m_template = TplCustom;
        m_open_assignments_next = true;
    } else if (m_template != TplCustom) {
        SyncStockReflection();
    }
    m_build_state.store(BuildIdle);
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
        m_stage = StageProfile;
        m_s2_visited = true;
    } else {
        m_stage = m_folder_chosen ? StageFwStatus : StageFwPrompt;
    }
    m_focus_continue = false;
    m_card_prev_state[0] = m_card_prev_state[1] = -1;
    m_card_lit_ns[0] = m_card_lit_ns[1] = 0;
    is_open = true;
}

/* ------------------------------------------------------------------ */
/* Build worker                                                        */

static bool copy_padded(const std::string &src, const std::string &dst,
                        uint64_t pad_to, std::string &err)
{
    FILE *in = g_fopen(src.c_str(), "rb");
    if (!in) {
        err = "cannot open " + src;
        return false;
    }
    FILE *out = g_fopen(dst.c_str(), "wb");
    if (!out) {
        fclose(in);
        err = "cannot create " + dst;
        return false;
    }
    const size_t kChunk = 1 << 20;
    std::vector<uint8_t> buf(kChunk);
    uint64_t written = 0;
    bool ok = true;
    size_t n;
    while ((n = fread(buf.data(), 1, kChunk, in)) > 0) {
        if (fwrite(buf.data(), 1, n, out) != n) {
            err = "short write to " + dst;
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
    if (ok && written < pad_to) {
        std::vector<uint8_t> pad(kChunk, 0xFF);
        uint64_t left = pad_to - written;
        while (left > 0 && ok) {
            size_t c = left < kChunk ? (size_t)left : kChunk;
            ok = fwrite(pad.data(), 1, c, out) == c;
            left -= c;
        }
        if (!ok) {
            err = "short pad write to " + dst;
        }
    }
    if (fclose(out) != 0 && ok) {
        err = "close failed on " + dst;
        ok = false;
    }
    if (!ok) {
        g_unlink(dst.c_str());
    }
    return ok;
}

static bool write_blank(const std::string &dst, uint64_t size, std::string &err)
{
    FILE *out = g_fopen(dst.c_str(), "wb");
    if (!out) {
        err = "cannot create " + dst;
        return false;
    }
    const size_t kChunk = 1 << 20;
    std::vector<uint8_t> buf(kChunk, 0xFF);
    uint64_t left = size;
    bool ok = true;
    while (left > 0) {
        size_t n = left < kChunk ? (size_t)left : kChunk;
        if (fwrite(buf.data(), 1, n, out) != n) {
            err = "short write to " + dst;
            ok = false;
            break;
        }
        left -= n;
    }
    if (fclose(out) != 0 && ok) {
        err = "close failed on " + dst;
        ok = false;
    }
    if (!ok) {
        g_unlink(dst.c_str());
    }
    return ok;
}

bool ProfileWizard::BuildWorker(std::string &err)
{
    // Step 0: create the profile dir + toml
    m_build_step.store(0);
    std::string name = m_name[0] ? m_name : m_name_hint;
    std::string id = g_profile_store.Create(name, err);
    if (id.empty()) {
        return false;
    }
    GwProfile *p = g_profile_store.Find(id);

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
                ok = copy_padded(m_library.ExternalPath(gi), p->ExtflashPath(), kExtPadOfw, err);
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
                ok = copy_padded(m_ext_path, p->ExtflashPath(), 0, err);
                p->prov_extflash = "user-file";
                break;
            }
        }
    }

    if (!ok) {
        std::string derr;
        g_profile_store.Delete(id, derr); // best-effort cleanup
        return false;
    }

    m_build_step.store(4);
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

bool ProfileWizard::ValidSources(std::string *why) const
{
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
    if (m_ext_choice != ExtFile && m_ext_size_mib < MinExtSizeMiB()) {
        if (why) *why = "extflash: Zelda content needs at least 4 MiB";
        return false;
    }
    return true;
}

bool ProfileWizard::ZeldaInvolved() const
{
    return m_template == TplStockZelda ||
           m_ext_choice == ExtOfwZelda ||
           m_bank1_choice == B1OfwZelda;
}

int ProfileWizard::MinExtSizeMiB() const
{
    return ZeldaInvolved() ? 4 : 1;
}

// While a stock template is active the Bank Assignments accordion is a
// read-only reflection of what the template will build -- keep the slot
// state mirroring it so opening the accordion always shows the truth.
void ProfileWizard::SyncStockReflection()
{
    if (m_template == TplStockMario) {
        m_bank1_choice = B1OfwMario;
        m_bank2_choice = B2Blank;
        m_ext_choice = ExtOfwMario;
        m_ext_size_mib = 64;
    } else if (m_template == TplStockZelda) {
        m_bank1_choice = B1OfwZelda;
        m_bank2_choice = B2Blank;
        m_ext_choice = ExtOfwZelda;
        m_ext_size_mib = 64;
    }
    if (m_ext_size_mib < MinExtSizeMiB()) {
        m_ext_size_mib = MinExtSizeMiB();
    }
}

/* ------------------------------------------------------------------ */
/* Firmware stage (S1a prompt / S1b status) + the game-card motif       */

static const char *kGameDisplay[2] = { "Mario", "Zelda" };

// Middle-ellipsize a path to fit max_w (full path goes in a tooltip).
static std::string EllipsizeMiddle(const std::string &s, float max_w)
{
    if (ImGui::CalcTextSize(s.c_str()).x <= max_w) {
        return s;
    }
    for (size_t keep = s.size(); keep > 6; keep--) {
        size_t head = keep / 2, tail = keep - head;
        std::string t = s.substr(0, head) + "..." + s.substr(s.size() - tail);
        if (ImGui::CalcTextSize(t.c_str()).x <= max_w) {
            return t;
        }
    }
    return "...";
}

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
    ImVec2 size(w, GNW_CARD_H * sc);
    ImVec2 end(pos.x + size.x, pos.y + size.y);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float r = GNW_RADIUS_CARD * sc;
    float pad = GNW_PAD_CARD * sc;

    ImVec4 border = GNW_COL_CARD_BORDER;
    border.w = 0.5f;
    if (st == kCardComplete) {
        border = GNW_COL_ACCENT_DIM;
        border.w *= lit;
    } else if (st == kCardPartial) {
        border = GNW_COL_WARN;
        border.w = 0.4f;
    }
    dl->AddRectFilled(pos, end, ImGui::GetColorU32(GNW_COL_CARD_BG), r);
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
    ImGui::SetCursorPosX((win_w - ImGui::CalcTextSize(title).x) * 0.5f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
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

bool ProfileWizard::DrawExtSizeStepper()
{
    bool changed = false;
    int min_mib = MinExtSizeMiB();
    if (m_ext_size_mib < min_mib) {
        m_ext_size_mib = min_mib;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Size");
    ImGui::SameLine();
    ImGui::BeginDisabled(m_ext_size_mib <= min_mib);
    if (ImGui::Button("-##extsize")) {
        m_ext_size_mib = std::max(min_mib, m_ext_size_mib / 2);
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Text("%d MiB", m_ext_size_mib);
    ImGui::SameLine();
    ImGui::BeginDisabled(m_ext_size_mib >= 256);
    if (ImGui::Button("+##extsize")) {
        m_ext_size_mib = std::min(256, m_ext_size_mib * 2);
        changed = true;
    }
    ImGui::EndDisabled();
    return changed;
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
    m_assignments_open = ImGui::CollapsingHeader("Bank Assignments");
    if (!m_assignments_open) {
        return;
    }

    // Always editable: under a stock template these show the reflected
    // stock values (synced at selection time); the first edit here
    // switches the Template to Custom, keeping every current value --
    // including the one just changed (owner call, reverses the earlier
    // read-only-reflection behavior).
    bool changed = false;

    auto slot_file = [&changed](const char *label, std::string &path) {
        ImGui::TextUnformatted(path.empty() ? "(no file)" : path.c_str());
        ImGui::SameLine();
        FilePicker(label, path.c_str(), kBinFilter, 2, false,
                   [&path](const char *p) { path = p; });
        // (dialog result lands via callback; the combo switch to File...
        // already counted as a change)
    };

    ImGui::Indent();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Bank 1");
    ImGui::SameLine(120 * g_viewport_mgr.m_scale);
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
    float patch_w = b1_show_patch
        ? ImGui::CalcTextSize("Patched").x + ImGui::GetFrameHeight() +
          3 * ImGui::GetStyle().ItemSpacing.x
        : 0.0f;
    ImGui::SetNextItemWidth(b1_show_patch ? -patch_w : -FLT_MIN);
    if (ImGui::BeginCombo("##b1", b1_items[b1_sel].label)) {
        for (int i = 0; i < b1_n; i++) {
            if (ImGui::Selectable(b1_items[i].label, i == b1_sel)) {
                if (m_bank1_choice != b1_items[i].choice) {
                    m_bank1_choice = b1_items[i].choice;
                    changed = true;
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
                        }
                    }
                }
            }
        }
        ImGui::EndCombo();
    }
    if (b1_show_patch) {
        ImGui::SameLine();
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
        slot_file("Bank1 file", m_bank1_path);
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Bank 2");
    ImGui::SameLine(120 * g_viewport_mgr.m_scale);
    const char *b2_items[] = { "Empty", "File..." };
    ImGui::SetNextItemWidth(-FLT_MIN);
    changed |= ImGui::Combo("##b2", &m_bank2_choice, b2_items, 2);
    if (m_bank2_choice == B2File) {
        slot_file("Bank2 file", m_bank2_path);
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Extflash");
    ImGui::SameLine(120 * g_viewport_mgr.m_scale);
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
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##ext", ext_items[ext_sel].label)) {
        for (int i = 0; i < ext_n; i++) {
            if (ImGui::Selectable(ext_items[i].label, i == ext_sel)) {
                if (m_ext_choice != ext_items[i].choice) {
                    m_ext_choice = ext_items[i].choice;
                    changed = true;
                }
            }
        }
        ImGui::EndCombo();
    }
    if (m_ext_choice == ExtFile) {
        slot_file("Extflash file", m_ext_path);
    } else {
        changed |= DrawExtSizeStepper();
    }

    ImGui::Unindent();

    if (changed && m_template != TplCustom) {
        m_template = TplCustom;
    }
    if (changed && m_ext_size_mib < MinExtSizeMiB()) {
        m_ext_size_mib = MinExtSizeMiB();
    }
}

void ProfileWizard::DrawForm()
{
    // Name -- single line, label + field.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Name");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##name", m_name_hint.c_str(), m_name, sizeof(m_name));

    ImGui::Spacing();
    DrawFirmwareStrip();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("Template");
    ImGui::Spacing();
    // Order (owner call): Mario, Zelda, Custom last. Stock entries are
    // hidden entirely (not disabled) until that game's dumps are
    // found+verified -- the backup-folder row above is how users make
    // them appear.
    const char *stock_names[2] = { "Stock Mario", "Stock Zelda" };
    for (int i = 0; i < 2; i++) {
        bool avail = m_library.status[i].Available() && m_library.status[i].external_found;
        if (!avail) {
            continue;
        }
        if (ImGui::RadioButton(stock_names[i], m_template == i)) {
            if (m_template != i) {
                m_close_assignments_next = true;
            }
            m_template = i;
            SyncStockReflection();
        }
    }
    if (ImGui::RadioButton("Custom", m_template == TplCustom)) {
        if (m_template != TplCustom) {
            m_open_assignments_next = true;
        }
        m_template = TplCustom;
    }

    ImGui::Spacing();
    DrawBankAssignments();
}

void ProfileWizard::DrawBuildView()
{
    static const char *kSteps[] = {
        "Creating profile", "Building bank 1", "Building bank 2",
        "Building external flash", "Finishing",
    };
    int state = m_build_state.load();
    int step = m_build_step.load();

    if (state == BuildRunning) {
        ImGui::Text("%s...", kSteps[step < 5 ? step : 4]);
        ImGui::ProgressBar((step + 1) / 5.0f, ImVec2(-1, 0));
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
                gwemu_relaunch_with_flash_images(p->Bank1Path().c_str(),
                                                 p->Bank2Path().c_str(),
                                                 p->ExtflashPath().c_str());
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
    float pad = 28 * g_viewport_mgr.m_scale;
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

    bool building = m_build_state.load() != BuildIdle;
    bool fw_stage = !building && m_stage != StageProfile;
    if (fw_stage) {
        DrawFirmwareStage(); // draws its own centered title
    } else {
        ImGui::PushFont(g_font_mgr.m_menu_font_medium);
        ImGui::TextUnformatted("New Device Profile");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 10 * g_viewport_mgr.m_scale));
        if (building) {
            DrawBuildView();
        } else {
            DrawForm();
        }
    }

    // Natural height of everything above the bottom row, BEFORE any
    // pinning -- the host resizes the OS window toward
    // DesiredHeight(), which is what makes "never scrolls, never
    // clips" hold.
    float content_bottom = ImGui::GetCursorPosY();
    float reason_h = ImGui::GetTextLineHeightWithSpacing();
    float btn_h = ImGui::GetFrameHeightWithSpacing();
    m_desired_h = content_bottom + reason_h + btn_h +
                  (28 * g_viewport_mgr.m_scale);

    // Layout signature: the discrete states that legitimately change the
    // natural height. The host resizes only when this changes -- never
    // from per-frame height deltas (which fought user drag-resizes and
    // compositor size grants, degrading the window over time).
    unsigned sig = (unsigned)m_template |
                   ((unsigned)m_assignments_open << 3) |
                   ((unsigned)(m_build_state.load() != BuildIdle) << 4) |
                   ((unsigned)m_patched << 5) |
                   ((unsigned)m_bank1_choice << 6) |
                   ((unsigned)m_bank2_choice << 9) |
                   ((unsigned)m_ext_choice << 11) |
                   ((unsigned)((int)(m_desired_h / 24)) << 16);
    // Stage + card states also change the natural layout (hash-combined
    // rather than shifted -- the height field above uses the high bits).
    sig ^= (unsigned)m_stage * 0x9E3779B9u;
    sig ^= (unsigned)GnwCardStateOf(m_library.status[0]) * 0x85EBCA6Bu;
    sig ^= (unsigned)GnwCardStateOf(m_library.status[1]) * 0xC2B2AE35u;
    if (sig != m_last_sig) {
        m_last_sig = sig;
        m_content_changed = true;
    }

    // Bottom row: Cancel (left) / Create (right), pinned to the window
    // bottom. Cancel = skip into the normal settings menu (the host
    // falls back there because WasCompleted() stays false).
    if (fw_stage) {
        // Firmware-stage bottom row: [Skip] left; right = the single
        // accent element (Select folder... on S1a, Continue on S1b once
        // at least one card is Complete).
        float sc = g_viewport_mgr.m_scale;
        float y = ImGui::GetWindowHeight() - btn_h -
                  (28 * g_viewport_mgr.m_scale);
        if (ImGui::GetCursorPosY() < y) {
            ImGui::SetCursorPosY(y);
        }
        if (ImGui::Button("Skip", ImVec2(110 * sc, 0))) {
            EnterProfileStage();
        }
        float bw = 160 * sc;
        ImGui::SameLine(ImGui::GetWindowWidth() - bw -
                        (28 * g_viewport_mgr.m_scale));
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
        float y = ImGui::GetWindowHeight() - btn_h -
                  (28 * g_viewport_mgr.m_scale);
        if (ImGui::GetCursorPosY() < y) {
            ImGui::SetCursorPosY(y);
        }
        if (ImGui::Button("Cancel", ImVec2(110 * g_viewport_mgr.m_scale, 0))) {
            is_open = false;
        }
        std::string why;
        bool can_create = ValidSources(&why);
        float create_w = 110 * g_viewport_mgr.m_scale;
        ImGui::SameLine(ImGui::GetWindowWidth() - create_w -
                        (28 * g_viewport_mgr.m_scale));
        ImGui::BeginDisabled(!can_create);
        if (ImGui::Button("Create", ImVec2(create_w, 0))) {
            StartBuild();
        }
        ImGui::EndDisabled();
        if (!can_create) {
            // Reason, subtle, right-aligned above the button row.
            ImVec2 sz = ImGui::CalcTextSize(why.c_str());
            ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - sz.x -
                                           (28 * g_viewport_mgr.m_scale),
                                       y - ImGui::GetTextLineHeightWithSpacing()));
            ImGui::TextDisabled("%s", why.c_str());
        }
    }

    ImGui::End();
}
