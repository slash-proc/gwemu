//
// gnw-h7b0 User Interface -- Flash tab (Phase 2, presets iteration)
//
#include "flash-storage-view.hh"
#include "flash-backups.hh"
#include "widgets.hh"
#include "misc.hh"
#include <glib.h>
#include <glib/gstdio.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <inttypes.h>

extern "C" {
#include "../../contrib/gnw-tools/gnw_boot_images.h"
#include "../../contrib/gnw-tools/gnw_cfw_build.h"
}
#include "gwemu-hud.h"
#include "common.hh"
#include "../gwemu-profiles.hh"
#include "../gwemu-settings.h"
#include "hw/misc/gnw_h7b0_nor.h"

static const char *kGameNames[2] = { "mario", "zelda" };

static const GnwNorChip *SelectedNorChip(const std::string &key)
{
    const GnwNorChip *chip = gnw_nor_chip_by_key(key.c_str());
    return chip ? chip : gnw_nor_chip_by_key(GNW_NOR_DEFAULT_CHIP_KEY);
}

GnwFlashStorageView::GnwFlashStorageView()
{
    m_backup_dir = g_config.general.firmware_backup_dir &&
                   g_config.general.firmware_backup_dir[0]
                       ? g_config.general.firmware_backup_dir
                       : "backup";
    if (g_config.general.extflash_chip && g_config.general.extflash_chip[0]) {
        m_nor_chip_key = g_config.general.extflash_chip;
    }
    if (g_config.general.active_profile && g_config.general.active_profile[0]) {
        GwProfile *profile = g_profile_store.Find(g_config.general.active_profile);
        if (profile) m_nor_chip_key = profile->nor_chip;
    }
    RescanBackupDir();
}

GnwFlashStorageView::~GnwFlashStorageView()
{
    for (auto &t : m_size_threads) {
        if (t.joinable()) t.join();
    }
}

void GnwFlashStorageView::RescanBackupDir()
{
    // Shared scanner (flash-backups.cc) -- also used by the profile wizard.
    m_library.dir = m_backup_dir;
    m_library.Rescan();
    for (int i = 0; i < 2; i++) {
        m_status[i] = m_library.status[i];
    }

    // A preset/selection may no longer be valid after a rescan.
    if ((m_preset == kPresetStockMario) && !m_status[0].Available()) m_preset = kPresetCustom;
    if ((m_preset == kPresetStockZelda) && !m_status[1].Available()) m_preset = kPresetCustom;
    if ((m_preset == kPresetMarioRetroGo) && !m_status[0].Available()) m_preset = kPresetCustom;
    if ((m_preset == kPresetZeldaRetroGo) && !m_status[1].Available()) m_preset = kPresetCustom;
    ApplyPreset(m_preset);
}

// Sets bank1/bank2/extflash state for a given preset. Custom leaves
// whatever the user already has configured untouched (it's the "do it
// yourself" option, nothing to auto-set).
void GnwFlashStorageView::ApplyPreset(int preset)
{
    m_preset = preset;
    switch (preset) {
    case kPresetStockMario:
        m_bank[0] = GnwBankSlot{}; m_bank[0].choice = 1; m_bank[0].patch = false;
        m_bank[1] = GnwBankSlot{};
        break;
    case kPresetStockZelda:
        m_bank[0] = GnwBankSlot{}; m_bank[0].choice = 2; m_bank[0].patch = false;
        m_bank[1] = GnwBankSlot{};
        break;
    case kPresetMarioRetroGo:
        m_bank[0] = GnwBankSlot{}; m_bank[0].choice = 1; m_bank[0].patch = true; m_bank[0].bootloader = true;
        m_bank[1] = GnwBankSlot{}; m_bank[1].choice = 3; // Browse -- user must still pick a file
        m_ofw_at_zero = true;
        break;
    case kPresetZeldaRetroGo:
        m_bank[0] = GnwBankSlot{}; m_bank[0].choice = 2; m_bank[0].patch = true; m_bank[0].bootloader = true;
        m_bank[1] = GnwBankSlot{}; m_bank[1].choice = 3;
        m_ofw_at_zero = true;
        break;
    case kPresetCustom:
    default:
        m_ofw_at_zero = false;
        break;
    }
    SyncImplicitExtflashRow();
}

uint32_t GnwFlashStorageView::ExtflashSizeBytes() const
{
    return SelectedNorChip(m_nor_chip_key)->size_bytes;
}

uint64_t GnwFlashStorageView::RequiredExtflashBytes() const
{
    uint64_t required = 0;
    if (m_bank[0].choice >= 1 && m_bank[0].choice <= 2) {
        int idx = m_bank[0].choice - 1;
        std::string path = m_backup_dir + "/flash_backup_" + kGameNames[idx] + ".bin";
        GStatBuf st;
        if (g_stat(path.c_str(), &st) == 0 && st.st_size > 0) {
            required = st.st_size;
        }
        if (m_bank[0].patch) {
            uint32_t patched = m_patched_size_cache[idx].load();
            required = std::max<uint64_t>(required, patched);
        }
    }
    for (const auto &row : m_extflash_rows) {
        required = std::max(required, (uint64_t)row.offset + row.size);
    }
    return required;
}

void GnwFlashStorageView::ResolveExtflashRowSizes()
{
    for (auto &row : m_extflash_rows) {
        if (row.implicit) {
            // Size is set for real from gnw-make-cfw-images's own stdout
            // (parsed in ApplyBank1) the moment Apply actually runs; before
            // that we don't know the real used-length yet, so leave it at 0
            // -- the geometry bar treats a 0-size implicit row as "not yet
            // applied" rather than drawing a fake-looking segment.
            continue;
        }
        GStatBuf st;
        if (g_stat(row.path.c_str(), &st) == 0 && st.st_size > 0) {
            row.size = (uint32_t)st.st_size;
        } else {
            row.size = 0;
        }
    }
}

// Runs gnw-make-cfw-images for `game` (if not already cached) purely to
// learn its real output byte size, so the geometry bar can show a real
// proportional segment for the "OFW @ 0" row immediately -- not deferred
// until the user clicks Apply. Same shell-out approach ApplyBank1() uses
// (see its own comment on why this isn't a linked library call yet); this
// duplicates that one popen()+parse, not the whole Apply pipeline (no
// gnw_make_boot_images call, no blob splicing -- this is a size probe,
// not a real Apply). gnw-make-cfw-images always uses bootloader=true, so
// caching per-game (not per-setting) is safe -- there is no other input
// that changes its output size for a given game today.
//
// Non-blocking: kicks off a background thread if not already cached/
// running, and returns whatever's cached right now (0 if the computation
// hasn't finished yet -- callers already treat 0 as "not ready", see the
// header comment on m_patched_size_cache). Popen()-ing this inline used to
// block the render thread for the patch op's runtime; short for a single
// game, but the same freeze-prone pattern as the SD card build bug, fixed
// the same way for consistency.
uint32_t GnwFlashStorageView::GetOrComputePatchedExtflashSize(const char *game)
{
    int idx = (strcmp(game, "mario") == 0) ? 0 : 1;
    uint32_t cached = m_patched_size_cache[idx].load();
    if (cached != 0) return cached;
    if (m_size_compute_running[idx].load()) return 0;

    if (m_size_threads[idx].joinable()) m_size_threads[idx].join(); // prior run, already finished

    m_size_compute_running[idx] = true;
    std::string game_str = game;
    /* Probe the complete supported range so the cached used length can
     * constrain every chip choice without rerunning the patch per choice. */
    uint64_t capacity_mib = 256;
    m_size_threads[idx] = std::thread([this, idx, game_str, capacity_mib]() {
        std::string cmd = "./build/contrib/gnw-tools/gnw-make-cfw-images ";
        cmd += game_str;
        cmd += " . ";
        cmd += std::to_string(capacity_mib);
        cmd += " 2>&1";
        FILE *p = popen(cmd.c_str(), "r");
        if (!p) {
            m_size_compute_running[idx] = false;
            return;
        }
        char buf[256];
        std::string output;
        while (fgets(buf, sizeof(buf), p)) output += buf;
        int rc = pclose(p);

        if (rc == 0) {
            size_t marker = output.find("extflash-patched.bin (");
            if (marker != std::string::npos) {
                unsigned long used = strtoul(output.c_str() + marker + 22, nullptr, 10);
                if (used != 0) m_patched_size_cache[idx] = (uint32_t)used;
            }
        }
        m_size_compute_running[idx] = false;
    });
    return 0;
}

void GnwFlashStorageView::SyncImplicitExtflashRow()
{
    bool eligible = (m_bank[0].choice != 0 && m_bank[0].choice <= 2) && m_bank[0].patch;
    bool want_implicit = eligible && m_ofw_at_zero;
    bool have_implicit = !m_extflash_rows.empty() && m_extflash_rows[0].implicit;

    if (want_implicit && !have_implicit) {
        const char *game = kGameNames[m_bank[0].choice - 1];
        GnwExtflashBlobRow row;
        row.implicit = true;
        row.offset = 0;
        row.path = std::string(game) + "-extflash-patched.bin (patch output)";
        row.size = GetOrComputePatchedExtflashSize(game);
        m_extflash_rows.insert(m_extflash_rows.begin(), row);
    } else if (!want_implicit && have_implicit) {
        m_extflash_rows.erase(m_extflash_rows.begin());
    } else if (want_implicit && have_implicit) {
        const char *game = kGameNames[m_bank[0].choice - 1];
        m_extflash_rows[0].path = std::string(game) + "-extflash-patched.bin (patch output)";
        m_extflash_rows[0].size = GetOrComputePatchedExtflashSize(game);
    }
}

// Splices `data`/`len` into `path` at `offset`, extending/0xFF-padding the
// file to at least offset+len if it's shorter.
static bool SpliceBlobIntoFile(const char *path, uint32_t offset,
                                const uint8_t *data, size_t len,
                                std::string *err)
{
    FILE *f = fopen(path, "r+b");
    if (!f) {
        if (err) *err = std::string("cannot open ") + path;
        return false;
    }
    fseek(f, 0, SEEK_END);
    long cur_len = ftell(f);
    if (cur_len < 0) cur_len = 0;
    if ((long)(offset + len) > cur_len) {
        fseek(f, 0, SEEK_END);
        uint8_t pad_byte = 0xFF;
        for (long i = cur_len; i < (long)offset; i++) {
            fwrite(&pad_byte, 1, 1, f);
        }
    }
    fseek(f, (long)offset, SEEK_SET);
    fwrite(data, 1, len, f);
    fclose(f);
    return true;
}

static bool PadFileWithFF(const char *path, uint64_t capacity,
                          std::string *err)
{
    GStatBuf st;
    if (g_stat(path, &st) != 0 || st.st_size < 0) {
        if (err) *err = std::string("cannot stat extflash image ") + path;
        return false;
    }
    if ((uint64_t)st.st_size > capacity) {
        if (err) *err = std::string("extflash image requires ") +
            std::to_string(st.st_size) + " bytes; selected chip holds " +
            std::to_string(capacity) + " bytes";
        return false;
    }
    if ((uint64_t)st.st_size == capacity) return true;

    FILE *f = g_fopen(path, "ab");
    if (!f) {
        if (err) *err = std::string("cannot extend extflash image ") + path;
        return false;
    }
    std::vector<uint8_t> fill(1 << 20, 0xFF);
    uint64_t left = capacity - (uint64_t)st.st_size;
    bool ok = true;
    while (left) {
        size_t n = (size_t)std::min<uint64_t>(left, fill.size());
        if (fwrite(fill.data(), 1, n, f) != n) {
            ok = false;
            break;
        }
        left -= n;
    }
    if (fclose(f) != 0) ok = false;
    if (!ok && err) *err = std::string("failed to pad extflash image with 0xFF: ") + path;
    return ok;
}

bool GnwFlashStorageView::ApplyBank1(std::string *err)
{
    if (m_bank[0].choice == 0) return true; // nothing to do
    if (m_bank[0].choice > 2) {
        // Bank1 set to "Browse for a file" -- not a stock/patchable OFW
        // selection, nothing for this function to do.
        return true;
    }
    const char *game = kGameNames[m_bank[0].choice - 1];
    char *cerr = nullptr;
    if (m_bank[0].patch) {
        std::string internal = m_backup_dir + "/internal_flash_backup_" + game + ".bin";
        std::string external = m_backup_dir + "/flash_backup_" + game + ".bin";
        std::string patch = std::string("../gnwmanager/gnwmanager/cli/gnw_patch/binaries/") +
                            game + "/0x08032000.bin";
        std::string out_dir = m_backup_dir + "/qemu-images";
        g_mkdir_with_parents(out_dir.c_str(), 0755);
        std::string bank1 = out_dir + "/" + game + "-bank1-patched.bin";
        std::string extflash = out_dir + "/" + game + "-extflash-patched.bin";
        GnwCfwBuildResult result = {};
        if (!gnw_cfw_build_images(game, internal.c_str(), external.c_str(),
                                  patch.c_str(), bank1.c_str(), extflash.c_str(),
                                  ExtflashSizeBytes(), &result, &cerr)) {
            if (err) *err = cerr ? cerr : "CFW image build failed";
            free(cerr);
            return false;
        }
        free(cerr);
        if (!m_extflash_rows.empty() && m_extflash_rows[0].implicit) {
            m_extflash_rows[0].size = result.extflash_used;
        }
    } else if (!gnw_make_boot_images_for_size(game, ".", ExtflashSizeBytes(),
                                              &cerr)) {
        if (err) *err = cerr ? cerr : "gnw_make_boot_images failed";
        free(cerr);
        return false;
    } else {
        free(cerr);
    }

    ResolveExtflashRowSizes();

    const char *ext_suffix = m_bank[0].patch ? "-extflash-patched.bin" : "-extflash.bin";
    std::string ext_path = m_backup_dir + "/qemu-images/" + game + ext_suffix;
    GStatBuf ext_st;
    uint64_t required = 0;
    if (g_stat(ext_path.c_str(), &ext_st) == 0 && ext_st.st_size > 0) {
        required = ext_st.st_size;
    }
    for (const auto &row : m_extflash_rows) {
        required = std::max(required, (uint64_t)row.offset + row.size);
    }
    if (required > ExtflashSizeBytes()) {
        if (err) *err = "extflash contents require " +
            std::to_string(required) + " bytes, but " +
            SelectedNorChip(m_nor_chip_key)->name + " holds " +
            std::to_string(ExtflashSizeBytes()) + " bytes";
        return false;
    }
    for (auto &row : m_extflash_rows) {
        if (row.implicit) continue;
        gchar *contents = nullptr;
        gsize len = 0;
        GError *gerr = nullptr;
        if (!g_file_get_contents(row.path.c_str(), &contents, &len, &gerr)) {
            if (err) *err = "cannot read extflash blob " + row.path;
            if (gerr) g_error_free(gerr);
            return false;
        }
        bool ok = SpliceBlobIntoFile(ext_path.c_str(), row.offset,
                                      (const uint8_t *)contents, len, err);
        g_free(contents);
        if (!ok) return false;
    }

    if (g_stat(ext_path.c_str(), &ext_st) == 0 && ext_st.st_size > 0 &&
        !PadFileWithFF(ext_path.c_str(), ExtflashSizeBytes(), err)) {
        return false;
    }

    return true;
}

void GnwFlashStorageView::GetFinalImagePaths(std::string *bank1, std::string *bank2,
                                              std::string *extflash) const
{
    bank1->clear();
    bank2->clear();
    extflash->clear();

    if (m_bank[0].choice >= 1 && m_bank[0].choice <= 2) {
        const char *game = kGameNames[m_bank[0].choice - 1];
        const char *b1_suffix = m_bank[0].patch ? "-bank1-patched.bin" : "-bank1.bin";
        const char *ext_suffix = m_bank[0].patch ? "-extflash-patched.bin" : "-extflash.bin";
        *bank1 = m_backup_dir + "/qemu-images/" + game + b1_suffix;
        *extflash = m_backup_dir + "/qemu-images/" + game + ext_suffix;
    } else if (m_bank[0].choice > 2) {
        *bank1 = m_bank[0].custom_path; // Bank1 "Browse" selection, used as-is
    }

    // Bank2: a browsed/retro-go file is used directly (-global can point at
    // any path, no need to copy it into backup/qemu-images/); otherwise the
    // blank bank2.bin ApplyBank1()'s gnw_make_boot_images() call already
    // produced for bank1's game.
    if (m_bank[1].choice > 2 || (m_preset != kPresetCustom && !m_bank[1].custom_path.empty())) {
        *bank2 = m_bank[1].custom_path;
    } else if (m_bank[0].choice >= 1 && m_bank[0].choice <= 2) {
        *bank2 = m_backup_dir + "/qemu-images/" + kGameNames[m_bank[0].choice - 1] + "-bank2.bin";
    }
}

// Small "found/verified" indicator -- a filled dot, not verbose text; the
// user explicitly asked for low-key status, detail moved into a tooltip.
static void DrawStatusDot(const char *tag, bool found, bool verified)
{
    ImU32 color = !found ? IM_COL32(90, 90, 90, 255)
                : verified ? IM_COL32(70, 200, 90, 255)
                           : IM_COL32(220, 180, 60, 255);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float r = ImGui::GetTextLineHeight() * 0.3f;
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(p.x + r, p.y + r + 2.0f), r, color);
    ImGui::Dummy(ImVec2(r * 2.0f + 4.0f, ImGui::GetTextLineHeight()));
    if (ImGui::IsItemHovered()) {
        const char *state = !found ? "not found" : verified ? "verified" : "hash mismatch";
        ImGui::SetTooltip("%s: %s", tag, state);
    }
}

static void DrawGameStatusRow(const char *label, const GnwBackupGameStatus &st)
{
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    DrawStatusDot("internal", st.internal_found, st.internal_verified);
    ImGui::SameLine();
    DrawStatusDot("external", st.external_found, st.external_verified);
}

// Unified Bank1/Bank2 widget -- see the header/class comment for why this
// one function drives both banks instead of two different widget shapes.
static const SDL_DialogFileFilter kBankBrowseFilter[] = {
    { ".bin Files", "bin" }, { "All Files", "*" }
};

void GnwFlashStorageView::DrawBankSlot(const char *label, int bank_idx, bool allow_patch)
{
    GnwBankSlot &slot = m_bank[bank_idx];
    const char *items[4] = { "None", "Mario (stock)", "Zelda (stock)", "Browse..." };
    bool avail[4] = { true, m_status[0].Available(), m_status[1].Available(), true };

    ImGui::PushID(bank_idx);
    if (ImGui::BeginCombo(label, items[slot.choice])) {
        for (int i = 0; i < 4; i++) {
            if (!avail[i]) continue;
            bool sel = (slot.choice == i);
            if (ImGui::Selectable(items[i], sel)) {
                slot.choice = i;
                if (bank_idx == 0) SyncImplicitExtflashRow();
                // Selecting "Browse..." opens the file dialog immediately,
                // in the same click -- previously required a separate
                // button press after picking this from the dropdown, which
                // read as broken/two-step for no reason. On cancel, the
                // SDL callback simply never fires (see misc.cc), so
                // custom_path is left as whatever it already was.
                if (i == 3) {
                    GnwBankSlot *slot_ptr = &slot;
                    ShowOpenFileDialog(kBankBrowseFilter, 2,
                                        slot.custom_path.c_str(),
                                        [slot_ptr](const char *p) {
                                            slot_ptr->custom_path = p;
                                        });
                }
            }
        }
        ImGui::EndCombo();
    }

    if (slot.choice == 3) { // Browse...
        ImGui::SameLine(0, 10 * g_viewport_mgr.m_scale);
        InlineFileField("##file", slot.custom_path.c_str(), kBankBrowseFilter, 2, false,
                        [&slot](const char *p) { slot.custom_path = p; });
    } else if (slot.choice != 0 && allow_patch) {
        if (ImGui::Checkbox("Patch", &slot.patch)) {
            SyncImplicitExtflashRow();
        }
        ImGui::BeginDisabled(!slot.patch);
        ImGui::SameLine();
        ImGui::Checkbox("Bootloader", &slot.bootloader);
        ImGui::EndDisabled();
    } else if (slot.choice != 0 && !allow_patch) {
        ImGui::TextDisabled("(not patchable -- bank2 only)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("gnw-make-cfw-images always patches for bank1's "
                              "dual-boot role; there's no \"patched OFW for "
                              "bank2\" concept in this project's backend.");
        }
    }
    ImGui::PopID();
}

// ---------------------------------------------------------------------
// Extflash geometry bar -- proportional-width segments (used blob / free
// gap), hover for detail. Concept ported from gnw-web-builder's
// GeometryBar.svelte + classify.ts's extflashSegments() (read-only
// reference, that's a Svelte/TS web app -- this is a from-scratch ImGui
// equivalent of the same segment model, not a code port).
// ---------------------------------------------------------------------
struct GnwGeoSegment {
    uint32_t offset, size;
    bool is_free;
    std::string label;
};

static std::vector<GnwGeoSegment> BuildExtflashSegments(
    const std::vector<GnwExtflashBlobRow> &rows, uint32_t total_size)
{
    std::vector<const GnwExtflashBlobRow *> sorted;
    for (auto &r : rows) if (r.size > 0) sorted.push_back(&r);
    std::sort(sorted.begin(), sorted.end(),
              [](const GnwExtflashBlobRow *a, const GnwExtflashBlobRow *b) {
                  return a->offset < b->offset;
              });

    std::vector<GnwGeoSegment> segs;
    uint32_t cursor = 0;
    for (auto *r : sorted) {
        if (r->offset > cursor) {
            segs.push_back({ cursor, r->offset - cursor, true, "Free" });
        }
        std::string label = r->implicit ? "OFW patch output" : r->path;
        segs.push_back({ r->offset, r->size, false, label });
        cursor = std::max(cursor, r->offset + r->size);
    }
    if (cursor < total_size) {
        segs.push_back({ cursor, total_size - cursor, true, "Free" });
    }
    return segs;
}

void GnwFlashStorageView::DrawExtflashGeometryBar()
{
    uint32_t total = ExtflashSizeBytes();
    auto segs = BuildExtflashSegments(m_extflash_rows, total);
    if (segs.empty()) {
        ImGui::TextDisabled("(nothing placed yet)");
        return;
    }

    ImVec2 bar_size(ImGui::GetContentRegionAvail().x, 24.0f);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    float x = origin.x;
    for (auto &s : segs) {
        float w = bar_size.x * ((float)s.size / (float)total);
        if (w < 1.0f) w = 1.0f; // keep tiny blobs visibly clickable
        ImU32 color = s.is_free ? IM_COL32(60, 60, 60, 255) : IM_COL32(70, 140, 220, 255);
        draw->AddRectFilled(ImVec2(x, origin.y), ImVec2(x + w, origin.y + bar_size.y), color);
        draw->AddRect(ImVec2(x, origin.y), ImVec2(x + w, origin.y + bar_size.y), IM_COL32(0, 0, 0, 180));

        ImGui::SetCursorScreenPos(ImVec2(x, origin.y));
        ImGui::PushID(&s);
        ImGui::InvisibleButton("seg", ImVec2(w, bar_size.y));
        ImGui::PopID();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s\noffset 0x%x, size %.1f KiB",
                               s.label.c_str(), s.offset, s.size / 1024.0f);
        }
        x += w;
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + bar_size.y + 4.0f));
}

void GnwFlashStorageView::DrawAddBlobModal()
{
    if (ImGui::Button("+ Add Blob")) {
        m_modal_path.clear();
        m_modal_offset_mib = 0;
        ImGui::OpenPopup("Add Blob");
    }

    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Add Blob", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static const SDL_DialogFileFilter bin_filter[] = { { ".bin Files", "bin" }, { "All Files", "*" } };
        char path_buf[512];
        strncpy(path_buf, m_modal_path.c_str(), sizeof(path_buf) - 1);
        path_buf[sizeof(path_buf) - 1] = 0;
        if (ImGui::InputText("File", path_buf, sizeof(path_buf))) {
            m_modal_path = path_buf;
        }
        ImGui::SameLine();
        InlineFileField("##modalfile", m_modal_path.c_str(), bin_filter, 2, false,
                        [this](const char *p) { m_modal_path = p; });
        ImGui::InputInt("Offset (MiB)", &m_modal_offset_mib);
        if (m_modal_offset_mib < 0) m_modal_offset_mib = 0;

        ImGui::Separator();
        bool can_save = !m_modal_path.empty();
        ImGui::BeginDisabled(!can_save);
        if (ImGui::Button("Save")) {
            GnwExtflashBlobRow row;
            row.path = m_modal_path;
            row.offset = (uint32_t)m_modal_offset_mib * 1024u * 1024u;
            m_extflash_rows.push_back(row);
            ResolveExtflashRowSizes();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void GnwFlashStorageView::DrawExtflashEditor()
{
    // Picks up the background size computation's result once it lands --
    // GetOrComputePatchedExtflashSize() returns immediately (0 if still
    // running), so calling this every frame is cheap/safe now, not a
    // popen() per frame.
    bool ofw_available = (m_bank[0].choice != 0 && m_bank[0].choice <= 2) && m_bank[0].patch;
    ImGui::BeginDisabled(!ofw_available);
    if (ImGui::Checkbox("OFW @ 0", &m_ofw_at_zero)) {
        SyncImplicitExtflashRow();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Place the patched OFW extflash output at offset 0%s",
                          ofw_available ? "" : " (needs Bank 1 set to a patched game)");
    }

    DrawExtflashGeometryBar();

    int remove_idx = -1;
    for (size_t i = 0; i < m_extflash_rows.size(); i++) {
        auto &row = m_extflash_rows[i];
        ImGui::PushID((int)i);
        ImGui::BulletText("0x%x  %s", row.offset, row.path.c_str());
        if (!row.implicit) {
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) remove_idx = (int)i;
        }
        ImGui::PopID();
    }
    if (remove_idx >= 0) {
        m_extflash_rows.erase(m_extflash_rows.begin() + remove_idx);
    }

    DrawAddBlobModal();
}

void GnwFlashStorageView::Draw()
{
    SectionTitle("Backup Folder");
    static char dir_buf[512];
    strncpy(dir_buf, m_backup_dir.c_str(), sizeof(dir_buf) - 1);
    dir_buf[sizeof(dir_buf) - 1] = 0;
    if (ImGui::InputText("##backupdir", dir_buf, sizeof(dir_buf))) {
        m_backup_dir = dir_buf;
    }
    ImGui::SameLine();
    InlineFileField("##backupdirpicker", m_backup_dir.c_str(), nullptr, 0, true,
                    [this](const char *p) {
                        m_backup_dir = p;
                        gwemu_settings_set_string(
                            &g_config.general.firmware_backup_dir, p);
                        gwemu_settings_save();
                        RescanBackupDir();
                    });
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) {
        gwemu_settings_set_string(&g_config.general.firmware_backup_dir,
                                  m_backup_dir.c_str());
        gwemu_settings_save();
        RescanBackupDir();
    }
    DrawGameStatusRow("Mario", m_status[0]);
    DrawGameStatusRow("Zelda", m_status[1]);

    ImGui::Separator();
    SectionTitle("Preset");
    static const char *preset_names[5] = {
        "Stock Mario", "Stock Zelda", "Mario + Retro-Go", "Zelda + Retro-Go", "Custom",
    };
    bool preset_avail[5] = {
        m_status[0].Available(), m_status[1].Available(),
        m_status[0].Available(), m_status[1].Available(), true,
    };
    if (ImGui::BeginCombo("##preset", preset_names[m_preset])) {
        for (int i = 0; i < 5; i++) {
            if (!preset_avail[i]) continue;
            if (ImGui::Selectable(preset_names[i], m_preset == i)) {
                ApplyPreset(i);
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();
    SectionTitle("Internal Flash");
    if (m_preset == kPresetCustom) {
        DrawBankSlot("Bank 1", 0, /*allow_patch=*/true);
        DrawBankSlot("Bank 2", 1, /*allow_patch=*/false);
    } else if (m_preset == kPresetStockMario || m_preset == kPresetStockZelda) {
        ImGui::Text("Bank 1: %s (stock)", kGameNames[m_bank[0].choice - 1]);
        ImGui::Text("Bank 2: blank");
    } else { // +Retro-Go
        ImGui::Text("Bank 1: %s (patched)", kGameNames[m_bank[0].choice - 1]);
        static const SDL_DialogFileFilter bin_filter[] = { { ".bin Files", "bin" }, { "All Files", "*" } };
        ImGui::Text("Bank 2: ");
        ImGui::SameLine();
        InlineFileField("##bank2rg", m_bank[1].custom_path.c_str(), bin_filter, 2, false,
                        [this](const char *p) { m_bank[1].custom_path = p; });
    }

    SyncImplicitExtflashRow();
    ResolveExtflashRowSizes();
    ImGui::Separator();
    SectionTitle("External Flash");
    uint64_t required = RequiredExtflashBytes();
    const GnwNorChip *selected_chip = SelectedNorChip(m_nor_chip_key);
    char selected_label[160];
    snprintf(selected_label, sizeof(selected_label), "%s — %" PRIu64
             " MiB (JEDEC %02X %02X %02X)", selected_chip->name,
             selected_chip->size_bytes >> 20, selected_chip->jedec[0],
             selected_chip->jedec[1], selected_chip->jedec[2]);
    if (ImGui::BeginCombo("NOR chip", selected_label)) {
        for (unsigned i = 0; i < GNW_NOR_CHIP_COUNT; i++) {
            const GnwNorChip &chip = gnw_nor_chips[i];
            char label[160];
            snprintf(label, sizeof(label), "%s — %" PRIu64
                     " MiB (JEDEC %02X %02X %02X)", chip.name,
                     chip.size_bytes >> 20, chip.jedec[0], chip.jedec[1],
                     chip.jedec[2]);
            ImGui::BeginDisabled(chip.size_bytes < required);
            if (ImGui::Selectable(label, m_nor_chip_key == chip.key)) {
                m_nor_chip_key = chip.key;
                SyncImplicitExtflashRow();
            }
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    ImGui::Text("Capacity: %" PRIu64 " MiB", selected_chip->size_bytes >> 20);
    if (required) {
        ImGui::SameLine();
        ImGui::TextColored(selected_chip->size_bytes < required
                               ? ImVec4(1, 0.3f, 0.3f, 1)
                               : ImVec4(0.3f, 1, 0.3f, 1),
                           "Required by blobs: %" PRIu64 " bytes", required);
    }

    if (m_preset != kPresetStockMario && m_preset != kPresetStockZelda) {
        DrawExtflashEditor();
    }

    ImGui::Separator();
    ImGui::BeginDisabled(RequiredExtflashBytes() > ExtflashSizeBytes());
    if (ImGui::Button("Apply")) {
        m_show_apply_confirm = true;
        ImGui::OpenPopup("Apply Flash Config");
    }
    ImGui::EndDisabled();

    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Apply Flash Config", &m_show_apply_confirm,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("This will reset the device. Are you sure?");
        ImGui::Separator();
        if (ImGui::Button("Yes")) {
            std::string err;
            if (ApplyBank1(&err)) {
                std::string bank1, bank2, extflash;
                GetFinalImagePaths(&bank1, &bank2, &extflash);
                // Does not return on success -- process image is replaced.
                // Preserve the active profile's SD card (if any) across
                // the flash-only Apply -- this view doesn't manage SD.
                std::string sdp;
                bool rdp_locked = false;
                GwProfile *ap = g_profile_store.Find(
                    g_config.general.active_profile);
                if (ap) {
                    sdp = ap->SdPath();
                    rdp_locked = ap->rdp_locked;
                    ap->nor_chip = m_nor_chip_key;
                    std::string save_err;
                    if (!g_profile_store.Save(*ap, save_err)) {
                        m_status_msg = "Could not save NOR chip selection: " + save_err;
                        m_status_is_error = true;
                        ImGui::CloseCurrentPopup();
                        ImGui::EndPopup();
                        return;
                    }
                }
                gwemu_settings_set_string(&g_config.general.extflash_chip,
                                          m_nor_chip_key.c_str());
                gwemu_settings_save();
                gwemu_relaunch_with_flash_images(
                    bank1.empty() ? nullptr : bank1.c_str(),
                    bank2.empty() ? nullptr : bank2.c_str(),
                    extflash.empty() ? nullptr : extflash.c_str(),
                    sdp.empty() ? nullptr : sdp.c_str(), rdp_locked,
                    m_nor_chip_key.c_str());
                // Only reached if the relaunch itself failed (logged by
                // gwemu_relaunch_with_flash_images) -- old config keeps running.
                m_status_msg = "Relaunch failed, see log; previous config still running.";
                m_status_is_error = true;
            } else {
                m_status_msg = err;
                m_status_is_error = true;
            }
            m_show_apply_confirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            m_show_apply_confirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!m_status_msg.empty()) {
        ImGui::TextColored(m_status_is_error ? ImVec4(1, 0.3f, 0.3f, 1) : ImVec4(0.3f, 1, 0.3f, 1),
                            "%s", m_status_msg.c_str());
    }
}
