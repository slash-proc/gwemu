/*
 * In-process SD card qcow2 creation -- see gwemu-sdcreate.h.
 *
 * Threading choice: callers (the profile wizard's BuildWorker, the SD Card
 * tab's) run on their own std::thread without the BQL, but the block layer
 * needs it two different ways here:
 *
 *   - bdrv_img_create()/blk_new_open()/blk_unref() are GLOBAL_STATE_CODE
 *     and assert bql_locked().
 *   - blk_pwrite() is a generated coroutine wrapper: it ends in
 *     AIO_WAIT_WHILE(qemu_aio_context), whose in_aio_context_home_thread()
 *     test for the main context is literally `bql_locked()`. With the BQL
 *     held it polls the main AioContext itself (correct); without it, it
 *     takes the other branch and aio_poll()s the main context from a
 *     foreign thread, racing the real main loop. So the BQL is required
 *     for the writes too.
 *
 * Therefore the lock is taken per operation rather than once around the
 * whole build. That matters: formatting a blank card writes only a few MiB
 * of metadata (all-zero sector writes are dropped to keep the qcow2
 * sparse) and would be fine either way, but populating a card from a
 * retro-go sd_content/ copies gigabytes, and holding the BQL across that
 * would freeze the UI and the emulated machine for minutes. One cluster
 * (4-32 KiB) is written per lock cycle, so the main loop gets the lock
 * back constantly and stays responsive.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: MIT
 */
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "block/block-global-state.h"
#include "system/block-backend.h"

#include "gwemu-sdcreate.h"
#include "../contrib/gnw-tools/gnw_sdimg.h"

typedef struct {
    BlockBackend *blk;
    gwemu_sdcreate_progress_fn cb;
    void *cb_opaque;
    uint64_t bytes;              /* running total of bytes actually written */
} SdWriteCtx;

static bool buf_all_zero(const uint8_t *buf, size_t len)
{
    return buf[0] == 0 && memcmp(buf, buf + 1, len - 1) == 0;
}

static int sd_blk_write(void *opaque, uint64_t lba, const void *buf,
                        uint32_t count)
{
    SdWriteCtx *ctx = opaque;
    size_t len = (size_t)count * 512;

    /*
     * Drop all-zero writes: a fresh qcow2 reads back as zeros for
     * unallocated clusters, which is exactly the builder's documented
     * assumption, and skipping them keeps the image sparse.
     */
    if (!buf_all_zero(buf, len)) {
        int ret;

        bql_lock();
        ret = blk_pwrite(ctx->blk, lba * 512, (int64_t)len, buf, 0);
        bql_unlock();
        if (ret < 0) {
            return ret;
        }
        ctx->bytes += len;
    }
    if (ctx->cb) {
        ctx->cb(ctx->cb_opaque, ctx->bytes);
    }
    return 0;
}

bool gwemu_sdcreate_qcow2_progress(const char *path, uint64_t total_bytes,
                                   const char *content_dir,
                                   gwemu_sdcreate_progress_fn cb,
                                   void *cb_opaque, char **err)
{
    Error *errp = NULL;
    BlockBackend *blk;
    QDict *options;
    SdWriteCtx ctx;
    char *berr = NULL;
    bool ok;

    if (err) {
        *err = NULL;
    }

    bql_lock();
    bdrv_img_create(path, "qcow2", NULL, NULL, NULL, total_bytes,
                    BDRV_O_RDWR, true, &errp);
    if (errp) {
        bql_unlock();
        goto out_err;
    }

    options = qdict_new();
    qdict_put_str(options, "driver", "qcow2");
    /* blk_new_open consumes the options dict. */
    blk = blk_new_open(path, NULL, options, BDRV_O_RDWR, &errp);
    bql_unlock();
    if (!blk) {
        goto out_err;
    }

    ctx = (SdWriteCtx){ .blk = blk, .cb = cb, .cb_opaque = cb_opaque };
    ok = gnw_sdimg_build(sd_blk_write, &ctx, total_bytes, content_dir, &berr);

    bql_lock();
    blk_unref(blk);
    bql_unlock();

    if (!ok) {
        if (err) {
            *err = berr ? berr : strdup("SD image build failed");
            berr = NULL;
        }
        unlink(path); /* best-effort: don't leave a half-made image */
    }
    free(berr);
    return ok;

out_err:
    if (err) {
        *err = strdup(error_get_pretty(errp));
    }
    error_free(errp);
    unlink(path);
    return false;
}

bool gwemu_sdcreate_qcow2(const char *path, uint64_t total_bytes,
                          const char *content_dir, char **err)
{
    return gwemu_sdcreate_qcow2_progress(path, total_bytes, content_dir,
                                         NULL, NULL, err);
}
