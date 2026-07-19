//
// gnw-h7b0 User Interface -- minimal snapshot-manager stub (Phase 1)
//
// Structural reference: xemu's own ui/xui/snapshot-manager.hh (Xbox-save
// specific, not ported -- see docs/peripheral-coverage.md /
// CLAUDE.md for why). This just satisfies the API surface main.cc/
// actions.cc/popup-menu.cc already call (g_snapshot_mgr.Draw(),
// LoadSnapshotChecked()) with real QEMU savevm/loadvm calls, deferring
// any actual snapshot-browser UI to a later phase.
//
#pragma once

class SnapshotManager
{
public:
    void Draw();
    void LoadSnapshotChecked(const char *name);
};

extern SnapshotManager g_snapshot_mgr;
