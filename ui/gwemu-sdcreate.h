/*
 * In-process SD card qcow2 creation for the profile wizard -- creates a
 * qcow2 through QEMU's own block layer and formats it (MBR + FAT32,
 * matching the real Game & Watch card layout) via contrib/gnw-tools'
 * gnw_sdimg builder. No qemu-img subprocess, no Python.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: MIT
 */
#ifndef GWEMU_SDCREATE_H
#define GWEMU_SDCREATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Called (from the calling thread, with the BQL NOT held) after every
 * sector-write the builder issues, with the running total of bytes
 * actually written to the image so far. All-zero writes are dropped and
 * so contribute nothing -- for a content-populated card the total tracks
 * the copied file bytes plus a little FAT/directory metadata, which is
 * what makes it a usable progress signal against a directory scan's byte
 * count. Must be cheap and non-blocking (it runs thousands of times).
 */
typedef void (*gwemu_sdcreate_progress_fn)(void *opaque,
                                           uint64_t bytes_written);

/*
 * Create `path` as a new qcow2 of virtual size `total_bytes` (must satisfy
 * gnw_sdimg_build's size rules: power of 2 or 512 KiB multiple, >= 64 MiB)
 * and format it MBR+FAT32, populated from `content_dir` if non-NULL.
 *
 * Threading: safe to call from a non-main thread (the wizard's build
 * worker, the SD Card tab's) -- takes the BQL internally around each
 * individual block-layer call; see the .c file for the rationale. Do NOT
 * call while already holding the BQL (e.g. from the main loop / an ImGui
 * handler).
 *
 * On failure returns false, best-effort unlinks the half-made file and,
 * if err is non-NULL, sets *err to a malloc'd message (free() it).
 */
bool gwemu_sdcreate_qcow2(const char *path, uint64_t total_bytes,
                          const char *content_dir, char **err);

/* As above, plus a progress callback (either may be NULL). */
bool gwemu_sdcreate_qcow2_progress(const char *path, uint64_t total_bytes,
                                   const char *content_dir,
                                   gwemu_sdcreate_progress_fn cb,
                                   void *cb_opaque, char **err);

#ifdef __cplusplus
}
#endif

#endif
