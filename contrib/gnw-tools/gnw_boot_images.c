/*
 * See gnw_boot_images.h.
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
#include "gnw_boot_images.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <glib/gstdio.h>

static char *xstrdup_printf(const char *fmt, ...)
    __attribute__((format(gnu_printf, 1, 2)));

static char *xstrdup_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int len = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (len < 0) {
        va_end(ap2);
        return NULL;
    }
    char *buf = malloc((size_t)len + 1);
    if (buf) {
        vsnprintf(buf, (size_t)len + 1, fmt, ap2);
    }
    va_end(ap2);
    return buf;
}

/*
 * Evaluates a small "N * N * N ..." integer expression, e.g. "256 * 1024"
 * or "64 * 1024 * 1024" -- exactly the shape gnw_h7b0_soc.h's own
 * FLASH_BANK_SIZE/EXTFLASH_SIZE #defines use. A single enclosing pair of
 * parens is stripped first if present. Supports decimal and 0x-hex terms.
 */
static bool eval_mul_expr(const char *expr, unsigned long long *out)
{
    char buf[256];
    size_t len = strlen(expr);
    /* Trim surrounding whitespace. */
    while (len > 0 && (unsigned char)expr[len - 1] <= ' ') {
        len--;
    }
    while (*expr != '\0' && (unsigned char)*expr <= ' ') {
        expr++;
        len--;
    }
    if (len == 0 || len >= sizeof(buf)) {
        return false;
    }
    memcpy(buf, expr, len);
    buf[len] = '\0';

    /* Strip one enclosing "(...)" pair if the whole expression is wrapped. */
    if (buf[0] == '(' && buf[len - 1] == ')') {
        memmove(buf, buf + 1, len - 2);
        buf[len - 2] = '\0';
    }

    unsigned long long result = 1;
    char *saveptr = NULL;
    char *tok = strtok_r(buf, "*", &saveptr);
    if (!tok) {
        return false;
    }
    while (tok) {
        char *end = NULL;
        errno = 0;
        unsigned long long v = strtoull(tok, &end, 0);
        if (errno != 0 || end == tok) {
            return false;
        }
        /* allow trailing whitespace after the numeral */
        while (*end != '\0' && (unsigned char)*end <= ' ') {
            end++;
        }
        if (*end != '\0') {
            return false;
        }
        result *= v;
        tok = strtok_r(NULL, "*", &saveptr);
    }
    *out = result;
    return true;
}

static bool read_define(const char *header_path, const char *name,
                         unsigned long long *out, char **error_msg)
{
    FILE *f = fopen(header_path, "r");
    if (!f) {
        if (error_msg) {
            *error_msg = xstrdup_printf("could not open %s: %s", header_path,
                                        strerror(errno));
        }
        return false;
    }

    char line[512];
    char needle[256];
    snprintf(needle, sizeof(needle), "#define %s", name);
    size_t needle_len = strlen(needle);
    bool found = false;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, needle, needle_len) != 0) {
            continue;
        }
        /* Next char after the name must be whitespace (avoid matching a
         * #define whose name is a prefix of `name`, e.g. FOO vs FOOBAR). */
        char after = line[needle_len];
        if (after != ' ' && after != '\t') {
            continue;
        }
        if (eval_mul_expr(line + needle_len, out)) {
            found = true;
        }
        break;
    }
    fclose(f);

    if (!found && error_msg) {
        *error_msg = xstrdup_printf("could not find/parse #define %s in %s",
                                    name, header_path);
    }
    return found;
}

static bool read_whole_file(const char *path, unsigned char **data,
                             size_t *size, char **error_msg)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (error_msg) {
            *error_msg = xstrdup_printf("missing %s -- dump it from real "
                                        "hardware first (`gnwmanager dump`), "
                                        "then place it there", path);
        }
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        if (error_msg) {
            *error_msg = xstrdup_printf("could not seek %s: %s", path,
                                        strerror(errno));
        }
        return false;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        if (error_msg) {
            *error_msg = xstrdup_printf("could not tell %s: %s", path,
                                        strerror(errno));
        }
        return false;
    }
    rewind(f);

    unsigned char *buf = malloc((size_t)sz > 0 ? (size_t)sz : 1);
    if (!buf) {
        fclose(f);
        if (error_msg) {
            *error_msg = xstrdup_printf("out of memory reading %s", path);
        }
        return false;
    }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(buf);
        if (error_msg) {
            *error_msg = xstrdup_printf("short read on %s", path);
        }
        return false;
    }
    fclose(f);

    *data = buf;
    *size = (size_t)sz;
    return true;
}

static bool write_padded_ff(const char *out_path, const unsigned char *data,
                             size_t data_size, unsigned long long target_size,
                             const char *label, char **error_msg)
{
    if ((unsigned long long)data_size > target_size) {
        if (error_msg) {
            *error_msg = xstrdup_printf(
                "%s: source is %zu bytes, larger than target size %llu",
                label, data_size, target_size);
        }
        return false;
    }

    FILE *f = fopen(out_path, "wb");
    if (!f) {
        if (error_msg) {
            *error_msg = xstrdup_printf("could not create %s: %s", out_path,
                                        strerror(errno));
        }
        return false;
    }

    bool ok = true;
    if (data_size > 0 && fwrite(data, 1, data_size, f) != data_size) {
        ok = false;
    }
    unsigned long long pad = target_size - (unsigned long long)data_size;
    if (ok && pad > 0) {
        unsigned char ffbuf[8192];
        memset(ffbuf, 0xFF, sizeof(ffbuf));
        while (ok && pad > 0) {
            size_t chunk = pad < sizeof(ffbuf) ? (size_t)pad : sizeof(ffbuf);
            if (fwrite(ffbuf, 1, chunk, f) != chunk) {
                ok = false;
                break;
            }
            pad -= chunk;
        }
    }
    fclose(f);

    if (!ok && error_msg) {
        *error_msg = xstrdup_printf("write error on %s", out_path);
    }
    return ok;
}

static bool ensure_dir(const char *path, char **error_msg)
{
    if (g_mkdir(path, 0755) != 0 && errno != EEXIST) {
        if (error_msg) {
            *error_msg = xstrdup_printf("could not create directory %s: %s",
                                        path, strerror(errno));
        }
        return false;
    }
    return true;
}

bool gnw_make_boot_images(const char *game, const char *repo_root,
                           char **error_msg)
{
    if (error_msg) {
        *error_msg = NULL;
    }

    char *header_path = xstrdup_printf("%s/include/hw/arm/gnw_h7b0_soc.h",
                                        repo_root);

    unsigned long long bank_size = 0, extflash_size = 0;
    bool ok = read_define(header_path, "FLASH_BANK_SIZE", &bank_size,
                           error_msg);
    if (ok) {
        ok = read_define(header_path, "EXTFLASH_SIZE", &extflash_size,
                          error_msg);
    }
    free(header_path);
    if (!ok) {
        return false;
    }

    char *backup_dir = xstrdup_printf("%s/backup", repo_root);
    char *out_dir = xstrdup_printf("%s/backup/qemu-images", repo_root);
    ok = ensure_dir(out_dir, error_msg);

    char *internal_src = NULL, *extflash_src = NULL;
    unsigned char *internal_data = NULL, *extflash_data = NULL;
    size_t internal_len = 0, extflash_len = 0;
    if (ok) {
        internal_src = xstrdup_printf("%s/internal_flash_backup_%s.bin",
                                       backup_dir, game);
        extflash_src = xstrdup_printf("%s/flash_backup_%s.bin", backup_dir,
                                       game);
        ok = read_whole_file(internal_src, &internal_data, &internal_len,
                              error_msg);
    }
    if (ok) {
        ok = read_whole_file(extflash_src, &extflash_data, &extflash_len,
                              error_msg);
    }

    char *bank1_out = xstrdup_printf("%s/%s-bank1.bin", out_dir, game);
    char *bank2_out = xstrdup_printf("%s/%s-bank2.bin", out_dir, game);
    char *extflash_out = xstrdup_printf("%s/%s-extflash.bin", out_dir, game);

    if (ok) {
        ok = write_padded_ff(bank1_out, internal_data, internal_len,
                              bank_size, "bank1", error_msg);
    }
    if (ok) {
        /* bank2: no real dump exists for stock firmware -- entirely 0xFF. */
        ok = write_padded_ff(bank2_out, NULL, 0, bank_size, "bank2",
                              error_msg);
    }
    if (ok) {
        ok = write_padded_ff(extflash_out, extflash_data, extflash_len,
                              extflash_size, "extflash", error_msg);
    }

    free(backup_dir);
    free(out_dir);
    free(internal_src);
    free(extflash_src);
    free(bank1_out);
    free(bank2_out);
    free(extflash_out);

    free(internal_data);
    free(extflash_data);
    return ok;
}
