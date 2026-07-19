/*
 * Firmware-rewrite engine for the C port of gnwmanager's CFW patch pipeline
 * (gnwmanager/cli/gnw_patch/firmware.py + patch.py + device machinery,
 * remove-keystone-engine branch). Structural 1:1 port of gnw-web-builder's
 * already-verified TypeScript port (packages/gnw-patch/src/firmware.ts,
 * device.ts) -- read those files' own doc comments for the semantics being
 * mirrored here; this header tracks their structure closely to make the
 * correspondence auditable.
 *
 * Simplification vs. the TS/Python originals: buffers here are fixed-
 * capacity (sized once at construction, matching _common_prepare's one-time
 * `internal.extend()` and each firmware's real dumped size) rather than
 * fully growable-on-arbitrary-slice-assignment. Confirmed by inspecting
 * every real call site in mario.py/zelda.ts: every `replace`/`compress`/
 * `set_range`/`clear_range` operation writes a same-length region (the
 * "new" data's length always matches the range being replaced) -- the one
 * genuine exception is ExtFirmware.shorten(), which truncates the
 * *logical* length (ext_offset accounting), never grows it. So a fixed
 * backing allocation + a separate shrinkable `len` field covers every real
 * usage without needing full bytearray-style slice-resize semantics.
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
#ifndef GNW_CFW_ENGINE_H
#define GNW_CFW_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gnw_thumb_asm.h"
#include "gnw_symbols_mario.h"

/* ---- Lookup: uint32->uint32 relocation table, open addressing ---------- */
typedef struct {
    uint32_t *keys;
    uint32_t *vals;
    uint8_t *used;
    size_t cap;   /* power of 2 */
    size_t count;
} GnwLookup;

void gnw_lookup_init(GnwLookup *l);
void gnw_lookup_free(GnwLookup *l);
void gnw_lookup_set(GnwLookup *l, uint32_t key, uint32_t val);
/* Returns true and sets *val if found. */
bool gnw_lookup_get(const GnwLookup *l, uint32_t key, uint32_t *val);

/* ---- Firmware: one flat buffer + FLASH_BASE + patch primitives --------- */
typedef struct {
    uint8_t *data;
    size_t cap;   /* fixed allocation size */
    size_t len;   /* logical length, <= cap; only ExtFirmware.shorten() ever reduces it */
    uint32_t flash_base;
    GnwLookup *lookup;              /* shared with sibling firmwares via Device */
    const GnwSymbolEntry *symbols;  /* NULL if this firmware has no symbol table */
    size_t symbols_count;
} GnwFirmware;

/* error reporting: every operation that can fail sets *err (if non-NULL,
 * malloc'd, caller frees) and returns false/0/error sentinel as documented
 * per-function. Callers in this engine treat any false/negative return as
 * fatal (matching Python's raise-on-error semantics) and propagate up. */

void gnw_fw_init(GnwFirmware *fw, uint8_t *data, size_t cap, size_t len, uint32_t flash_base,
                  GnwLookup *lookup, const GnwSymbolEntry *symbols, size_t symbols_count);

uint32_t gnw_fw_int(const GnwFirmware *fw, size_t offset, int size);
bool gnw_fw_set_range(GnwFirmware *fw, size_t start, size_t end, uint8_t val, char **err);
bool gnw_fw_clear_range(GnwFirmware *fw, size_t start, size_t end, char **err);
bool gnw_fw_address(const GnwFirmware *fw, const char *symbol, bool sub_base, uint32_t *out, char **err);

bool gnw_fw_replace_bytes(GnwFirmware *fw, size_t offset, const uint8_t *data, size_t len, char **err);
bool gnw_fw_replace_symbol(GnwFirmware *fw, size_t offset, const char *symbol, char **err);
bool gnw_fw_replace_int(GnwFirmware *fw, size_t offset, uint32_t data, int size, char **err);
bool gnw_fw_relative_symbol(GnwFirmware *fw, size_t offset, const char *symbol, char **err);
bool gnw_fw_relative_int(GnwFirmware *fw, size_t offset, uint32_t data, char **err);
bool gnw_fw_b(GnwFirmware *fw, size_t offset, uint32_t data, char **err);
bool gnw_fw_bl_symbol(GnwFirmware *fw, size_t offset, const char *symbol, char **err);
bool gnw_fw_bl_int(GnwFirmware *fw, size_t offset, uint32_t data, char **err);
bool gnw_fw_asm(GnwFirmware *fw, size_t offset, const char *code, char **err);
bool gnw_fw_nop(GnwFirmware *fw, size_t offset, int count, char **err);
/* same-firmware move/copy; del=true clears the source range after copying */
bool gnw_fw_move_copy(GnwFirmware *fw, size_t offset, int32_t data, size_t size, bool del, char **err);
bool gnw_fw_add(GnwFirmware *fw, size_t offset, uint32_t data, int size, char **err);
/* compress [offset,offset+size) in place via LZMA1 (gnw_lzma), returns compressed length or 0 on error */
size_t gnw_fw_compress(GnwFirmware *fw, size_t offset, size_t size, char **err);
bool gnw_fw_lookup_ref(GnwFirmware *fw, size_t offset, char **err);
bool gnw_fw_lookup_refs(GnwFirmware *fw, const size_t *offsets, size_t n, char **err);

/* ---- RWData: the rwdata-init table (lz77 decompress on load, LZMA on write) */
#define GNW_RWDATA_MAX_ELEMENTS 5

typedef struct {
    GnwFirmware *firmware; /* internal firmware this table lives in */
    size_t table_start;
    uint8_t *datas[GNW_RWDATA_MAX_ELEMENTS];
    size_t data_lens[GNW_RWDATA_MAX_ELEMENTS];
    uint32_t dsts[GNW_RWDATA_MAX_ELEMENTS];
    size_t n;
    uint32_t last_fn;
} GnwRWData;

bool gnw_rwdata_load(GnwRWData *rw, GnwFirmware *firmware, size_t table_start, size_t table_len, char **err);
void gnw_rwdata_free(GnwRWData *rw);
size_t gnw_rwdata_table_end(const GnwRWData *rw);
bool gnw_rwdata_append(GnwRWData *rw, const uint8_t *data, size_t len, uint32_t dst, char **err);
/* sum of each entry's compressed length (no memoization -- see .c for why that's fine) */
bool gnw_rwdata_compressed_len(GnwRWData *rw, size_t *out, char **err);
bool gnw_rwdata_write_table_and_data(GnwRWData *rw, size_t end_of_table_reference, size_t data_offset,
                                      size_t *out_total_len, char **err);

/* ---- Device: internal + external + compressed_memory + shared lookup --- */
typedef struct {
    GnwFirmware internal;
    GnwFirmware external;
    GnwFirmware compressed_memory;
    GnwLookup lookup;
    GnwRWData rwdata;
    bool has_rwdata;

    int32_t ext_offset;
    size_t int_pos;
    size_t compressed_memory_pos;

    /* args (subset actually referenced by mario.py/zelda.py patch()) */
    double compression_ratio;
    bool disable_sleep;
    int sleep_time;         /* 0 = not set */
    bool no_save;
    bool no_mario_song;
    bool no_sleep_images;
    bool no_smb2;
    bool no_la;
    bool no_second_beep;
    bool no_hour_tune;
} GnwDevice;

bool gnw_device_crypt(GnwDevice *d, const uint8_t key[16], const uint8_t nonce[8],
                       size_t enc_start, size_t enc_end, char **err);
size_t gnw_device_compressed_memory_compressed_len(GnwDevice *d, size_t add_index, char **err);
size_t gnw_device_int_free_space(GnwDevice *d, char **err);
/* idx = RWDATA_DTCM_IDX for the specific game (mario=1, zelda's driver passes its own) */
bool gnw_device_rwdata_lookup_idx(GnwDevice *d, int idx, size_t lower, size_t size, char **err);
bool gnw_device_rwdata_erase_idx(GnwDevice *d, int idx, size_t lower, size_t size, char **err);

/* moveExt: returns new location (in internal) or (uint32_t)-1 on error via *err */
uint32_t gnw_device_move_to_int(GnwDevice *d, uint32_t ext_or_neg1, const uint8_t *ext_bytes, size_t ext_bytes_len,
                                 size_t size, const size_t *refs, size_t n_refs, char **err);
uint32_t gnw_device_move_ext(GnwDevice *d, uint32_t ext, size_t size, const size_t *refs, size_t n_refs, char **err);
uint32_t gnw_device_move_to_compressed_memory(GnwDevice *d, uint32_t ext, size_t size,
                                               const size_t *refs, size_t n_refs, char **err);

#endif
