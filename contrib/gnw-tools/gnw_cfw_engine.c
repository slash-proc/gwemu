/*
 * See gnw_cfw_engine.h for scope/rationale.
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
#include "gnw_cfw_engine.h"
#include "gnw_aes128.h"
#include "gnw_lz77.h"
#include "gnw_lzma.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *errdup(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static char *errdup(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return strdup(buf);
}

/* ---- Lookup ------------------------------------------------------------- */
void gnw_lookup_init(GnwLookup *l)
{
    l->cap = 1024;
    l->count = 0;
    l->keys = calloc(l->cap, sizeof(uint32_t));
    l->vals = calloc(l->cap, sizeof(uint32_t));
    l->used = calloc(l->cap, 1);
}

void gnw_lookup_free(GnwLookup *l)
{
    free(l->keys); free(l->vals); free(l->used);
    l->keys = NULL; l->vals = NULL; l->used = NULL; l->cap = l->count = 0;
}

static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static void lookup_grow(GnwLookup *l);

void gnw_lookup_set(GnwLookup *l, uint32_t key, uint32_t val)
{
    if (l->count * 4 >= l->cap * 3) { /* load factor 0.75 */
        lookup_grow(l);
    }
    size_t mask = l->cap - 1;
    size_t i = hash32(key) & mask;
    while (l->used[i]) {
        if (l->keys[i] == key) { l->vals[i] = val; return; }
        i = (i + 1) & mask;
    }
    l->used[i] = 1;
    l->keys[i] = key;
    l->vals[i] = val;
    l->count++;
}

bool gnw_lookup_get(const GnwLookup *l, uint32_t key, uint32_t *val)
{
    size_t mask = l->cap - 1;
    size_t i = hash32(key) & mask;
    while (l->used[i]) {
        if (l->keys[i] == key) { *val = l->vals[i]; return true; }
        i = (i + 1) & mask;
    }
    return false;
}

static void lookup_grow(GnwLookup *l)
{
    GnwLookup nl;
    nl.cap = l->cap * 2;
    nl.count = 0;
    nl.keys = calloc(nl.cap, sizeof(uint32_t));
    nl.vals = calloc(nl.cap, sizeof(uint32_t));
    nl.used = calloc(nl.cap, 1);
    for (size_t i = 0; i < l->cap; i++) {
        if (l->used[i]) {
            gnw_lookup_set(&nl, l->keys[i], l->vals[i]);
        }
    }
    free(l->keys); free(l->vals); free(l->used);
    *l = nl;
}

/* ---- Firmware ------------------------------------------------------------ */
void gnw_fw_init(GnwFirmware *fw, uint8_t *data, size_t cap, size_t len, uint32_t flash_base,
                  GnwLookup *lookup, const GnwSymbolEntry *symbols, size_t symbols_count)
{
    fw->data = data;
    fw->cap = cap;
    fw->len = len;
    fw->flash_base = flash_base;
    fw->lookup = lookup;
    fw->symbols = symbols;
    fw->symbols_count = symbols_count;
}

uint32_t gnw_fw_int(const GnwFirmware *fw, size_t offset, int size)
{
    uint32_t v = 0;
    for (int i = 0; i < size; i++) {
        v |= ((uint32_t)fw->data[offset + i]) << (8 * i);
    }
    return v;
}

bool gnw_fw_set_range(GnwFirmware *fw, size_t start, size_t end, uint8_t val, char **err)
{
    if (end > fw->len) { if (err) *err = errdup("set_range: end %zu exceeds length %zu", end, fw->len); return false; }
    memset(fw->data + start, val, end - start);
    return true;
}

bool gnw_fw_clear_range(GnwFirmware *fw, size_t start, size_t end, char **err)
{
    return gnw_fw_set_range(fw, start, end, 0, err);
}

bool gnw_fw_address(const GnwFirmware *fw, const char *symbol, bool sub_base, uint32_t *out, char **err)
{
    for (size_t i = 0; i < fw->symbols_count; i++) {
        if (strcmp(fw->symbols[i].name, symbol) == 0) {
            uint32_t a = fw->symbols[i].addr;
            if (a == 0) break; /* matches Python: address==0 -> MissingSymbolError */
            *out = sub_base ? (a - fw->flash_base) : a;
            return true;
        }
    }
    if (err) *err = errdup("Cannot find symbol \"%s\"", symbol);
    return false;
}

bool gnw_fw_replace_bytes(GnwFirmware *fw, size_t offset, const uint8_t *data, size_t len, char **err)
{
    if (offset >= fw->len) { if (err) *err = errdup("replace: offset %zu exceeds length %zu", offset, fw->len); return false; }
    if (offset + len > fw->len) { if (err) *err = errdup("replace: end exceeds length"); return false; }
    memcpy(fw->data + offset, data, len);
    return true;
}

bool gnw_fw_replace_symbol(GnwFirmware *fw, size_t offset, const char *symbol, char **err)
{
    uint32_t addr;
    if (!gnw_fw_address(fw, symbol, false, &addr, err)) return false;
    uint8_t b[4] = { (uint8_t)addr, (uint8_t)(addr>>8), (uint8_t)(addr>>16), (uint8_t)(addr>>24) };
    return gnw_fw_replace_bytes(fw, offset, b, 4, err);
}

bool gnw_fw_replace_int(GnwFirmware *fw, size_t offset, uint32_t data, int size, char **err)
{
    uint8_t b[4];
    for (int i = 0; i < size; i++) b[i] = (uint8_t)(data >> (8*i));
    return gnw_fw_replace_bytes(fw, offset, b, (size_t)size, err);
}

bool gnw_fw_relative_symbol(GnwFirmware *fw, size_t offset, const char *symbol, char **err)
{
    uint32_t dst;
    if (!gnw_fw_address(fw, symbol, false, &dst, err)) return false;
    uint32_t src = fw->flash_base + (uint32_t)offset;
    uint32_t rel = dst - src; /* wraps mod 2^32, matches Python's explicit +0x100000000 on negative */
    return gnw_fw_replace_int(fw, offset, rel, 4, err);
}

bool gnw_fw_relative_int(GnwFirmware *fw, size_t offset, uint32_t data, char **err)
{
    uint32_t src = fw->flash_base + (uint32_t)offset;
    uint32_t dst = data < fw->flash_base ? data + fw->flash_base : data;
    uint32_t rel = dst - src;
    return gnw_fw_replace_int(fw, offset, rel, 4, err);
}

bool gnw_fw_b(GnwFirmware *fw, size_t offset, uint32_t data, char **err)
{
    int64_t pc = (int64_t)offset + 4;
    int64_t signed_jump = (int64_t)data - pc;
    int64_t absjump = signed_jump < 0 ? -signed_jump : signed_jump;
    if (absjump > (2 * (1 << 10))) { if (err) *err = errdup("b: jump too large"); return false; }
    int32_t j = (int32_t)(signed_jump >> 1);
    uint32_t j11 = (uint32_t)(j & 0x7FF); /* two's complement in 11 bits */
    uint8_t byte0 = (uint8_t)(0xE0 | ((j11 >> 8) & 0x7));
    uint8_t byte1 = (uint8_t)(j11 & 0xFF);
    fw->data[offset + 0] = byte1;
    fw->data[offset + 1] = byte0;
    return true;
}

static bool bl_encode(uint32_t flash_base, size_t offset, uint32_t dst_address, uint8_t out[4], char **err)
{
    int64_t pc = (int64_t)flash_base + (int64_t)offset + 4;
    int64_t jump = (int64_t)dst_address - pc;
    int64_t absjump = jump < 0 ? -jump : jump;
    if (absjump > (4 * (1 << 20))) { if (err) *err = errdup("bl: jump too large"); return false; }

    int32_t offset_stage_1 = (int32_t)(jump >> 12);
    uint32_t s1_11 = (uint32_t)(offset_stage_1 & 0x7FF);
    uint8_t s1b0 = (uint8_t)(0xF0 | ((s1_11 >> 8) & 0x7));
    uint8_t s1b1 = (uint8_t)(s1_11 & 0xFF);

    int32_t offset_stage_2 = (int32_t)((jump - ((int64_t)offset_stage_1 << 12)) >> 1);
    if (offset_stage_2 >> 11) { if (err) *err = errdup("bl: jump 0x%x too large", (unsigned)jump); return false; }
    uint8_t s2b0 = (uint8_t)(0xF8 | ((offset_stage_2 >> 8) & 0x7));
    uint8_t s2b1 = (uint8_t)(offset_stage_2 & 0xFF);

    out[0] = s1b1; out[1] = s1b0; out[2] = s2b1; out[3] = s2b0;
    return true;
}

bool gnw_fw_bl_symbol(GnwFirmware *fw, size_t offset, const char *symbol, char **err)
{
    uint32_t dst;
    if (!gnw_fw_address(fw, symbol, false, &dst, err)) return false;
    uint8_t out[4];
    if (!bl_encode(fw->flash_base, offset, dst, out, err)) return false;
    return gnw_fw_replace_bytes(fw, offset, out, 4, err);
}

bool gnw_fw_bl_int(GnwFirmware *fw, size_t offset, uint32_t data, char **err)
{
    uint32_t dst = fw->flash_base + data;
    uint8_t out[4];
    if (!bl_encode(fw->flash_base, offset, dst, out, err)) return false;
    return gnw_fw_replace_bytes(fw, offset, out, 4, err);
}

bool gnw_fw_asm(GnwFirmware *fw, size_t offset, const char *code, char **err)
{
    uint8_t *out; size_t out_len; char *asm_err = NULL;
    /* thumb_asm's addr param only matters for b.w forms; always pass flash_base+offset,
     * matching patch.py's asm() (which passes it unconditionally for b.w, and thumb_asm
     * ignores it for forms that don't need it). */
    if (!gnw_thumb_assemble(code, fw->flash_base + (uint32_t)offset, &out, &out_len, &asm_err)) {
        if (err) *err = asm_err ? asm_err : strdup("asm failed");
        return false;
    }
    bool ok = gnw_fw_replace_bytes(fw, offset, out, out_len, err);
    free(out);
    return ok;
}

bool gnw_fw_nop(GnwFirmware *fw, size_t offset, int count, char **err)
{
    size_t size = (size_t)count * 2;
    if (offset + size > fw->len) { if (err) *err = errdup("nop: exceeds length"); return false; }
    for (int i = 0; i < count; i++) {
        fw->data[offset + i*2 + 0] = 0x00;
        fw->data[offset + i*2 + 1] = 0xbf;
    }
    return true;
}

bool gnw_fw_move_copy(GnwFirmware *fw, size_t offset, int32_t data, size_t size, bool del, char **err)
{
    size_t old_start = offset;
    size_t old_end = old_start + size;
    size_t new_start = (size_t)((int64_t)offset + data);
    size_t new_end = new_start + size;
    if (old_end > fw->len || new_end > fw->len) { if (err) *err = errdup("move_copy: out of range"); return false; }

    /* Python does self[new_start:new_end] = self[old_start:old_end] -- if
     * regions overlap this must behave like memmove (copy old bytes first),
     * not memcpy. */
    uint8_t *tmp = malloc(size);
    memcpy(tmp, fw->data + old_start, size);
    memcpy(fw->data + new_start, tmp, size);
    free(tmp);

    if (del) {
        if (data < 0) {
            if (new_end > offset) { if (!gnw_fw_clear_range(fw, new_end, old_end, err)) return false; }
            else { if (!gnw_fw_clear_range(fw, old_start, old_end, err)) return false; }
        } else {
            if (new_start < old_end) { if (!gnw_fw_clear_range(fw, old_start, new_start, err)) return false; }
            else { if (!gnw_fw_clear_range(fw, old_start, old_end, err)) return false; }
        }
    }
    for (size_t i = 0; i < size; i++) {
        gnw_lookup_set(fw->lookup, fw->flash_base + (uint32_t)old_start + (uint32_t)i,
                                    fw->flash_base + (uint32_t)new_start + (uint32_t)i);
    }
    return true;
}

bool gnw_fw_add(GnwFirmware *fw, size_t offset, uint32_t data, int size, char **err)
{
    uint32_t val = gnw_fw_int(fw, offset, size) + data;
    return gnw_fw_replace_int(fw, offset, val, size, err);
}

size_t gnw_fw_compress(GnwFirmware *fw, size_t offset, size_t size, char **err)
{
    uint8_t *out; size_t out_len; char *lzma_err = NULL;
    if (!gnw_lzma_compress(fw->data + offset, size, &out, &out_len, &lzma_err)) {
        if (err) *err = lzma_err ? lzma_err : strdup("lzma compress failed");
        return 0;
    }
    if (!gnw_fw_clear_range(fw, offset, offset + size, err)) { free(out); return 0; }
    if (!gnw_fw_replace_bytes(fw, offset, out, out_len, err)) { free(out); return 0; }
    free(out);
    return out_len;
}

bool gnw_fw_lookup_ref(GnwFirmware *fw, size_t offset, char **err)
{
    uint32_t val = gnw_fw_int(fw, offset, 4);
    uint32_t nv;
    if (!gnw_lookup_get(fw->lookup, val, &nv)) {
        if (err) *err = errdup("lookup: 0x%08X at offset 0x%zX not found", val, offset);
        return false;
    }
    return gnw_fw_replace_int(fw, offset, nv, 4, err);
}

bool gnw_fw_lookup_refs(GnwFirmware *fw, const size_t *offsets, size_t n, char **err)
{
    for (size_t i = 0; i < n; i++) {
        if (!gnw_fw_lookup_ref(fw, offsets[i], err)) return false;
    }
    return true;
}

/* ---- RWData --------------------------------------------------------------- */
bool gnw_rwdata_load(GnwRWData *rw, GnwFirmware *firmware, size_t table_start, size_t table_len, char **err)
{
    memset(rw, 0, sizeof(*rw));
    rw->firmware = firmware;
    rw->table_start = table_start;

    size_t base = table_start;
    while (base < table_start + table_len - 4) {
        size_t i = base;
        i += 4; /* fn ptr, replaced wholesale on write */
        uint32_t data_addr = (uint32_t)i + gnw_fw_int(firmware, i, 4);
        i += 4;
        uint32_t data_len = gnw_fw_int(firmware, i, 4) >> 1;
        i += 4;
        uint32_t data_dst = gnw_fw_int(firmware, i, 4);
        i += 4;

        uint8_t *decoded; size_t decoded_len; char *lz_err = NULL;
        if (!gnw_lz77_decompress(firmware->data + data_addr, data_len, &decoded, &decoded_len, &lz_err)) {
            if (err) *err = lz_err ? lz_err : strdup("rwdata: lz77 decode failed");
            return false;
        }
        if (!gnw_fw_clear_range(firmware, data_addr, data_addr + data_len, err)) { free(decoded); return false; }
        if (!gnw_rwdata_append(rw, decoded, decoded_len, data_dst, err)) { free(decoded); return false; }
        free(decoded);

        base += 16;
    }

    size_t last_element_offset = table_start + table_len - 4;
    int32_t last_fn = (int32_t)gnw_fw_int(firmware, last_element_offset, 4);
    rw->last_fn = (uint32_t)((int64_t)last_fn + (int64_t)last_element_offset);

    if (!gnw_fw_set_range(firmware, table_start, table_start + 16 * GNW_RWDATA_MAX_ELEMENTS + 4, 0x77, err)) return false;
    return true;
}

void gnw_rwdata_free(GnwRWData *rw)
{
    for (size_t i = 0; i < rw->n; i++) free(rw->datas[i]);
}

size_t gnw_rwdata_table_end(const GnwRWData *rw)
{
    return rw->table_start + 4 * 4 * rw->n + 4 + 4;
}

bool gnw_rwdata_append(GnwRWData *rw, const uint8_t *data, size_t len, uint32_t dst, char **err)
{
    if (rw->n >= GNW_RWDATA_MAX_ELEMENTS) { if (err) *err = errdup("rwdata: MAX_TABLE_ELEMENTS exceeded"); return false; }
    uint8_t *copy = malloc(len ? len : 1);
    memcpy(copy, data, len);
    rw->datas[rw->n] = copy;
    rw->data_lens[rw->n] = len;
    rw->dsts[rw->n] = dst;
    rw->n++;
    return true;
}

bool gnw_rwdata_compressed_len(GnwRWData *rw, size_t *out, char **err)
{
    size_t total = 0;
    for (size_t k = 0; k < rw->n; k++) {
        uint8_t *c; size_t c_len; char *lzma_err = NULL;
        if (!gnw_lzma_compress(rw->datas[k], rw->data_lens[k], &c, &c_len, &lzma_err)) {
            if (err) *err = lzma_err ? lzma_err : strdup("rwdata compressed_len: lzma failed");
            return false;
        }
        free(c);
        total += c_len;
    }
    *out = total;
    return true;
}

bool gnw_rwdata_write_table_and_data(GnwRWData *rw, size_t end_of_table_reference, size_t data_offset,
                                      size_t *out_total_len, char **err)
{
    size_t data_addrs[GNW_RWDATA_MAX_ELEMENTS], data_lens[GNW_RWDATA_MAX_ELEMENTS];
    size_t index = data_offset;
    size_t total_len = 0;

    for (size_t k = 0; k < rw->n; k++) {
        uint8_t *c; size_t c_len; char *lzma_err = NULL;
        if (!gnw_lzma_compress(rw->datas[k], rw->data_lens[k], &c, &c_len, &lzma_err)) {
            if (err) *err = lzma_err ? lzma_err : strdup("rwdata write: lzma failed");
            return false;
        }
        if (!gnw_fw_replace_bytes(rw->firmware, index, c, c_len, err)) { free(c); return false; }
        free(c);
        data_addrs[k] = index;
        data_lens[k] = c_len;
        index += c_len;
        total_len += c_len;
    }

    index = rw->table_start;
    for (size_t k = 0; k < rw->n; k++) {
        if (!gnw_fw_relative_symbol(rw->firmware, index, "rwdata_inflate", err)) return false;
        index += 4;

        uint32_t rel_addr = (uint32_t)data_addrs[k] - (uint32_t)index;
        if (!gnw_fw_replace_int(rw->firmware, index, rel_addr, 4, err)) return false;
        index += 4;

        if (!gnw_fw_replace_int(rw->firmware, index, (uint32_t)data_lens[k], 4, err)) return false;
        index += 4;

        if (!gnw_fw_replace_int(rw->firmware, index, rw->dsts[k], 4, err)) return false;
        index += 4;
    }

    if (!gnw_fw_relative_symbol(rw->firmware, index, "bss_rwdata_init", err)) return false;
    index += 4;
    if (!gnw_fw_relative_int(rw->firmware, index, rw->last_fn, err)) return false;
    index += 4;

    if (index != gnw_rwdata_table_end(rw)) { if (err) *err = errdup("rwdata: table_end mismatch"); return false; }

    if (!gnw_fw_relative_int(rw->firmware, end_of_table_reference, (uint32_t)index, err)) return false;

    *out_total_len = total_len;
    return true;
}

/* ---- Device ----------------------------------------------------------------- */
bool gnw_device_crypt(GnwDevice *d, const uint8_t key[16], const uint8_t nonce[8],
                       size_t enc_start, size_t enc_end, char **err)
{
    (void)err;
    uint8_t key_r[16];
    for (int i = 0; i < 16; i++) key_r[i] = key[15 - i];
    GnwAes128Ctx ctx;
    gnw_aes128_init(&ctx, key_r);

    uint8_t iv[16];
    for (int i = 0; i < 8; i++) iv[i] = nonce[7 - i];
    iv[8]=0x00; iv[9]=0x00; iv[10]=0x71; iv[11]=0x23; iv[12]=0x20; iv[13]=0x00; iv[14]=0x00; iv[15]=0x00;

    GnwFirmware *ext = &d->external;
    for (size_t offset = enc_start; offset < enc_end; offset += 16) {
        uint8_t counter_block[16];
        memcpy(counter_block, iv, 16);
        uint32_t counter = (ext->flash_base + (uint32_t)offset) >> 4;
        counter_block[12] = (uint8_t)(((counter >> 24) & 0x0F) | (counter_block[12] & 0xF0));
        counter_block[13] = (uint8_t)((counter >> 16) & 0xFF);
        counter_block[14] = (uint8_t)((counter >> 8) & 0xFF);
        counter_block[15] = (uint8_t)(counter & 0xFF);

        uint8_t cipher[16];
        gnw_aes128_encrypt_block(&ctx, counter_block, cipher);
        for (int i = 0; i < 16; i++) {
            ext->data[offset + i] ^= cipher[15 - i];
        }
    }
    return true;
}

size_t gnw_device_compressed_memory_compressed_len(GnwDevice *d, size_t add_index, char **err)
{
    size_t index = d->compressed_memory_pos + add_index;
    if (index == 0) return 0;
    uint8_t *c; size_t c_len; char *lzma_err = NULL;
    if (!gnw_lzma_compress(d->compressed_memory.data, index, &c, &c_len, &lzma_err)) {
        if (err) *err = lzma_err ? lzma_err : strdup("compressed_memory_compressed_len: lzma failed");
        return 0;
    }
    free(c);
    return c_len;
}

size_t gnw_device_int_free_space(GnwDevice *d, char **err)
{
    size_t cmem_len = gnw_device_compressed_memory_compressed_len(d, 0, err);
    if (err && *err) return 0;
    size_t out = d->internal.len - d->int_pos - cmem_len;
    if (d->has_rwdata) {
        size_t rw_len;
        if (!gnw_rwdata_compressed_len(&d->rwdata, &rw_len, err)) return 0;
        out -= rw_len;
    }
    return out;
}

bool gnw_device_rwdata_lookup_idx(GnwDevice *d, int idx, size_t lower, size_t size, char **err)
{
    (void)err;
    uint8_t *arr = d->rwdata.datas[idx];
    size_t arr_len = d->rwdata.data_lens[idx];
    uint32_t lo = (uint32_t)lower + d->external.flash_base;
    uint32_t hi = lo + (uint32_t)size;
    for (size_t i = 0; i + 4 <= arr_len; i += 4) {
        uint32_t val = arr[i] | (arr[i+1]<<8) | (arr[i+2]<<16) | (arr[i+3]<<24);
        if (val >= lo && val < hi) {
            uint32_t nv;
            if (gnw_lookup_get(&d->lookup, val, &nv)) {
                arr[i] = (uint8_t)nv; arr[i+1]=(uint8_t)(nv>>8); arr[i+2]=(uint8_t)(nv>>16); arr[i+3]=(uint8_t)(nv>>24);
            }
        }
    }
    return true;
}

bool gnw_device_rwdata_erase_idx(GnwDevice *d, int idx, size_t lower, size_t size, char **err)
{
    (void)err;
    uint8_t *arr = d->rwdata.datas[idx];
    size_t arr_len = d->rwdata.data_lens[idx];
    uint32_t lo = (uint32_t)lower + 0x90000000u;
    uint32_t hi = lo + (uint32_t)size;
    for (size_t i = 0; i + 4 <= arr_len; i += 4) {
        uint32_t val = arr[i] | (arr[i+1]<<8) | (arr[i+2]<<16) | (arr[i+3]<<24);
        if (val >= lo && val < hi) {
            arr[i]=arr[i+1]=arr[i+2]=arr[i+3]=0;
        }
    }
    return true;
}

static bool device_move_copy_cross(GnwDevice *d, GnwFirmware *dst, size_t dst_offset, GnwFirmware *src,
                                    size_t src_offset, size_t size, bool del, char **err)
{
    if (!gnw_fw_replace_bytes(dst, dst_offset, src->data + src_offset, size, err)) return false;
    if (del) { if (!gnw_fw_clear_range(src, src_offset, src_offset + size, err)) return false; }
    for (size_t i = 0; i < size; i++) {
        gnw_lookup_set(&d->lookup, src->flash_base + (uint32_t)src_offset + (uint32_t)i,
                                    dst->flash_base + (uint32_t)dst_offset + (uint32_t)i);
    }
    return true;
}

static uint32_t round_up_word(uint32_t v) { return ((v + 3) / 4) * 4; }
static uint32_t round_down_word(uint32_t v) { return (v / 4) * 4; }

uint32_t gnw_device_move_to_int(GnwDevice *d, uint32_t ext_or_neg1, const uint8_t *ext_bytes, size_t ext_bytes_len,
                                 size_t size, const size_t *refs, size_t n_refs, char **err)
{
    size_t free_space = gnw_device_int_free_space(d, err);
    if (err && *err) return (uint32_t)-1;
    if (free_space < size) { if (err) *err = errdup("NotEnoughSpaceError"); return (uint32_t)-1; }

    uint32_t new_loc = (uint32_t)d->int_pos;
    if (ext_bytes) {
        if (!gnw_fw_replace_bytes(&d->internal, d->int_pos, ext_bytes, ext_bytes_len, err)) return (uint32_t)-1;
    } else {
        if (!device_move_copy_cross(d, &d->internal, d->int_pos, &d->external, ext_or_neg1, size, true, err)) return (uint32_t)-1;
    }
    d->int_pos += round_up_word((uint32_t)size);
    if (refs) { if (!gnw_fw_lookup_refs(&d->internal, refs, n_refs, err)) return (uint32_t)-1; }
    return new_loc;
}

static uint32_t device_move_ext_external(GnwDevice *d, uint32_t ext, size_t size, const size_t *refs, size_t n_refs, char **err)
{
    if (!gnw_fw_move_copy(&d->external, ext, d->ext_offset, size, true, err)) return (uint32_t)-1;
    /* Python's move() also updates the lookup table via _move_copy -- gnw_fw_move_copy does that internally. */
    if (refs) { if (!gnw_fw_lookup_refs(&d->internal, refs, n_refs, err)) return (uint32_t)-1; }
    return ext + (uint32_t)d->ext_offset;
}

uint32_t gnw_device_move_ext(GnwDevice *d, uint32_t ext, size_t size, const size_t *refs, size_t n_refs, char **err)
{
    char *inner_err = NULL;
    uint32_t new_loc = gnw_device_move_to_int(d, ext, NULL, 0, size, refs, n_refs, &inner_err);
    if (!inner_err) {
        d->ext_offset -= (int32_t)round_down_word((uint32_t)size);
        return new_loc;
    }
    /* NotEnoughSpaceError -> fall back to external; any other error propagates */
    bool not_enough = strstr(inner_err, "NotEnoughSpaceError") != NULL;
    free(inner_err);
    if (!not_enough) { if (err) *err = errdup("move_ext: unexpected error"); return (uint32_t)-1; }
    return device_move_ext_external(d, ext, size, refs, n_refs, err);
}

uint32_t gnw_device_move_to_compressed_memory(GnwDevice *d, uint32_t ext, size_t size,
                                               const size_t *refs, size_t n_refs, char **err)
{
    size_t current_len = gnw_device_compressed_memory_compressed_len(d, 0, err);
    if (err && *err) return (uint32_t)-1;

    if (d->compressed_memory_pos + size > d->compressed_memory.len) {
        /* Python: except NotEnoughSpaceError: return self.move_ext(ext, size, reference)
         * -- the FULL move_ext (try move_to_int first, THEN move_ext_external),
         * not move_ext_external directly. Confirmed by reading firmware.py's
         * actual move_to_compressed_memory() source -- a real bug here diverged
         * from the (correct) move_ext_external-direct call a few lines below,
         * which really is what Python does for its own "diff > int_free_space"
         * branch. Two similar-looking fallback branches, two different Python
         * targets -- worth double-checking rather than assuming symmetry. */
        return gnw_device_move_ext(d, ext, size, refs, n_refs, err);
    }
    memcpy(d->compressed_memory.data + d->compressed_memory_pos, d->external.data + ext, size);

    size_t new_len = gnw_device_compressed_memory_compressed_len(d, size, err);
    if (err && *err) return (uint32_t)-1;
    size_t diff = new_len - current_len;
    double ratio = (double)size / (double)diff;

    size_t int_free = gnw_device_int_free_space(d, err);
    if (err && *err) return (uint32_t)-1;

    if (diff > int_free) {
        memset(d->compressed_memory.data + d->compressed_memory_pos, 0, size);
        return device_move_ext_external(d, ext, size, refs, n_refs, err);
    }
    if (ratio < d->compression_ratio) {
        memset(d->compressed_memory.data + d->compressed_memory_pos, 0, size);
        return gnw_device_move_ext(d, ext, size, refs, n_refs, err);
    }

    if (!device_move_copy_cross(d, &d->compressed_memory, d->compressed_memory_pos, &d->external, ext, size, true, err))
        return (uint32_t)-1;
    if (refs) { if (!gnw_fw_lookup_refs(&d->internal, refs, n_refs, err)) return (uint32_t)-1; }
    uint32_t new_loc = (uint32_t)d->compressed_memory_pos;
    d->compressed_memory_pos += round_up_word((uint32_t)size);
    d->ext_offset -= (int32_t)round_down_word((uint32_t)size);
    return new_loc;
}
