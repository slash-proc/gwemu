/*
 * GWemu -- runtime control of QEMU's gdbstub from the GUI.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef GWEMU_GDBSTUB_H
#define GWEMU_GDBSTUB_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start (or restart) the gdbstub listening on addr:port, or tear it down
 * when enable is false. Safe to call with or without the BQL held.
 *
 * addr may be NULL/empty, which binds all interfaces (QEMU's own default
 * for "tcp::PORT"). Returns true on success; on failure writes a
 * human-readable message into errbuf (if non-NULL) and returns false.
 */
bool gwemu_gdbstub_apply(bool enable, const char *addr, int port,
                         char *errbuf, size_t errlen);

/* True once a CPU exists, i.e. once the stub can be started at all. */
bool gwemu_gdbstub_machine_ready(void);

/* True if the stub is currently listening (as far as we started it). */
bool gwemu_gdbstub_is_running(void);

/* The connection string gwemu_gdbstub_apply() would use, e.g.
 * "tcp:127.0.0.1:1234". Returns a pointer to a static buffer. */
const char *gwemu_gdbstub_device_string(const char *addr, int port);

#ifdef __cplusplus
}
#endif

#endif
