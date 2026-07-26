/*
 * GWemu -- runtime control of QEMU's gdbstub from the GUI.
 *
 * QEMU normally takes the gdbstub from the command line (-gdb / -s), but
 * gdbserver_start() (gdbstub/system.c) is also the runtime entry point
 * behind the HMP "gdbserver" command and can be called while the machine
 * is running. Passing the literal device string "none" is the supported
 * teardown: gdbserver_start() then creates no chardev and, because the
 * gdbstub state is already initialised, runs
 * qemu_chr_fe_deinit(&gdbserver_system_state.chr, true) which destroys
 * the previously created socket chardev (releasing the listening port)
 * and leaves the stub in RS_INACTIVE. That is exactly what
 * "gdbserver none" does at the monitor, so enable/disable from the GUI
 * both take effect immediately -- no restart, no leaked socket.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "exec/gdbstub.h"
#include "hw/core/cpu.h"

#include "gwemu-gdbstub.h"

static bool gdbstub_running;

const char *gwemu_gdbstub_device_string(const char *addr, int port)
{
    static char buf[128];

    /* QEMU's socket parser treats an empty host as "all interfaces".
     * A literal IPv6 address must be bracketed by the caller. */
    snprintf(buf, sizeof(buf), "tcp:%s:%d", addr ? addr : "", port);
    return buf;
}

bool gwemu_gdbstub_machine_ready(void)
{
    /* gdbserver_start() refuses to attach before there is a CPU, and the
     * HUD is initialised well before machine init finishes -- so the
     * startup application has to wait for this. */
    return first_cpu != NULL;
}

bool gwemu_gdbstub_is_running(void)
{
    return gdbstub_running;
}

bool gwemu_gdbstub_apply(bool enable, const char *addr, int port,
                         char *errbuf, size_t errlen)
{
    Error *err = NULL;
    const char *device;
    bool ok;
    bool need_lock;

    if (errbuf && errlen) {
        errbuf[0] = '\0';
    }

    if (!enable && !gdbstub_running) {
        /* Never started -- don't drag the gdbstub state into existence
         * (and don't spawn its hidden HMP monitor chardev) just to turn
         * off something that was never on. */
        return true;
    }

    if (enable && (port < 1 || port > 65535)) {
        if (errbuf && errlen) {
            snprintf(errbuf, errlen, "GDB port %d is out of range (1-65535)",
                     port);
        }
        return false;
    }

    device = enable ? gwemu_gdbstub_device_string(addr, port) : "none";

    /* gdbserver_start() is global-state code: chardev creation, monitor
     * init and the vm-change-state handler all expect the BQL. The HUD
     * is normally drawn with it held (see ui/gwemu.c gl_render_frame),
     * but startup application runs from other contexts, so check. */
    need_lock = !bql_locked();
    if (need_lock) {
        bql_lock();
    }
    ok = gdbserver_start(device, &err);
    if (need_lock) {
        bql_unlock();
    }

    if (!ok) {
        if (errbuf && errlen) {
            snprintf(errbuf, errlen, "%s",
                     err ? error_get_pretty(err) : "unknown error");
        }
        error_free(err);
        return false;
    }

    gdbstub_running = enable;
    return true;
}
