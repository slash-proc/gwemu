/*
 * Minimal single-file HTTP(S) GET. See gwemu-http.h.
 *
 * Why this exists rather than a popen("curl ...") one-liner: on Windows
 * popen() spawns a cmd.exe, which flashes a visible console window on
 * top of the GUI for every fetch, and the POSIX command line it would
 * run there is wrong anyway ('single quotes' aren't quoting, /dev/null
 * isn't a path, and the static build ships no wget). Both symptoms were
 * reported from a real Windows run of the profile wizard. WinINet is a
 * Windows system DLL, so this stays consistent with the fully-static
 * Windows build (no new bundled dependency).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "gwemu-http.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#include <wininet.h>
#endif

/*
 * Writes the body to `tmp`; the caller does the rename. Returns false
 * with *err set on any failure.
 */
static bool fetch_to_file(const char *url, const char *tmp, char **err);

bool gwemu_http_download(const char *url, const char *dst_path, char **err)
{
    if (err) {
        *err = NULL;
    }

    char *dir = g_path_get_dirname(dst_path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    char *tmp = g_strdup_printf("%s.part", dst_path);
    bool ok = fetch_to_file(url, tmp, err);

    if (ok) {
        /* g_rename() overwrites on Windows too, unlike plain rename(). */
        if (g_rename(tmp, dst_path) != 0) {
            if (err && !*err) {
                *err = g_strdup_printf("could not move downloaded file into "
                                       "place: %s", g_strerror(errno));
            }
            ok = false;
        }
    }
    if (!ok) {
        g_unlink(tmp);
    }
    g_free(tmp);
    return ok;
}

#ifdef _WIN32

static bool fetch_to_file(const char *url, const char *tmp, char **err)
{
    bool ok = false;
    HINTERNET session = NULL, req = NULL;
    FILE *f = NULL;

    session = InternetOpenA("gwemu", INTERNET_OPEN_TYPE_PRECONFIG,
                            NULL, NULL, 0);
    if (!session) {
        if (err) *err = g_strdup("could not initialise WinINet");
        goto out;
    }

    /*
     * INTERNET_FLAG_NO_UI: never pop a modal credential/certificate
     * dialog from this worker thread. RELOAD/NO_CACHE_WRITE: this runs
     * once per game and a stale WinINet cache entry would be worse than
     * a re-fetch.
     */
    req = InternetOpenUrlA(session, url, NULL, 0,
                           INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD |
                           INTERNET_FLAG_NO_CACHE_WRITE |
                           INTERNET_FLAG_NO_UI, 0);
    if (!req) {
        if (err) {
            *err = g_strdup_printf("connection failed (WinINet error %lu)",
                                   (unsigned long)GetLastError());
        }
        goto out;
    }

    DWORD status = 0, status_len = sizeof(status), idx = 0;
    if (HttpQueryInfoA(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                       &status, &status_len, &idx) && status != 200) {
        if (err) {
            *err = g_strdup_printf("server returned HTTP %lu",
                                   (unsigned long)status);
        }
        goto out;
    }

    f = g_fopen(tmp, "wb");
    if (!f) {
        if (err) {
            *err = g_strdup_printf("cannot write %s: %s", tmp,
                                   g_strerror(errno));
        }
        goto out;
    }

    char buf[16384];
    DWORD got = 0;
    guint64 total = 0;
    while (InternetReadFile(req, buf, sizeof(buf), &got) && got > 0) {
        if (fwrite(buf, 1, got, f) != got) {
            if (err) {
                *err = g_strdup_printf("short write to %s: %s", tmp,
                                       g_strerror(errno));
            }
            goto out;
        }
        total += got;
    }
    if (total == 0) {
        if (err) *err = g_strdup("server returned an empty response");
        goto out;
    }
    ok = true;

out:
    if (f) {
        fclose(f);
    }
    if (req) {
        InternetCloseHandle(req);
    }
    if (session) {
        InternetCloseHandle(session);
    }
    return ok;
}

#else

static bool fetch_to_file(const char *url, const char *tmp, char **err)
{
    /*
     * Linux/macOS: curl (or wget) is effectively always present, and
     * popen() here costs nothing visible -- no console window to flash.
     * The arguments are ours, not user input, but quote anyway.
     */
    char *q_tmp = g_shell_quote(tmp);
    char *q_url = g_shell_quote(url);
    char *cmd = g_strdup_printf("curl -fsSL -o %s %s 2>/dev/null || "
                                "wget -qO %s %s 2>/dev/null",
                                q_tmp, q_url, q_tmp, q_url);
    FILE *pf = popen(cmd, "r");
    int rc = pf ? pclose(pf) : -1;
    g_free(cmd);
    g_free(q_tmp);
    g_free(q_url);

    GStatBuf st;
    bool ok = rc == 0 && g_stat(tmp, &st) == 0 && st.st_size > 0;
    if (!ok && err) {
        *err = g_strdup(pf ? "transfer failed (curl/wget)"
                           : "could not run curl/wget");
    }
    return ok;
}

#endif
