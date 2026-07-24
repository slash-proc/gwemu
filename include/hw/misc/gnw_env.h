/*
 * Shared helper for the GNW_* diagnostic env-var probes.
 *
 * A bare presence check (getenv() != NULL) made GNW_FOO=0 enable the
 * probe -- confirmed user-facing trap (exporting the vars with value 0
 * still produced the full trace firehose). Off means unset, empty, or
 * "0"; anything else is on.
 */
#ifndef GNW_ENV_H
#define GNW_ENV_H

#include <stdlib.h>
#include <string.h>

static inline bool gnw_env_enabled(const char *name)
{
    const char *v = getenv(name);
    return v != NULL && v[0] != '\0' && strcmp(v, "0") != 0;
}

#endif
