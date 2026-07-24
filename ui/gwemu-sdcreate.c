/*
 * In-process SD card qcow2 creation -- see gwemu-sdcreate.h.
 *
 * Threading choice: the wizard's BuildWorker runs on its own std::thread
 * without the BQL, but bdrv_img_create()/blk_new_open()/blk_unref() are
 * GLOBAL_STATE_CODE (they assert bql_locked()). So this function takes
 * bql_lock() around the whole creation, exactly like a vCPU or migration
 * thread doing block work. The main loop stalls only for the duration of
 * the call; a fresh FAT32 format writes only the MBR/boot/FAT metadata
 * (a few MiB even at 32 GiB virtual size, since all-zero sector writes
 * are dropped to keep the qcow2 sparse), measured well under a second,
 * so holding the BQL that long from the worker is acceptable and far
 * simpler than a main-loop BH handshake.
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

static bool buf_all_zero(const uint8_t *buf, size_t len)
{
    return buf[0] == 0 && memcmp(buf, buf + 1, len - 1) == 0;
}

static int sd_blk_write(void *opaque, uint64_t lba, const void *buf,
                        uint32_t count)
{
    BlockBackend *blk = opaque;

    /*
     * Drop all-zero writes: a fresh qcow2 reads back as zeros for
     * unallocated clusters, which is exactly the builder's documented
     * assumption, and skipping them keeps the image sparse.
     */
    if (buf_all_zero(buf, (size_t)count * 512)) {
        return 0;
    }
    return blk_pwrite(blk, lba * 512, (int64_t)count * 512, buf, 0);
}

bool gwemu_sdcreate_qcow2(const char *path, uint64_t total_bytes,
                          const char *content_dir, char **err)
{
    Error *errp = NULL;
    bool ok = false;

    if (err) {
        *err = NULL;
    }

    bql_lock();

    bdrv_img_create(path, "qcow2", NULL, NULL, NULL, total_bytes,
                    BDRV_O_RDWR, true, &errp);
    if (errp) {
        goto out_err;
    }

    {
        QDict *options = qdict_new();
        qdict_put_str(options, "driver", "qcow2");
        /* blk_new_open consumes the options dict. */
        BlockBackend *blk = blk_new_open(path, NULL, options,
                                         BDRV_O_RDWR, &errp);
        if (!blk) {
            goto out_err;
        }
        char *berr = NULL;
        ok = gnw_sdimg_build(sd_blk_write, blk, total_bytes, content_dir,
                             &berr);
        blk_unref(blk);
        if (!ok && err) {
            *err = berr ? berr : strdup("SD image build failed");
        } else {
            free(berr);
        }
    }

out:
    bql_unlock();
    if (!ok) {
        unlink(path); /* best-effort: don't leave a half-made image */
    }
    return ok;

out_err:
    if (err) {
        *err = strdup(error_get_pretty(errp));
    }
    error_free(errp);
    goto out;
}
