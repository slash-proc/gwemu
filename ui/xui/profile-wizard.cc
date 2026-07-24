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
#include "gwemu-hud.h"
#include "../gwemu-profiles.hh"

#include <glib.h>
#include <glib/gstdio.h>
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

// gnwmanager's pre-built novel-code binary -- same repo-layout
// convention the CLI tool documents (checkout next to this repo).
static std::string PatchBinaryPath(const char *game)
{
    return std::string("../gnwmanager/gnwmanager/cli/gnw_patch/binaries/") +
           game + "/0x08032000.bin";
}

ProfileWizard::ProfileWizard() = default;

ProfileWizard::~ProfileWizard()
{
    JoinWorker();
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
    m_page = PageTemplate;
    m_build_state.store(BuildIdle);
    m_created_id.clear();
    m_name_hint = GwProfileStore::GenerateName();
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
    if (m_template == TplStockMario || m_template == TplStockZelda) {
        int gi = m_template == TplStockMario ? 0 : 1;
        const char *game = GnwBackupLibrary::GameName(gi);
        if (m_stock_patched) {
            // Patched OFW: gnw_cfw_build writes bank1+extflash directly
            // into the profile (in-process -- no popen, see Phase 1).
            m_build_step.store(1);
            char *cerr = NULL;
            ok = gnw_cfw_build_images(game,
                                      m_library.InternalPath(gi).c_str(),
                                      m_library.ExternalPath(gi).c_str(),
                                      PatchBinaryPath(game).c_str(),
                                      p->Bank1Path().c_str(),
                                      p->ExtflashPath().c_str(),
                                      NULL, &cerr);
            if (!ok) {
                err = cerr ? cerr : "CFW patch failed";
                free(cerr);
            }
            p->prov_bank1 = std::string("patched-") + game;
            p->prov_extflash = std::string("patched-") + game;
        } else {
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
    m_page = PageBuild;
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
    if (m_template == TplStockMario || m_template == TplStockZelda) {
        int gi = m_template == TplStockMario ? 0 : 1;
        if (!m_library.status[gi].Available() || !m_library.status[gi].external_found) {
            if (why) *why = "verified OFW dumps not found in the backup folder";
            return false;
        }
        if (m_stock_patched &&
            !g_file_test(PatchBinaryPath(GnwBackupLibrary::GameName(gi)).c_str(),
                         G_FILE_TEST_EXISTS)) {
            if (why) *why = "gnwmanager patch binary not found (../gnwmanager checkout)";
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
    return true;
}

void ProfileWizard::DrawTemplatePage()
{
    ImGui::TextUnformatted("Template");
    ImGui::Spacing();

    const char *stock_names[2] = { "Stock Mario", "Stock Zelda" };
    for (int i = 0; i < 2; i++) {
        bool avail = m_library.status[i].Available() && m_library.status[i].external_found;
        ImGui::BeginDisabled(!avail);
        if (ImGui::RadioButton(stock_names[i], m_template == i)) {
            m_template = i;
        }
        ImGui::EndDisabled();
        if (!avail) {
            ImGui::SameLine();
            ImGui::TextDisabled("(needs verified dumps in the backup folder)");
        }
    }
    if (ImGui::RadioButton("Custom", m_template == TplCustom)) {
        m_template = TplCustom;
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Name (optional)");
    ImGui::SetNextItemWidth(240 * g_viewport_mgr.m_scale);
    ImGui::InputTextWithHint("##name", m_name_hint.c_str(), m_name, sizeof(m_name));
}

void ProfileWizard::DrawSourcesPage()
{
    if (m_template == TplStockMario || m_template == TplStockZelda) {
        int gi = m_template == TplStockMario ? 0 : 1;
        ImGui::Text("Stock %s from your verified dumps.", GnwBackupLibrary::GameName(gi));
        ImGui::Spacing();
        ImGui::Checkbox("Patched OFW (retro-go dual-boot hotkey)", &m_stock_patched);
        if (m_stock_patched &&
            !g_file_test(PatchBinaryPath(GnwBackupLibrary::GameName(gi)).c_str(),
                         G_FILE_TEST_EXISTS)) {
            ImGui::TextDisabled("gnwmanager patch binary not found -- needs a ../gnwmanager checkout");
        }
        return;
    }

    auto slot_file = [](const char *label, std::string &path) {
        ImGui::TextUnformatted(path.empty() ? "(no file)" : path.c_str());
        ImGui::SameLine();
        FilePicker(label, path.c_str(), kBinFilter, 2, false,
                   [&path](const char *p) { path = p; });
    };

    ImGui::TextUnformatted("Bank 1 (internal flash)");
    const char *b1_items[] = { "Blank (0xFF)", "Mario OFW", "Zelda OFW", "File..." };
    ImGui::SetNextItemWidth(200 * g_viewport_mgr.m_scale);
    ImGui::Combo("##b1", &m_bank1_choice, b1_items, 4);
    if (m_bank1_choice == B1File) {
        slot_file("Bank1 file", m_bank1_path);
    }
    ImGui::Spacing();

    ImGui::TextUnformatted("Bank 2 (internal flash)");
    const char *b2_items[] = { "Blank (0xFF)", "File..." };
    ImGui::SetNextItemWidth(200 * g_viewport_mgr.m_scale);
    ImGui::Combo("##b2", &m_bank2_choice, b2_items, 2);
    if (m_bank2_choice == B2File) {
        slot_file("Bank2 file", m_bank2_path);
    }
    ImGui::Spacing();

    ImGui::TextUnformatted("External flash");
    const char *ext_items[] = { "Blank (0xFF)", "Blank + Mario OFW assets",
                                "Blank + Zelda OFW assets", "File..." };
    ImGui::SetNextItemWidth(240 * g_viewport_mgr.m_scale);
    ImGui::Combo("##ext", &m_ext_choice, ext_items, 4);
    if (m_ext_choice == ExtFile) {
        slot_file("Extflash file", m_ext_path);
    } else {
        ImGui::SetNextItemWidth(120 * g_viewport_mgr.m_scale);
        ImGui::InputInt("Size (MiB)", &m_ext_size_mib);
        if (m_ext_size_mib < 1) m_ext_size_mib = 1;
        if (m_ext_size_mib > 256) m_ext_size_mib = 256;
    }
}

void ProfileWizard::DrawBuildPage()
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
            is_open = false;
        }
    } else if (state == BuildFailed) {
        JoinWorker();
        ImGui::TextUnformatted("Profile creation failed:");
        ImGui::TextWrapped("%s", m_build_error.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Back")) {
            m_build_state.store(BuildIdle);
            m_page = PageSources;
        }
    }
}

void ProfileWizard::Draw()
{
    if (!is_open) {
        return;
    }

    ImVec2 size(460 * g_viewport_mgr.m_scale, 340 * g_viewport_mgr.m_scale);
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - size.x) / 2,
                                   (io.DisplaySize.y - size.y) / 2),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
    if (!ImGui::Begin("New Device Profile", &is_open,
                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    ImGui::PushFont(g_font_mgr.m_menu_font_medium);
    const char *title = "New Device Profile";
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(title).x) / 2);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 8 * g_viewport_mgr.m_scale));

    switch (m_page) {
    case PageTemplate: DrawTemplatePage(); break;
    case PageSources:  DrawSourcesPage();  break;
    case PageBuild:    DrawBuildPage();    break;
    }

    ImGui::Dummy(ImVec2(0, 10 * g_viewport_mgr.m_scale));

    if (m_page != PageBuild) {
        if (m_page > PageTemplate && ImGui::Button("Back")) {
            m_page--;
        }
        if (m_page > PageTemplate) {
            ImGui::SameLine();
        }
        std::string why;
        bool can_next = m_page == PageTemplate || ValidSources(&why);
        ImGui::BeginDisabled(!can_next);
        if (ImGui::Button(m_page == PageSources ? "Create" : "Next")) {
            if (m_page == PageTemplate) {
                m_page = PageSources;
            } else {
                StartBuild();
            }
        }
        ImGui::EndDisabled();
        if (!can_next && m_page == PageSources) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", why.c_str());
        }
    }

    ImGui::End();
}
