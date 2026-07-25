/*
 * Minimal single-file HTTP(S) GET, for the GUI's small on-demand asset
 * fetches (currently just gnwmanager's per-game patch binary).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef GWEMU_HTTP_H
#define GWEMU_HTTP_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Download `url` to `dst_path` (written via a .part temp file and
 * renamed on success, so a partial transfer never leaves a valid-looking
 * destination). Blocking -- call from a worker thread, never from an
 * ImGui click handler (see CLAUDE.md "Async discipline").
 *
 * On failure returns false and, if `err` is non-NULL, stores a
 * g_malloc'd message the caller must g_free().
 */
bool gwemu_http_download(const char *url, const char *dst_path, char **err);

#ifdef __cplusplus
}
#endif

#endif
