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
    /*
     * Do NOT call vm_stop()/vm_start() directly from the UI/HUD thread.
     * gl_render_frame() holds the BQL around the ImGui update (where menu
     * clicks and keyboard shortcuts land), and vm_stop() then runs
     * pause_all_vcpus() + bdrv_drain_all() + bdrv_flush_all() on that same
     * thread. With a block-backed SD image attached that drain/flush can
     * block the UI thread indefinitely (macOS beachball) -- the main
     * AioContext progress the drain waits on cannot run while the UI holds
     * the BQL. Same class of bug as the watchdog's -watchdog-action pause
     * fix: defer via vmstop_request / a oneshot BH so the QEMU main loop
     * owns the state transition.
     *
     * Helpers live in ui/gwemu.c (C) so we don't pull qemu/aio.h into this
     * C++ TU -- qom/object.h uses `typename` as a parameter name, which is
     * a C++ keyword.
     */
    if (runstate_is_running()) {
        gwemu_request_vm_stop();
    } else {
        gwemu_request_vm_start();
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
