/*
 * FAT32 mkfs + populate-from-directory -- see gnw_fat32.h.
 *
 * Layout decisions mirror what scripts/make_sdcard_image.py (pyfatfs)
 * produced where they matter for on-disk compatibility (geometry fields,
 * reserved-sector count, FSInfo/backup-boot placement, volume-label root
 * entry, fatgen103 cluster-size table). FAT sizing uses the actual
 * fatgen103 formula in sectors -- pyfatfs computed it from the byte size,
 * yielding an absurdly oversized FAT; byte-identity with pyfatfs is
 * explicitly a non-goal (mountable-and-correct is the bar).
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "gnw_fat32.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define SECTOR_SIZE      512u
#define RSVD_SECTORS     32u
#define NUM_FATS         2u
#define BACKUP_BOOT_SEC  6u
#define FAT_EOC          0x0FFFFFFFu

typedef struct {
    gnw_sector_write_fn write;
    void *opaque;
    uint64_t vol_lba;          /* absolute LBA of volume start */
    uint32_t spc;              /* sectors per cluster */
    uint32_t fat_sectors;      /* size of one FAT, in sectors */
    uint32_t cluster_count;    /* number of data clusters */
    uint32_t data_start;       /* volume-relative first data sector */
    uint32_t next_free;        /* next never-allocated cluster */
    uint32_t *fat;             /* in-memory FAT, cluster_count + 2 entries */
    char *err;
} Fat32Ctx;

static void set_err(char **err, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void set_err(char **err, const char *fmt, ...)
{
    va_list ap;
    if (!err || *err) {
        return;
    }
    va_start(ap, fmt);
    if (vasprintf(err, fmt, ap) < 0) {
        *err = NULL;
    }
    va_end(ap);
}

/* Write `count` sectors at volume-relative sector `sec`. */
static bool vwrite(Fat32Ctx *c, uint64_t sec, const void *buf, uint32_t count,
                   char **err)
{
    int r = c->write(c->opaque, c->vol_lba + sec, buf, count);
    if (r < 0) {
        set_err(err, "sector write at lba %llu failed: %s",
                (unsigned long long)(c->vol_lba + sec), strerror(-r));
        return false;
    }
    return true;
}

static uint64_t cluster_sector(Fat32Ctx *c, uint32_t cl)
{
    return c->data_start + (uint64_t)(cl - 2) * c->spc;
}

/* Allocate the next sequential free cluster as an end-of-chain. */
static uint32_t alloc_cluster(Fat32Ctx *c, char **err)
{
    if (c->next_free >= c->cluster_count + 2) {
        set_err(err, "volume full (out of clusters)");
        return 0;
    }
    uint32_t cl = c->next_free++;
    c->fat[cl] = FAT_EOC;
    return cl;
}

/* Extend the chain ending at `tail` by one cluster; returns new tail. */
static uint32_t extend_chain(Fat32Ctx *c, uint32_t tail, char **err)
{
    uint32_t cl = alloc_cluster(c, err);
    if (cl) {
        c->fat[tail] = cl;
    }
    return cl;
}

static void wr16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

/* ---- directory entries ------------------------------------------------ */

typedef struct {
    uint8_t *buf;              /* raw 32-byte entries */
    size_t len, cap;
    uint8_t (*sfns)[11];       /* short names present, for dedupe */
    size_t nsfn, sfncap;
} DirBuf;

static bool dir_append(DirBuf *d, const uint8_t entry[32], char **err)
{
    if (d->len + 32 > d->cap) {
        size_t ncap = d->cap ? d->cap * 2 : 4096;
        uint8_t *nb = realloc(d->buf, ncap);
        if (!nb) {
            set_err(err, "out of memory");
            return false;
        }
        d->buf = nb;
        d->cap = ncap;
    }
    memcpy(d->buf + d->len, entry, 32);
    d->len += 32;
    return true;
}

static void fat_times(time_t t, uint16_t *fdate, uint16_t *ftime)
{
    struct tm tm;
    localtime_r(&t, &tm);
    int year = tm.tm_year + 1900;
    if (year < 1980) {
        year = 1980;
    }
    *fdate = (uint16_t)(((year - 1980) << 9) | ((tm.tm_mon + 1) << 5) |
                        tm.tm_mday);
    *ftime = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) |
                        (tm.tm_sec / 2));
}

static void fill_sfn_entry(uint8_t e[32], const uint8_t sfn[11],
                           uint8_t attr, uint32_t first_cluster,
                           uint32_t size, time_t mtime)
{
    uint16_t fd, ft;
    memset(e, 0, 32);
    memcpy(e, sfn, 11);
    e[11] = attr;
    fat_times(mtime, &fd, &ft);
    wr16(e + 14, ft);          /* create time */
    wr16(e + 16, fd);          /* create date */
    wr16(e + 18, fd);          /* access date */
    wr16(e + 20, (uint16_t)(first_cluster >> 16));
    wr16(e + 22, ft);          /* write time */
    wr16(e + 24, fd);          /* write date */
    wr16(e + 26, (uint16_t)first_cluster);
    wr32(e + 28, size);
}

static bool sfn_char_ok(unsigned char ch)
{
    if (ch <= 0x20 || ch > 0x7e) {
        return false;
    }
    return !strchr("\"*+,./:;<=>?[\\]|", ch);
}

/*
 * Build a candidate 8.3 name (space-padded, uppercased); returns true if
 * the long name is already an exact 8.3 representation (no LFN needed).
 */
static bool make_basis_sfn(const char *name, uint8_t sfn[11])
{
    const char *dot = strrchr(name, '.');
    const char *base = name;
    size_t blen = dot ? (size_t)(dot - name) : strlen(name);
    const char *ext = dot ? dot + 1 : "";
    size_t elen = strlen(ext);
    bool exact = true;
    size_t i, o = 0;

    if (dot == name) {         /* leading dot: fold whole name into base */
        base = name;
        blen = strlen(name);
        ext = "";
        elen = 0;
        exact = false;
    }
    memset(sfn, ' ', 11);
    for (i = 0; i < blen && o < 8; i++) {
        unsigned char ch = (unsigned char)base[i];
        if (!sfn_char_ok(ch)) {
            ch = '_';
            exact = false;
        }
        if (islower(ch)) {
            exact = false;
        }
        sfn[o++] = (uint8_t)toupper(ch);
    }
    if (blen > 8 || blen == 0) {
        exact = false;
    }
    for (i = 0, o = 8; i < elen && o < 11; i++) {
        unsigned char ch = (unsigned char)ext[i];
        if (!sfn_char_ok(ch)) {
            ch = '_';
            exact = false;
        }
        if (islower(ch)) {
            exact = false;
        }
        sfn[o++] = (uint8_t)toupper(ch);
    }
    if (elen > 3) {
        exact = false;
    }
    if (sfn[0] == 0xE5) {      /* 0xE5 means deleted; use spec substitute */
        sfn[0] = 0x05;
    }
    return exact;
}

static bool sfn_taken(DirBuf *d, const uint8_t sfn[11])
{
    for (size_t i = 0; i < d->nsfn; i++) {
        if (memcmp(d->sfns[i], sfn, 11) == 0) {
            return true;
        }
    }
    return false;
}

static bool sfn_record(DirBuf *d, const uint8_t sfn[11], char **err)
{
    if (d->nsfn == d->sfncap) {
        size_t ncap = d->sfncap ? d->sfncap * 2 : 64;
        void *nb = realloc(d->sfns, ncap * 11);
        if (!nb) {
            set_err(err, "out of memory");
            return false;
        }
        d->sfns = nb;
        d->sfncap = ncap;
    }
    memcpy(d->sfns[d->nsfn++], sfn, 11);
    return true;
}

/* Apply a ~N numeric tail to a basis name until unique in this dir. */
static bool uniquify_sfn(DirBuf *d, uint8_t sfn[11], char **err)
{
    for (unsigned n = 1; n < 1000000; n++) {
        char tail[9];
        int tlen = snprintf(tail, sizeof(tail), "~%u", n);
        size_t blen = 8;
        while (blen > 0 && sfn[blen - 1] == ' ') {
            blen--;
        }
        if (blen + tlen > 8) {
            blen = 8 - tlen;
        }
        uint8_t cand[11];
        memcpy(cand, sfn, 11);
        memset(cand, ' ', 8);
        memcpy(cand, sfn, blen);
        memcpy(cand + blen, tail, tlen);
        if (!sfn_taken(d, cand)) {
            memcpy(sfn, cand, 11);
            return true;
        }
    }
    set_err(err, "cannot generate unique 8.3 name");
    return false;
}

static uint8_t lfn_checksum(const uint8_t sfn[11])
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + sfn[i]);
    }
    return sum;
}

/*
 * Append LFN entries (if needed) + the 8.3 entry for `name`.
 * Names are treated as Latin-1 -> UCS-2 (content trees here are ASCII).
 */
static bool dir_add_named(DirBuf *d, const char *name, uint8_t attr,
                          uint32_t first_cluster, uint32_t size,
                          time_t mtime, char **err)
{
    uint8_t sfn[11], e[32];
    bool exact = make_basis_sfn(name, sfn);

    if (sfn_taken(d, sfn)) {
        exact = false;
        if (!uniquify_sfn(d, sfn, err)) {
            return false;
        }
    }
    if (!sfn_record(d, sfn, err)) {
        return false;
    }

    if (!exact) {
        size_t len = strlen(name);
        int nent = (int)((len + 12) / 13);
        uint8_t csum = lfn_checksum(sfn);
        for (int seq = nent; seq >= 1; seq--) {
            memset(e, 0, 32);
            e[0] = (uint8_t)(seq | (seq == nent ? 0x40 : 0));
            e[11] = 0x0F;      /* ATTR_LONG_NAME */
            e[13] = csum;
            static const int slots[13] = {1, 3, 5, 7, 9,
                                          14, 16, 18, 20, 22, 24,
                                          28, 30};
            for (int k = 0; k < 13; k++) {
                size_t idx = (size_t)(seq - 1) * 13 + k;
                uint16_t u;
                if (idx < len) {
                    u = (uint8_t)name[idx];
                } else if (idx == len) {
                    u = 0x0000;
                } else {
                    u = 0xFFFF;
                }
                wr16(e + slots[k], u);
            }
            if (!dir_append(d, e, err)) {
                return false;
            }
        }
    }
    fill_sfn_entry(e, sfn, attr, first_cluster, size, mtime);
    return dir_append(d, e, err);
}

/* ---- content population ----------------------------------------------- */

/* Stream a regular file into freshly allocated clusters. */
static bool write_file_data(Fat32Ctx *c, const char *path, uint64_t fsize,
                            uint32_t *first_out, char **err)
{
    uint32_t bpc = c->spc * SECTOR_SIZE;
    uint8_t *buf = NULL;
    FILE *f = NULL;
    uint32_t first = 0, tail = 0;
    bool ok = false;

    *first_out = 0;
    if (fsize == 0) {
        return true;
    }
    if (fsize > 0xFFFFFFFFu) {
        set_err(err, "%s: file exceeds FAT32 4GiB-1 limit", path);
        return false;
    }
    buf = malloc(bpc);
    f = fopen(path, "rb");
    if (!buf || !f) {
        set_err(err, "%s: %s", path, buf ? strerror(errno) : "out of memory");
        goto out;
    }
    for (uint64_t off = 0; off < fsize; off += bpc) {
        size_t chunk = (fsize - off < bpc) ? (size_t)(fsize - off) : bpc;
        if (fread(buf, 1, chunk, f) != chunk) {
            set_err(err, "%s: short read", path);
            goto out;
        }
        memset(buf + chunk, 0, bpc - chunk);
        uint32_t cl = first ? extend_chain(c, tail, err)
                            : alloc_cluster(c, err);
        if (!cl) {
            goto out;
        }
        if (!first) {
            first = cl;
        }
        tail = cl;
        if (!vwrite(c, cluster_sector(c, cl), buf, c->spc, err)) {
            goto out;
        }
    }
    *first_out = first;
    ok = true;
out:
    if (f) {
        fclose(f);
    }
    free(buf);
    return ok;
}

/* Flush a directory's entry buffer into a fresh-enough cluster chain. */
static bool flush_dir(Fat32Ctx *c, DirBuf *d, uint32_t first, char **err)
{
    uint32_t bpc = c->spc * SECTOR_SIZE;
    uint32_t cl = first, tail = first;
    uint8_t *buf = malloc(bpc);
    if (!buf) {
        set_err(err, "out of memory");
        return false;
    }
    for (size_t off = 0; off == 0 || off < d->len; off += bpc) {
        if (off > 0) {
            cl = extend_chain(c, tail, err);
            if (!cl) {
                free(buf);
                return false;
            }
            tail = cl;
        }
        size_t chunk = (d->len > off) ?
            ((d->len - off < bpc) ? d->len - off : bpc) : 0;
        memset(buf, 0, bpc);
        if (chunk) {
            memcpy(buf, d->buf + off, chunk);
        }
        if (!vwrite(c, cluster_sector(c, cl), buf, c->spc, err)) {
            free(buf);
            return false;
        }
    }
    free(buf);
    return true;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/*
 * Recursively populate directory `path` into a new cluster chain.
 * `self_first` is this directory's already-allocated first cluster;
 * `parent_first` is the parent's (0 for entries pointing at the root).
 */
static bool populate_dir(Fat32Ctx *c, const char *path, uint32_t self_first,
                         uint32_t parent_first, bool is_root,
                         const char *label, char **err)
{
    DirBuf d = {0};
    DIR *dp = NULL;
    char **names = NULL;
    size_t nnames = 0, namecap = 0;
    bool ok = false;
    uint8_t e[32];
    time_t now = time(NULL);

    if (is_root && label) {
        uint8_t l[11];
        memset(l, ' ', 11);
        for (int i = 0; i < 11 && label[i]; i++) {
            l[i] = (uint8_t)label[i];
        }
        fill_sfn_entry(e, l, 0x08 /* ATTR_VOLUME_ID */, 0, 0, now);
        if (!dir_append(&d, e, err)) {
            goto out;
        }
    }
    if (!is_root) {
        uint8_t dot[11];
        memset(dot, ' ', 11);
        dot[0] = '.';
        fill_sfn_entry(e, dot, 0x10, self_first, 0, now);
        if (!dir_append(&d, e, err)) {
            goto out;
        }
        dot[1] = '.';
        fill_sfn_entry(e, dot, 0x10, parent_first, 0, now);
        if (!dir_append(&d, e, err)) {
            goto out;
        }
    }

    if (path) {
        dp = opendir(path);
        if (!dp) {
            set_err(err, "%s: %s", path, strerror(errno));
            goto out;
        }
        struct dirent *de;
        while ((de = readdir(dp))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
                continue;
            }
            if (nnames == namecap) {
                namecap = namecap ? namecap * 2 : 32;
                char **nn = realloc(names, namecap * sizeof(*names));
                if (!nn) {
                    set_err(err, "out of memory");
                    goto out;
                }
                names = nn;
            }
            names[nnames] = strdup(de->d_name);
            if (!names[nnames]) {
                set_err(err, "out of memory");
                goto out;
            }
            nnames++;
        }
        /* Deterministic output independent of readdir order. */
        qsort(names, nnames, sizeof(*names), cmp_names);

        for (size_t i = 0; i < nnames; i++) {
            char *child;
            struct stat st;
            if (asprintf(&child, "%s/%s", path, names[i]) < 0) {
                set_err(err, "out of memory");
                goto out;
            }
            if (stat(child, &st) < 0) {
                set_err(err, "%s: %s", child, strerror(errno));
                free(child);
                goto out;
            }
            if (S_ISDIR(st.st_mode)) {
                uint32_t sub = alloc_cluster(c, err);
                if (!sub ||
                    !populate_dir(c, child, sub,
                                  is_root ? 0 : self_first, false, NULL,
                                  err) ||
                    !dir_add_named(&d, names[i], 0x10, sub, 0,
                                   st.st_mtime, err)) {
                    free(child);
                    goto out;
                }
            } else if (S_ISREG(st.st_mode)) {
                uint32_t first;
                if (!write_file_data(c, child, (uint64_t)st.st_size,
                                     &first, err) ||
                    !dir_add_named(&d, names[i], 0x20, first,
                                   (uint32_t)st.st_size, st.st_mtime,
                                   err)) {
                    free(child);
                    goto out;
                }
            } /* sockets/fifos/symlink-to-nowhere: silently skipped */
            free(child);
        }
    }

    ok = flush_dir(c, &d, self_first, err);
out:
    if (dp) {
        closedir(dp);
    }
    for (size_t i = 0; i < nnames; i++) {
        free(names[i]);
    }
    free(names);
    free(d.buf);
    free(d.sfns);
    return ok;
}

/* ---- boot sector / FSInfo / FAT --------------------------------------- */

static void build_boot_sector(uint8_t bs[SECTOR_SIZE], Fat32Ctx *c,
                              uint64_t total_sectors, uint32_t hidden,
                              const char *label)
{
    memset(bs, 0, SECTOR_SIZE);
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90;   /* jmp short +0x5a; nop */
    memcpy(bs + 3, "MSWIN4.1", 8);
    wr16(bs + 11, SECTOR_SIZE);                 /* BPB_BytsPerSec */
    bs[13] = (uint8_t)c->spc;                   /* BPB_SecPerClus */
    wr16(bs + 14, RSVD_SECTORS);                /* BPB_RsvdSecCnt */
    bs[16] = NUM_FATS;                          /* BPB_NumFATs */
    wr16(bs + 17, 0);                           /* BPB_RootEntCnt */
    wr16(bs + 19, 0);                           /* BPB_TotSec16 */
    bs[21] = 0xF8;                              /* BPB_Media */
    wr16(bs + 22, 0);                           /* BPB_FATSz16 */
    wr16(bs + 24, 63);                          /* BPB_SecPerTrk */
    wr16(bs + 26, 255);                         /* BPB_NumHeads */
    wr32(bs + 28, hidden);                      /* BPB_HiddSec */
    wr32(bs + 32, (uint32_t)total_sectors);     /* BPB_TotSec32 */
    wr32(bs + 36, c->fat_sectors);              /* BPB_FATSz32 */
    wr16(bs + 40, 0);                           /* BPB_ExtFlags */
    wr16(bs + 42, 0);                           /* BPB_FSVer */
    wr32(bs + 44, 2);                           /* BPB_RootClus */
    wr16(bs + 48, 1);                           /* BPB_FSInfo */
    wr16(bs + 50, BACKUP_BOOT_SEC);             /* BPB_BkBootSec */
    bs[64] = 0x80;                              /* BS_DrvNum */
    bs[66] = 0x29;                              /* BS_BootSig */
    wr32(bs + 67, (uint32_t)time(NULL));        /* BS_VolID */
    memset(bs + 71, ' ', 11);                   /* BS_VolLab */
    for (int i = 0; i < 11 && label[i]; i++) {
        bs[71 + i] = (uint8_t)label[i];
    }
    memcpy(bs + 82, "FAT32   ", 8);             /* BS_FilSysType */
    bs[510] = 0x55; bs[511] = 0xAA;
}

static void build_fsinfo(uint8_t fi[SECTOR_SIZE], uint32_t free_count,
                         uint32_t next_free)
{
    memset(fi, 0, SECTOR_SIZE);
    wr32(fi + 0, 0x41615252);      /* FSI_LeadSig */
    wr32(fi + 484, 0x61417272);    /* FSI_StrucSig */
    wr32(fi + 488, free_count);
    wr32(fi + 492, next_free);
    fi[510] = 0x55; fi[511] = 0xAA;
}

/* fatgen103 cluster-size table (same table pyfatfs used). */
static uint32_t pick_spc(uint64_t total_sectors)
{
    if (total_sectors <= 66600) {
        return 0;                       /* too small for FAT32 */
    }
    if (total_sectors <= 532480) {
        return 1;
    }
    if (total_sectors <= 16777216) {    /* up to 8 GiB: 4 KiB clusters */
        return 8;
    }
    if (total_sectors <= 33554432) {    /* up to 16 GiB: 8 KiB */
        return 16;
    }
    if (total_sectors <= 67108864) {    /* up to 32 GiB: 16 KiB */
        return 32;
    }
    return 64;                          /* above 32 GiB: 32 KiB */
}

bool gnw_fat32_build(gnw_sector_write_fn write, void *opaque,
                     uint64_t volume_lba, uint64_t volume_sectors,
                     uint32_t hidden_sectors, const char *label,
                     const char *content_dir, char **err)
{
    Fat32Ctx c = {
        .write = write,
        .opaque = opaque,
        .vol_lba = volume_lba,
    };
    uint8_t sect[SECTOR_SIZE];
    bool ok = false;

    if (err) {
        *err = NULL;
    }
    c.spc = pick_spc(volume_sectors);
    if (c.spc == 0) {
        set_err(err, "volume too small for FAT32 (%llu sectors)",
                (unsigned long long)volume_sectors);
        return false;
    }

    /* fatgen103 FAT-size formula (in sectors, unlike pyfatfs's bug). */
    {
        uint64_t tmp1 = volume_sectors - RSVD_SECTORS;
        uint64_t tmp2 = ((256 * c.spc) + NUM_FATS) / 2;
        c.fat_sectors = (uint32_t)((tmp1 + tmp2 - 1) / tmp2);
    }
    c.data_start = RSVD_SECTORS + NUM_FATS * c.fat_sectors;
    c.cluster_count = (uint32_t)((volume_sectors - c.data_start) / c.spc);
    c.next_free = 2;
    /*
     * Rounded up to a whole sector so the tail-sector write below never
     * reads past the allocation. Entries are kept in host order and
     * written raw: FAT32 is little-endian on disk and every host this
     * project targets (x86-64, arm64) is little-endian.
     */
    c.fat = calloc(((size_t)c.cluster_count + 2 + 127) & ~(size_t)127,
                   sizeof(uint32_t));
    if (!c.fat) {
        set_err(err, "out of memory (FAT of %u clusters)", c.cluster_count);
        return false;
    }
    c.fat[0] = 0x0FFFFFF8;
    c.fat[1] = FAT_EOC;

    /* Root directory: first allocation, so it lands on cluster 2. */
    uint32_t root = alloc_cluster(&c, err);
    if (root != 2 ||
        !populate_dir(&c, content_dir, root, 0, true, label, err)) {
        goto out;
    }

    /* Boot sector + FSInfo, primary and backup copies. */
    build_boot_sector(sect, &c, volume_sectors, hidden_sectors, label);
    if (!vwrite(&c, 0, sect, 1, err) ||
        !vwrite(&c, BACKUP_BOOT_SEC, sect, 1, err)) {
        goto out;
    }
    build_fsinfo(sect, c.cluster_count - (c.next_free - 2), c.next_free);
    if (!vwrite(&c, 1, sect, 1, err) ||
        !vwrite(&c, BACKUP_BOOT_SEC + 1, sect, 1, err)) {
        goto out;
    }

    /*
     * FATs: write only the sectors covering allocated entries; the tail
     * is all-zero and the underlying target is zero-initialized.
     */
    {
        uint32_t used_bytes = c.next_free * 4;
        uint32_t used_secs =
            (used_bytes + SECTOR_SIZE - 1) / SECTOR_SIZE;
        if (used_secs > c.fat_sectors) {
            used_secs = c.fat_sectors;
        }
        for (unsigned f = 0; f < NUM_FATS; f++) {
            if (!vwrite(&c, RSVD_SECTORS + (uint64_t)f * c.fat_sectors,
                        c.fat, used_secs, err)) {
                goto out;
            }
        }
    }
    ok = true;
out:
    free(c.fat);
    if (!ok) {
        set_err(err, "FAT32 build failed");
    }
    return ok;
}
