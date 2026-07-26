//
// gnw-h7b0 User Interface -- SD Card tab, see sdcard-view.hh.
//
#include "common.hh"
#include "main-menu.hh"
#include "font-manager.hh"
#include "gnw-style-tokens.hh"
#include "misc.hh"
#include "viewport-manager.hh"
#include "widgets.hh"
#include "../gwemu-profiles.hh"
#include "../gwemu-sdcreate.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>

// Card-size presets. Every entry is a power of two >= 64 MiB, which is
// what gnw_sdimg_build() (and behind it QEMU's hw/sd/sd.c) requires -- so
// no preset can ever produce an image the emulated card rejects.
struct GnwSdSize {
    uint64_t bytes;
    const char *label;
};
static const GnwSdSize kSizes[] = {
    {   64ull << 20, "64 MiB"  }, {  128ull << 20, "128 MiB" },
    {  256ull << 20, "256 MiB" }, {  512ull << 20, "512 MiB" },
    {    1ull << 30, "1 GiB"   }, {    2ull << 30, "2 GiB"   },
    {    4ull << 30, "4 GiB"   }, {    8ull << 30, "8 GiB"   },
    {   16ull << 30, "16 GiB"  }, {   32ull << 30, "32 GiB"  },
};
static const int kNumSizes = (int)(sizeof(kSizes) / sizeof(kSizes[0]));

static std::string HumanBytes(uint64_t b)
{
    static const char *unit[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)b;
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        u++;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), (u == 0 || v >= 100.0) ? "%.0f %s" : "%.1f %s",
             v, unit[u]);
    return buf;
}

// ---------------------------------------------------------------------
// Capacity math -- mirrors contrib/gnw-tools/gnw_fat32.c exactly (its
// fatgen103 cluster table and FAT-size formula) so the "will it fit?"
// answer shown before the build is the same arithmetic the build itself
// will do, not an independent guess that can disagree with it.
// ---------------------------------------------------------------------
static uint32_t PickSpc(uint64_t volume_sectors)
{
    if (volume_sectors <= 66600)    return 0;   // too small for FAT32
    if (volume_sectors <= 532480)   return 1;
    if (volume_sectors <= 16777216) return 8;   // up to 8 GiB: 4 KiB
    if (volume_sectors <= 33554432) return 16;  // up to 16 GiB: 8 KiB
    if (volume_sectors <= 67108864) return 32;  // up to 32 GiB: 16 KiB
    return 64;                                  // above 32 GiB: 32 KiB
}

// Bytes actually available for file data, and the cluster size in use.
static uint64_t UsableBytes(uint64_t card_bytes, uint64_t *cluster_bytes)
{
    const uint64_t kPartitionStartLba = 2048;   // gnw_sdimg.c
    const uint64_t kRsvdSectors = 32, kNumFats = 2;

    if (card_bytes <= kPartitionStartLba * 512) {
        return 0;
    }
    uint64_t vol = card_bytes / 512 - kPartitionStartLba;
    uint32_t spc = PickSpc(vol);
    if (spc == 0) {
        return 0;
    }
    if (cluster_bytes) {
        *cluster_bytes = (uint64_t)spc * 512;
    }
    uint64_t tmp2 = ((256 * (uint64_t)spc) + kNumFats) / 2;
    uint64_t fat_sectors = (vol - kRsvdSectors + tmp2 - 1) / tmp2;
    uint64_t data_start = kRsvdSectors + kNumFats * fat_sectors;
    if (vol <= data_start) {
        return 0;
    }
    return ((vol - data_start) / spc) * spc * 512;
}

// ---------------------------------------------------------------------
// Workers
// ---------------------------------------------------------------------
GnwSdCardView::~GnwSdCardView()
{
    m_scan_cancel.store(true);
    if (m_scan_worker.joinable()) m_scan_worker.join();
    if (m_build_worker.joinable()) m_build_worker.join();
}

// Recursive size/count walk. Runs on the scan worker only.
static void ScanDir(const std::string &path, std::atomic<uint64_t> *bytes,
                    std::atomic<uint64_t> *files, std::atomic<uint64_t> *dirs,
                    std::atomic<bool> *cancel, std::string *err)
{
    if (cancel->load() || !err->empty()) {
        return;
    }
    GError *gerr = nullptr;
    GDir *d = g_dir_open(path.c_str(), 0, &gerr);
    if (!d) {
        *err = gerr ? gerr->message : "cannot open folder";
        if (gerr) g_error_free(gerr);
        return;
    }
    const gchar *name;
    while ((name = g_dir_read_name(d)) != nullptr) {
        if (cancel->load()) break;
        std::string child = path + G_DIR_SEPARATOR_S + name;
        GStatBuf st;
        if (g_stat(child.c_str(), &st) != 0) {
            continue;   // vanished or unreadable -- the builder skips it too
        }
        if (S_ISDIR(st.st_mode)) {
            dirs->fetch_add(1);
            ScanDir(child, bytes, files, dirs, cancel, err);
        } else if (S_ISREG(st.st_mode)) {
            files->fetch_add(1);
            bytes->fetch_add((uint64_t)st.st_size);
        }
    }
    g_dir_close(d);
}

void GnwSdCardView::StartScan()
{
    m_scan_cancel.store(true);
    if (m_scan_worker.joinable()) {
        m_scan_worker.join();
    }
    m_scan_cancel.store(false);
    m_scan_bytes.store(0);
    m_scan_files.store(0);
    m_scan_dirs.store(0);
    {
        std::lock_guard<std::mutex> lock(m_scan_mutex);
        m_scan_error.clear();
    }
    if (m_content_dir.empty()) {
        m_scan_state.store(JobState::Idle);
        return;
    }

    m_scan_state.store(JobState::Running);
    std::string dir = m_content_dir;
    m_scan_worker = std::thread([this, dir]() {
        std::string err;
        if (!g_file_test(dir.c_str(), G_FILE_TEST_IS_DIR)) {
            err = "not a folder: " + dir;
        } else {
            ScanDir(dir, &m_scan_bytes, &m_scan_files, &m_scan_dirs,
                    &m_scan_cancel, &err);
        }
        std::lock_guard<std::mutex> lock(m_scan_mutex);
        m_scan_error = err;
        m_scan_state.store(err.empty() ? JobState::Done : JobState::Failed);
    });
}

void GnwSdCardView::SetContentDir(const char *path)
{
    m_content_dir = path ? path : "";
    gwemu_settings_set_string(&g_config.general.sdcard.content_dir,
                              m_content_dir.c_str());
    // Saved immediately, never at exit: the Flash tab's Apply restarts the
    // process with execv(), which does not run atexit handlers.
    gwemu_settings_save();

    // Seed the card name from the folder once, so the common case is a
    // single click away. A name the user has already typed is left alone.
    if (m_name_buf[0] == '\0' && !m_content_dir.empty()) {
        char *base = g_path_get_basename(m_content_dir.c_str());
        if (base) {
            g_strlcpy(m_name_buf, base, sizeof(m_name_buf));
            g_free(base);
        }
    }
    StartScan();
}

void GnwSdCardView::StartBuild()
{
    if (m_build_worker.joinable()) {
        m_build_worker.join();   // previous run already Done/Failed
    }

    // Registry filename: sanitized card name, deduped by numeric suffix,
    // exactly the convention profile-wizard.cc uses for shared cards.
    std::string stem = m_name_buf[0] ? m_name_buf : "sd-card";
    for (char &c : stem) {
        if (c == '/' || c == '\\' || c == ':') {
            c = '_';
        }
    }
    std::string root = GwProfileStore::SdCardsRoot();
    g_mkdir_with_parents(root.c_str(), 0755);
    std::string image = stem + ".qcow2";
    for (int n = 2; g_file_test((root + "/" + image).c_str(), G_FILE_TEST_EXISTS);
         n++) {
        image = stem + "-" + std::to_string(n) + ".qcow2";
    }
    std::string dst = root + "/" + image;

    uint64_t card_bytes = kSizes[m_size_choice].bytes;
    std::string content = m_content_dir;

    m_build_bytes.store(0);
    // Denominator for the progress bar: the scanned content size. The
    // builder's write callback reports bytes actually written (all-zero
    // writes are dropped), which for a populated card is the file data
    // plus a little FAT/directory metadata -- so this is close, and the
    // bar is clamped rather than allowed to overshoot.
    m_build_expect.store(m_scan_state.load() == JobState::Done
                             ? m_scan_bytes.load() : 0);
    {
        std::lock_guard<std::mutex> lock(m_build_mutex);
        m_build_message.clear();
        m_build_path = dst;
        m_build_image = image;
    }
    m_build_state.store(JobState::Running);

    m_build_worker = std::thread([this, dst, image, content, card_bytes]() {
        char *cerr = nullptr;
        bool ok = gwemu_sdcreate_qcow2_progress(
            dst.c_str(), card_bytes, content.empty() ? nullptr : content.c_str(),
            [](void *opaque, uint64_t written) {
                ((GnwSdCardView *)opaque)->m_build_bytes.store(written);
            },
            this, &cerr);

        if (ok) {
            // shareable=true sidecar -- this is what makes the card appear
            // in the profile wizard's "Shared" picker.
            std::string sidecar = dst.substr(0, dst.size() - 6) + ".toml";
            FILE *f = g_fopen(sidecar.c_str(), "wb");
            if (f) {
                fprintf(f, "shareable = true\ndisplay_name = \"%s\"\n",
                        image.c_str());
                fclose(f);
            } else {
                ok = false;
                free(cerr);
                cerr = strdup(("cannot create " + sidecar).c_str());
            }
        }

        std::lock_guard<std::mutex> lock(m_build_mutex);
        m_build_message = ok ? "" : (cerr ? cerr : "SD card creation failed");
        free(cerr);
        m_build_state.store(ok ? JobState::Done : JobState::Failed);
    });
}

void GnwSdCardView::JoinFinishedWorkers()
{
    JobState s = m_scan_state.load();
    if ((s == JobState::Done || s == JobState::Failed) &&
        m_scan_worker.joinable()) {
        m_scan_worker.join();   // already returned: instant
    }

    s = m_build_state.load();
    if ((s == JobState::Done || s == JobState::Failed) &&
        m_build_worker.joinable()) {
        m_build_worker.join();

        if (s == JobState::Done) {
            // Registry/profile bookkeeping happens here, on the render
            // thread, not in the worker -- g_profile_store is UI-thread
            // state.
            std::string image;
            {
                std::lock_guard<std::mutex> lock(m_build_mutex);
                image = m_build_image;
            }
            g_profile_store.Scan();
            if (m_attach_to_active) {
                GwProfile *p =
                    g_profile_store.Find(g_config.general.active_profile);
                if (p) {
                    p->sd.mode = GwSdMode::Shared;
                    p->sd.image = image;
                    std::string err;
                    if (!g_profile_store.Save(*p, err)) {
                        std::lock_guard<std::mutex> lock(m_build_mutex);
                        m_build_message = "card built, but attaching it to the "
                                          "active profile failed: " + err;
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------
// Fit check
// ---------------------------------------------------------------------
GnwSdCardView::FitInfo GnwSdCardView::ComputeFit() const
{
    FitInfo f{};
    f.card_bytes = kSizes[m_size_choice].bytes;
    uint64_t cluster = 4096;
    f.usable_bytes = UsableBytes(f.card_bytes, &cluster);
    f.known = (m_scan_state.load() == JobState::Done);
    if (f.known) {
        // Worst case: every file and subdirectory wastes up to one cluster
        // of slack. Deliberately conservative -- an over-estimate turns
        // into "pick the next size up", an under-estimate into a build
        // that runs for minutes and then fails.
        f.needed_bytes = m_scan_bytes.load() +
                         (m_scan_files.load() + m_scan_dirs.load() + 1) * cluster;
    }
    f.fits = !f.known || f.needed_bytes <= f.usable_bytes;
    return f;
}

// ---------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------
void GnwSdCardView::DrawFolderCard()
{
    float sc = g_viewport_mgr.m_scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float h = GNW_CARD_H * sc;
    ImVec2 end(pos.x + w, pos.y + h);
    float pad = GNW_PAD_CARD * sc;

    JobState scan = m_scan_state.load();
    bool have = !m_content_dir.empty();
    ImVec4 border = GNW_COL_CARD_BORDER;
    if (scan == JobState::Failed) {
        border = GNW_COL_ERR;
    } else if (have && scan == JobState::Done) {
        border = GNW_COL_ACCENT_DIM;
    }

    dl->AddRectFilled(pos, end, ImGui::GetColorU32(GNW_COL_CARD_BG),
                      GNW_RADIUS_CARD * sc);
    if (have && scan == JobState::Done) {
        dl->AddRectFilled(pos, end, ImGui::GetColorU32(GNW_COL_ACCENT_WASH),
                          GNW_RADIUS_CARD * sc);
    }
    dl->AddRect(pos, end, ImGui::GetColorU32(border), GNW_RADIUS_CARD * sc);

    // Right-hand button first, so the text column knows what width it has.
    const char *btn = have ? "Change..." : "Choose folder...";
    float btn_w = ImGui::CalcTextSize(btn).x + 2 * ImGui::GetStyle().FramePadding.x;
    ImGui::SetCursorScreenPos(ImVec2(end.x - pad - btn_w,
                                     pos.y + (h - ImGui::GetFrameHeight()) / 2));
    ImGui::PushStyleColor(ImGuiCol_Button, GNW_COL_ACCENT);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GNW_COL_ACCENT_HOVER);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, GNW_COL_ACCENT_ACTIVE);
    if (ImGui::Button(btn)) {
        ShowOpenFolderDialog(m_content_dir.c_str(),
                             [this](const char *p) { SetContentDir(p); });
    }
    ImGui::PopStyleColor(3);

    float text_w = (end.x - pad - btn_w) - (pos.x + pad) - 10 * sc;
    float line_h = ImGui::GetTextLineHeight();
    ImVec2 l1(pos.x + pad, pos.y + h / 2 - line_h - 2 * sc);
    ImVec2 l2(pos.x + pad, pos.y + h / 2 + 2 * sc);

    std::string title = std::string(ICON_FA_FOLDER "  ") +
                        (have ? EllipsizeMiddle(m_content_dir, text_w -
                                    ImGui::CalcTextSize(ICON_FA_FOLDER "  ").x)
                              : "No folder selected");
    dl->AddText(l1, ImGui::GetColorU32(have ? ImGuiCol_Text : ImGuiCol_TextDisabled),
                title.c_str());

    std::string sub;
    ImVec4 sub_col = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
    if (!have) {
        sub = "Its contents become the root of the card -- point it at "
              "retro-go's sd_content/.";
    } else if (scan == JobState::Running) {
        sub = "Measuring contents...";
    } else if (scan == JobState::Failed) {
        std::lock_guard<std::mutex> lock(m_scan_mutex);
        sub = std::string(ICON_FA_XMARK " ") + m_scan_error;
        sub_col = GNW_COL_ERR;
    } else if (scan == JobState::Done) {
        char buf[192];
        snprintf(buf, sizeof(buf), "%llu files in %llu folders  --  %s",
                 (unsigned long long)m_scan_files.load(),
                 (unsigned long long)m_scan_dirs.load(),
                 HumanBytes(m_scan_bytes.load()).c_str());
        sub = buf;
    }
    dl->AddText(l2, ImGui::GetColorU32(sub_col),
                EllipsizeMiddle(sub, text_w).c_str());

    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h));
    ImGui::Dummy(ImVec2(w, 0));
}

void GnwSdCardView::DrawSizeRow(const FitInfo &fit)
{
    float sc = g_viewport_mgr.m_scale;
    bool busy = (m_build_state.load() == JobState::Running);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Card size");
    ImGui::SameLine(120 * sc);

    ImGui::BeginDisabled(busy || m_size_choice <= 0);
    if (ImGui::Button("-##sdsize")) {
        m_size_choice--;
        g_config.general.sdcard.size_index = m_size_choice;
        gwemu_settings_save();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextUnformatted(kSizes[m_size_choice].label);
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || m_size_choice >= kNumSizes - 1);
    if (ImGui::Button("+##sdsize")) {
        m_size_choice++;
        g_config.general.sdcard.size_index = m_size_choice;
        gwemu_settings_save();
    }
    ImGui::EndDisabled();

    ImGui::SameLine(0, GNW_GAP_GUTTER * sc);
    if (!fit.known) {
        ImGui::TextDisabled("%s usable", HumanBytes(fit.usable_bytes).c_str());
    } else if (fit.fits) {
        char buf[192];
        snprintf(buf, sizeof(buf), ICON_FA_CHECK " Fits -- %s free of %s",
                 HumanBytes(fit.usable_bytes - fit.needed_bytes).c_str(),
                 HumanBytes(fit.usable_bytes).c_str());
        ImGui::TextColored(GNW_COL_ACCENT, "%s", buf);
    } else {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 ICON_FA_XMARK " Too small -- needs %s more than this card holds",
                 HumanBytes(fit.needed_bytes - fit.usable_bytes).c_str());
        ImGui::TextColored(GNW_COL_ERR, "%s", buf);
    }
}

void GnwSdCardView::DrawBuildRow(const FitInfo &fit)
{
    float sc = g_viewport_mgr.m_scale;
    JobState build = m_build_state.load();
    JobState scan = m_scan_state.load();
    bool running = (build == JobState::Running);

    // One place decides whether Build is pressable, and why not.
    const char *blocked = nullptr;
    if (running) {
        blocked = "A card is being built.";
    } else if (m_content_dir.empty()) {
        blocked = "Choose a content folder first.";
    } else if (scan == JobState::Running) {
        blocked = "Still measuring the folder's contents.";
    } else if (scan == JobState::Failed) {
        blocked = "That folder could not be read.";
    } else if (!fit.fits) {
        blocked = "The contents do not fit -- pick a larger card size.";
    }

    ImGui::BeginDisabled(blocked != nullptr);
    ImGui::PushStyleColor(ImGuiCol_Button, GNW_COL_ACCENT);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GNW_COL_ACCENT_HOVER);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, GNW_COL_ACCENT_ACTIVE);
    bool pressed = ImGui::Button("Build SD Card", ImVec2(160 * sc, 0));
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    if (blocked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", blocked);
    }
    if (pressed) {
        StartBuild();
    }

    ImGui::SameLine(0, GNW_GAP_GUTTER * sc);
    ImGui::BeginDisabled(running);
    ImGui::Checkbox("Attach to the active profile", &m_attach_to_active);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Point the currently selected profile at the new "
                          "card. Takes effect the next time that profile is "
                          "launched.");
    }

    if (blocked && !running) {
        ImGui::TextDisabled("%s", blocked);
    }

    if (running) {
        uint64_t done = m_build_bytes.load(), expect = m_build_expect.load();
        char overlay[128];
        if (expect > 0) {
            float frac = (float)((double)done / (double)expect);
            frac = std::min(frac, 0.99f);
            snprintf(overlay, sizeof(overlay), "%s of %s",
                     HumanBytes(done).c_str(), HumanBytes(expect).c_str());
            ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 0), overlay);
        } else {
            snprintf(overlay, sizeof(overlay), "%s written",
                     HumanBytes(done).c_str());
            // Nothing to divide by (blank card): an indeterminate-looking
            // bar with a live byte count still shows it is alive.
            ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(),
                               ImVec2(-FLT_MIN, 0), overlay);
        }
        return;
    }

    std::string message, path;
    {
        std::lock_guard<std::mutex> lock(m_build_mutex);
        message = m_build_message;
        path = m_build_path;
    }
    if (build == JobState::Done) {
        ImGui::Spacing();
        if (message.empty()) {
            ImGui::TextColored(GNW_COL_ACCENT, ICON_FA_CHECK " Card built.");
        } else {
            // Built, but the follow-up bookkeeping complained.
            ImGui::TextColored(GNW_COL_WARN, ICON_FA_CIRCLE_INFO " %s",
                               message.c_str());
        }
        ImGui::TextDisabled("%s", path.c_str());
        ImGui::TextDisabled("Attach it to any profile from the Profiles tab's "
                            "shared-card picker.");
    } else if (build == JobState::Failed) {
        ImGui::Spacing();
        ImGui::TextColored(GNW_COL_ERR, ICON_FA_XMARK " Could not build the card.");
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("%s", message.c_str());
        ImGui::PopStyleColor();
    }
}

void GnwSdCardView::Draw()
{
    float sc = g_viewport_mgr.m_scale;
    JoinFinishedWorkers();

    // One-time adoption of the persisted settings (the tab is a long-lived
    // member of MainMenuScene, so this runs once per process).
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        m_size_choice = std::clamp(g_config.general.sdcard.size_index, 0,
                                   kNumSizes - 1);
        if (g_config.general.sdcard.content_dir &&
            g_config.general.sdcard.content_dir[0]) {
            m_content_dir = g_config.general.sdcard.content_dir;
            char *base = g_path_get_basename(m_content_dir.c_str());
            if (base) {
                g_strlcpy(m_name_buf, base, sizeof(m_name_buf));
                g_free(base);
            }
            StartScan();
        }
    }

    SectionTitle("Content");
    ImGui::TextDisabled("Builds a FAT32 card image whose root is the contents "
                        "of the folder you pick.");
    ImGui::Dummy(ImVec2(0, 6 * sc));
    DrawFolderCard();

    // Once a scan lands, never leave the user staring at a card that
    // cannot hold what they chose: step up to the smallest preset that
    // fits. Never steps down -- a deliberately oversized card is a valid
    // choice.
    if (m_scan_state.load() == JobState::Done) {
        while (m_size_choice < kNumSizes - 1 && !ComputeFit().fits) {
            m_size_choice++;
        }
    }
    FitInfo fit = ComputeFit();

    ImGui::Dummy(ImVec2(0, GNW_GAP_SECTION * sc));
    SectionTitle("Card");
    DrawSizeRow(fit);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Name");
    ImGui::SameLine(120 * sc);
    ImGui::BeginDisabled(m_build_state.load() == JobState::Running);
    ImGui::SetNextItemWidth(280 * sc);
    ImGui::InputTextWithHint("##sdname", "sd-card", m_name_buf,
                             sizeof(m_name_buf));
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Stored in the shared-card library.");

    ImGui::Dummy(ImVec2(0, GNW_GAP_SECTION * sc));
    DrawBuildRow(fit);
}
