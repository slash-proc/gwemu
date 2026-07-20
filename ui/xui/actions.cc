//
// gnw-h7b0 User Interface -- action dispatch (adapted from xemu)
//
// Xbox-specific actions (disc eject/load, F5-F8 bound-snapshot-shortcut
// slots -- tied to gwemu-snapshots.c's shortcut-key-map, not ported) have
// been dropped. See CLAUDE.md for the porting rationale.
//
#include "common.hh"
#include "actions.hh"
#include "misc.hh"
#include "gwemu-hud.h"
#include "../gwemu-notifications.h"
#include "snapshot-manager.hh"

void ActionTogglePause(void)
{
    if (runstate_is_running()) {
        vm_stop(RUN_STATE_PAUSED);
    } else {
        vm_start();
    }
}

void ActionReset(void)
{
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

void ActionShutdown(void)
{
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
}

void ActionScreenshot(void)
{
	g_screenshot_pending = true;
}

void ActionLoadSnapshotChecked(const char *name)
{
    g_snapshot_mgr.LoadSnapshotChecked(name);
}
