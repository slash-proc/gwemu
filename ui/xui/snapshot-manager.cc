//
// gnw-h7b0 User Interface -- minimal snapshot-manager stub (Phase 1)
//
#include "common.hh"
#include "snapshot-manager.hh"
extern "C" {
#include "migration/snapshot.h"
}
#include "../xemu-notifications.h"

SnapshotManager g_snapshot_mgr;

void SnapshotManager::Draw()
{
    // Real snapshot-browser UI is a later phase; nothing to draw yet.
}

void SnapshotManager::LoadSnapshotChecked(const char *name)
{
    Error *err = NULL;
    load_snapshot(name, NULL, false, NULL, &err);
    if (err) {
        xemu_queue_error_message(error_get_pretty(err));
        error_free(err);
    }
}
