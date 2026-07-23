/*
 * gnw-h7b0 timeline script engine (headless capture/automation).
 *
 * Executes a user-supplied script of button presses, screenshots and a
 * final quit ON THE VIRTUAL CLOCK, inside QEMU -- the property that makes
 * runs repeatable: an action at guest-time 5:12.0 (or vblank frame
 * @18720) lands on the same guest microsecond every run, on any host, at
 * any emulation speed (including -icount faster-than-realtime runs).
 * This is deliberately the ONLY control surface for headless use --
 * interactive control planes (QMP commands / CLI / REST) were considered
 * and shelved: wall-clock-driven input is inherently unrepeatable.
 *
 * Activated by GNW_TIMELINE=<file>; entirely inert otherwise. See
 * docs/headless-capture.md for the script format.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GNW_TIMELINE_H
#define GNW_TIMELINE_H

#include "qemu/osdep.h"

typedef struct GnwH7B0GpioState GnwH7B0GpioState;

/*
 * Parse GNW_TIMELINE (if set) and arm the virtual-clock event chain.
 * Called once from the GPIO device's realize (the timeline needs the
 * button-injection device; screenshots/quit are device-agnostic).
 * Parse errors are fatal at startup (CI wants fail-fast, not a silently
 * wrong recording).
 */
void gnw_timeline_init(GnwH7B0GpioState *gpio);

/*
 * One display frame elapsed -- called from the LTDC vblank tick
 * unconditionally (cheap no-op when no @frame entries are armed).
 * Drives entries using @NNN frame-number addressing.
 */
void gnw_timeline_notify_vblank(void);

#endif
