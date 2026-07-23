/*
 * gnw-h7b0 headless session recorder -- see gnw_h7b0_recorder.c.
 * Armed by GNW_RECORD=<basename>; inert otherwise.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GNW_H7B0_RECORDER_H
#define GNW_H7B0_RECORDER_H

#include "qemu/osdep.h"

typedef struct QemuConsole QemuConsole;

/* Called once from the LTDC's realize with its console. */
void gnw_h7b0_recorder_init(QemuConsole *con);

#endif
