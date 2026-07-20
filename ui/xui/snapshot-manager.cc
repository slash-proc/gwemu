//
// gnw-h7b0 User Interface -- snapshot manager
//
#include "common.hh"
#include "snapshot-manager.hh"
extern "C" {
#include "migration/snapshot.h"
#include "block/snapshot.h"
}
#include "../gwemu-notifications.h"

SnapshotManager g_snapshot_mgr;

// bdrv_all_can_snapshot()/bdrv_all_find_vmstate_bs() require this board to
// have a snapshot-capable (qcow2-backed) block device attached via -drive.
// gnw-h7b0's normal launch (scripts/boot_qemu.sh) uses -global
// gnw-h7b0-soc.*-image= properties instead, which are plain memory-backed
// MemoryRegions, not BlockDriverStates -- so with no -drive attached,
// there is nothing to snapshot into, and every call below surfaces that
// as a real, honest error rather than silently no-opping. Attaching a
// qcow2 drive (e.g. -drive file=state.qcow2,if=none... -- see QEMU's own
// docs) is what makes this tab actually usable; not this project's SD
// card image, which is a plain raw FAT32 disk, not qcow2.
void SnapshotManager::Refresh()
{
    m_names.clear();
    m_error.clear();

    Error *err = NULL;
    if (!bdrv_all_can_snapshot(false, NULL, &err)) {
        m_error = err ? error_get_pretty(err) :
                         "No snapshot-capable block device attached";
        if (err) {
            error_free(err);
        }
        return;
    }

    BlockDriverState *bs = bdrv_all_find_vmstate_bs(NULL, false, NULL, &err);
    if (!bs) {
        m_error = err ? error_get_pretty(err) :
                         "No snapshot-capable block device attached";
        if (err) {
            error_free(err);
        }
        return;
    }

    QEMUSnapshotInfo *sn_tab = NULL;
    int n = bdrv_snapshot_list(bs, &sn_tab);
    if (n < 0) {
        m_error = "Failed to list snapshots";
        return;
    }
    for (int i = 0; i < n; i++) {
        m_names.push_back(sn_tab[i].name);
    }
    g_free(sn_tab);
}

void SnapshotManager::Draw()
{
    if (!m_refreshed_once) {
        Refresh();
        m_refreshed_once = true;
    }

    if (ImGui::Button("Refresh")) {
        Refresh();
    }
    ImGui::SameLine();

    static char name_buf[256] = "";
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##snap_name", "snapshot name", name_buf,
                              sizeof(name_buf));
    ImGui::SameLine();
    if (ImGui::Button("Save") && name_buf[0] != '\0') {
        Error *err = NULL;
        if (!save_snapshot(name_buf, true, NULL, false, NULL, &err)) {
            gwemu_queue_error_message(error_get_pretty(err));
            error_free(err);
        } else {
            gwemu_queue_notification(
                g_strdup_printf("Saved snapshot '%s'", name_buf));
        }
        Refresh();
    }

    ImGui::Spacing();

    if (!m_error.empty()) {
        ImGui::TextColored(ImVec4(1, 0.6f, 0, 1), "%s", m_error.c_str());
        ImGui::TextWrapped(
            "Attach a qcow2-backed drive at launch (-drive "
            "file=state.qcow2,if=none,...) to use snapshots.");
        return;
    }

    if (m_names.empty()) {
        ImGui::TextWrapped("No snapshots saved yet.");
        return;
    }

    if (ImGui::BeginTable("gnw_snapshot_tbl", 3,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Load", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Delete", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < m_names.size(); i++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(m_names[i].c_str());

            ImGui::TableSetColumnIndex(1);
            ImGui::PushID((int)(i * 2));
            if (ImGui::Button("Load")) {
                LoadSnapshotChecked(m_names[i].c_str());
            }
            ImGui::PopID();

            ImGui::TableSetColumnIndex(2);
            ImGui::PushID((int)(i * 2 + 1));
            if (ImGui::Button("Delete")) {
                Error *err = NULL;
                if (!delete_snapshot(m_names[i].c_str(), false, NULL, &err)) {
                    gwemu_queue_error_message(error_get_pretty(err));
                    error_free(err);
                }
                Refresh();
            }
            ImGui::PopID();
        }

        ImGui::EndTable();
    }
}

void SnapshotManager::LoadSnapshotChecked(const char *name)
{
    Error *err = NULL;
    load_snapshot(name, NULL, false, NULL, &err);
    if (err) {
        gwemu_queue_error_message(error_get_pretty(err));
        error_free(err);
    }
}
