/*
 * Top-level driver for the C port of scripts/make_cfw_images.py -- loads
 * stock firmware dumps + gnwmanager's pre-built novel-code patch binary,
 * runs the per-game patch() sequence, writes bank1-patched.bin /
 * extflash-patched.bin. Structural port of gnwmanager's
 * cli/gnw_patch/_patch.py::_common_prepare() + mario()/zelda() CLI
 * command bodies (bootloader=True always, matching this project's
 * documented default -- see scripts/make_cfw_images.py's own docstring).
 *
 * Deliberately skips the STOCK_ROM_SHA1 verification firmware.py's
 * IntFirmware/ExtFirmware constructors normally do -- a safety check for
 * catching a wrong input file, not something that affects the patch
 * transform's own correctness, and out of scope for this pass (no SHA1
 * implementation has been ported/vendored yet).
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
#include "gnw_mario_patch.h"
#include "gnw_zelda_patch.h"
#include "gnw_symbols_mario.h"
#include "gnw_symbols_zelda.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLASH_BANK_SIZE 0x40000u
#define EXTFLASH_SIZE   0x4000000u
#define BOOTLOADER_RESERVE 0x32000u /* 200KiB, matches gnwmanager's bootloader=True internal length */

struct game_config {
    const char *name;
    uint32_t stock_rom_end;
    uint32_t key_offset, nonce_offset;
    uint32_t rwdata_offset, rwdata_len;
    uint32_t enc_start, enc_end;
    uint32_t cmem_base, cmem_len;
    bool is_mario;
    const GnwSymbolEntry *symbols;
    size_t symbols_count;
};

static const struct game_config MARIO_CFG = {
    "mario", 0x18100, 0x106f4, 0x106e4, 0x180a4, 36, 0, 0xfe000,
    0x240f2124, 0xdedc, true, gnw_symbols_mario, /* count filled below */ 0
};
static const struct game_config ZELDA_CFG = {
    "zelda", 0x1b3e0, 0x165a4, 0x16590, 0x1b390, 20, 0x20000, 0x3254a0,
    0x240f2124, 1 /* unused, freeMemory.FLASH_LEN==0 for zelda */, false, gnw_symbols_zelda, 0
};

static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)len);
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(buf); return NULL; }
    fclose(f);
    *out_len = (size_t)len;
    return buf;
}

static bool write_file_padded(const char *path, const uint8_t *data, size_t len, size_t pad_to)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return false; }
    fwrite(data, 1, len, f);
    if (pad_to > len) {
        uint8_t *pad = malloc(pad_to - len);
        memset(pad, 0xFF, pad_to - len);
        fwrite(pad, 1, pad_to - len, f);
        free(pad);
    }
    fclose(f);
    return true;
}

int main(int argc, char **argv)
{
    if (argc < 2 || (strcmp(argv[1], "mario") != 0 && strcmp(argv[1], "zelda") != 0)) {
        fprintf(stderr, "usage: %s <mario|zelda> [repo_root]\n", argv[0]);
        return 1;
    }
    bool is_mario = strcmp(argv[1], "mario") == 0;
    struct game_config cfg = is_mario ? MARIO_CFG : ZELDA_CFG;
    cfg.symbols_count = is_mario ? gnw_symbols_mario_count : gnw_symbols_zelda_count;

    const char *repo_root = argc >= 3 ? argv[2] : ".";
    char path[1024];

    snprintf(path, sizeof(path), "%s/backup/internal_flash_backup_%s.bin", repo_root, cfg.name);
    size_t internal_raw_len;
    uint8_t *internal_raw = read_file(path, &internal_raw_len);
    if (!internal_raw) return 1;

    snprintf(path, sizeof(path), "%s/backup/flash_backup_%s.bin", repo_root, cfg.name);
    size_t external_len;
    uint8_t *external_data = read_file(path, &external_len);
    if (!external_data) return 1;

    /* gnwmanager's own pre-built novel-code binary for this game/variant */
    snprintf(path, sizeof(path), "%s/../gnwmanager/gnwmanager/cli/gnw_patch/binaries/%s/0x08032000.bin", repo_root, cfg.name);
    size_t patch_data_len;
    uint8_t *patch_data = read_file(path, &patch_data_len);
    if (!patch_data) {
        fprintf(stderr, "note: expects a gnwmanager checkout at ../gnwmanager relative to repo_root\n");
        return 1;
    }

    /* Ordering matches _common_prepare()/IntFirmware.__init__ exactly: RWData
     * is parsed from the RAW stock dump (device = cls(internal, elf, external))
     * BEFORE the STOCK_ROM_END splice (device.internal[novel_code_start:] =
     * patch_data[novel_code_start:]) overwrites that region -- confirmed the
     * hard way: doing the splice first corrupts a rwdata entry whose data_addr
     * falls past STOCK_ROM_END and lz77-decodes garbage. */
    size_t internal_final_len = BOOTLOADER_RESERVE; /* 0x32000, since patch_data_len == 0x20000 == FLASH_LEN for both games */
    uint8_t *internal_data = calloc(1, internal_final_len);
    memcpy(internal_data, internal_raw, internal_raw_len); /* raw stock dump, untouched */
    free(internal_raw);

    GnwDevice d;
    memset(&d, 0, sizeof(d));
    gnw_lookup_init(&d.lookup);
    gnw_fw_init(&d.internal, internal_data, internal_final_len, internal_final_len, 0x08000000,
                &d.lookup, cfg.symbols, cfg.symbols_count);
    gnw_fw_init(&d.external, external_data, external_len, external_len, 0x90000000,
                &d.lookup, NULL, 0);
    size_t cmem_len = is_mario ? cfg.cmem_len : 1; /* zelda's freeMemory.FLASH_LEN is 0; alloc 1 byte, never used */
    uint8_t *cmem_data = calloc(1, cmem_len);
    gnw_fw_init(&d.compressed_memory, cmem_data, is_mario ? cfg.cmem_len : 0, is_mario ? cfg.cmem_len : 0,
                cfg.cmem_base, &d.lookup, NULL, 0);

    d.compression_ratio = 1.4;
    /* all other args default false/0, matching make_cfw_images.py's documented defaults */

    char *err = NULL;

    /* crypt(): decrypt external using internal's embedded key/nonce */
    uint8_t key[16], nonce[8];
    memcpy(key, internal_data + cfg.key_offset, 16);
    memcpy(nonce, internal_data + cfg.nonce_offset, 8);
    if (!gnw_device_crypt(&d, key, nonce, cfg.enc_start, cfg.enc_end, &err)) {
        fprintf(stderr, "crypt failed: %s\n", err); return 1;
    }

    /* rwdata table load from the still-untouched stock bytes -- see ordering note above */
    if (!gnw_rwdata_load(&d.rwdata, &d.internal, cfg.rwdata_offset, cfg.rwdata_len, &err)) {
        fprintf(stderr, "rwdata load failed: %s\n", err); return 1;
    }
    d.has_rwdata = true;

    /* NOW splice in the novel-code patch past STOCK_ROM_END */
    memcpy(internal_data + cfg.stock_rom_end, patch_data + cfg.stock_rom_end, patch_data_len - cfg.stock_rom_end);
    free(patch_data);

    /* empty_offset: search from rwdata.table_end for a 256-byte all-zero run,
     * bounded by the class's FLASH_LEN constant (0x20000) -- NOT the
     * possibly-extended buffer length -- matching IntFirmware.empty_offset
     * exactly (`for addr in range(search_start, self.FLASH_LEN, 0x10)`). */
    size_t search_start = gnw_rwdata_table_end(&d.rwdata);
    size_t int_pos = (size_t)-1;
    for (size_t addr = search_start; addr + 256 <= 0x20000u; addr += 0x10) {
        bool all_zero = true;
        for (size_t i = 0; i < 256; i++) { if (internal_data[addr + i] != 0) { all_zero = false; break; } }
        if (all_zero) { int_pos = addr; break; }
    }
    if (int_pos == (size_t)-1) { fprintf(stderr, "empty_offset: not found\n"); return 1; }
    d.int_pos = int_pos;

    bool patch_ok = is_mario ? gnw_mario_patch(&d, &err) : gnw_zelda_patch(&d, &err);
    if (!patch_ok) {
        fprintf(stderr, "patch failed: %s\n", err);
        return 1;
    }

    /* HeaderMetaData pack at 0x1B8: blocks4k(round_up_page(ext_len)>>12) | flags(magic=4, is_mario/is_zelda bit) */
    uint32_t blocks4k = (((uint32_t)d.external.len + 4095) / 4096) & 0xFFFFFFu;
    uint32_t flags = 0x4 | (is_mario ? (1u << 4) : (1u << 5)); /* METADATA_MAGIC=4 */
    uint32_t metadata = (blocks4k) | (flags << 24);
    if (!gnw_fw_replace_int(&d.internal, 0x1B8, metadata, 4, &err)) {
        fprintf(stderr, "metadata write failed: %s\n", err); return 1;
    }

    snprintf(path, sizeof(path), "%s/backup/qemu-images/%s-bank1-patched.bin", repo_root, cfg.name);
    if (!write_file_padded(path, d.internal.data, d.internal.len, FLASH_BANK_SIZE)) return 1;
    snprintf(path, sizeof(path), "%s/backup/qemu-images/%s-extflash-patched.bin", repo_root, cfg.name);
    if (!write_file_padded(path, d.external.data, d.external.len, EXTFLASH_SIZE)) return 1;

    printf("wrote %s-bank1-patched.bin (%zu bytes used) and %s-extflash-patched.bin (%zu bytes used)\n",
           cfg.name, d.internal.len, cfg.name, d.external.len);
    return 0;
}
