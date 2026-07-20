/*
 * GWemu QEMU Monitor Interface
 *
 * Copyright (c) 2020-2021 Matt Borgerson
 *
 * Based on gdbstub.c
 *
 * Copyright (c) 2003-2005 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef GWEMU_MONITOR_H
#define GWEMU_MONITOR_H

#ifdef __cplusplus
extern "C" {
#endif

void gwemu_monitor_init(void);
char *gwemu_get_monitor_buffer(void);
void gwemu_run_monitor_command(const char *cmd);

#ifdef __cplusplus
}
#endif

#endif
