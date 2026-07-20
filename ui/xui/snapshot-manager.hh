//
// gnw-h7b0 User Interface -- snapshot manager
//
// Structural reference: xemu's own ui/xui/snapshot-manager.hh (Xbox-save
// specific, not ported -- see docs/peripheral-coverage.md /
// CLAUDE.md for why). Real save/load/delete/list against QEMU's own
// generic internal-snapshot API (include/migration/snapshot.h,
// include/block/snapshot.h) -- no G&W-specific save-state metadata.
//
#pragma once
#include <string>
#include <vector>

class SnapshotManager
{
public:
    void Draw();
    void LoadSnapshotChecked(const char *name);

private:
    void Refresh();

    std::vector<std::string> m_names;
    std::string m_error;
    std::string m_new_name_buf;
    bool m_refreshed_once = false;
};

extern SnapshotManager g_snapshot_mgr;
