/*
 * See gnw_thumb_asm.h for scope/rationale.
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
#define _POSIX_C_SOURCE 200809L /* strdup, strtok_r */

#include "gnw_thumb_asm.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- small dynamic byte buffer -------------------------------------- */

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} ByteBuf;

static void bb_init(ByteBuf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static void bb_push(ByteBuf *b, uint8_t byte)
{
    if (b->len == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 16;
        b->data = realloc(b->data, b->cap);
    }
    b->data[b->len++] = byte;
}

static void bb_push16(ByteBuf *b, uint16_t hw)
{
    bb_push(b, hw & 0xFF);
    bb_push(b, (hw >> 8) & 0xFF);
}

static void bb_push32(ByteBuf *b, uint16_t hw1, uint16_t hw2)
{
    bb_push16(b, hw1);
    bb_push16(b, hw2);
}

static void bb_free(ByteBuf *b)
{
    free(b->data);
    bb_init(b);
}

/* ---- error helper ----------------------------------------------------- */

static bool fail(char **error_msg, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static bool fail(char **error_msg, const char *fmt, ...)
{
    if (error_msg) {
        va_list ap;
        va_start(ap, fmt);
        char *msg = NULL;
        int n = vsnprintf(NULL, 0, fmt, ap);
        va_end(ap);
        if (n >= 0) {
            msg = malloc((size_t)n + 1);
            if (msg) {
                va_start(ap, fmt);
                vsnprintf(msg, (size_t)n + 1, fmt, ap);
                va_end(ap);
            }
        }
        *error_msg = msg;
    }
    return false;
}

/* ---- registers/conditions --------------------------------------------- */

static const char *const COND_NAMES[] = {
    "eq", "ne", "cs", "hs", "cc", "lo", "mi", "pl",
    "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al",
};
static const int COND_VALUES[] = {
    0, 1, 2, 2, 3, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
};
#define NUM_CONDS (sizeof(COND_NAMES) / sizeof(COND_NAMES[0]))

static int cond_value(const char *name)
{
    for (size_t i = 0; i < NUM_CONDS; i++) {
        if (strcmp(COND_NAMES[i], name) == 0) {
            return COND_VALUES[i];
        }
    }
    return -1;
}

/* Strips a trailing "<cond>.w" / ".w" from `mnemonic` if what remains
 * (case-insensitively) matches `base` (e.g. "mov"), mirroring the Python
 * regex ^mov(?:eq|ne|...)?\.w$ etc. Returns true on match. */
static bool strip_cond_w(const char *mnemonic, const char *base)
{
    size_t base_len = strlen(base);
    size_t mnem_len = strlen(mnemonic);
    if (mnem_len < base_len || strncmp(mnemonic, base, base_len) != 0) {
        return false;
    }
    const char *rest = mnemonic + base_len;
    size_t rest_len = strlen(rest);
    if (rest_len >= 2 && strcmp(rest + rest_len - 2, ".w") == 0) {
        size_t cond_len = rest_len - 2;
        if (cond_len == 0) {
            return true;
        }
        char cond_buf[8];
        if (cond_len >= sizeof(cond_buf)) {
            return false;
        }
        memcpy(cond_buf, rest, cond_len);
        cond_buf[cond_len] = '\0';
        return cond_value(cond_buf) >= 0;
    }
    return false;
}

static int reg_value(const char *token)
{
    if (strcmp(token, "sp") == 0) return 13;
    if (strcmp(token, "lr") == 0) return 14;
    if (strcmp(token, "pc") == 0) return 15;
    if (token[0] == 'r' && token[1] != '\0') {
        char *end;
        long v = strtol(token + 1, &end, 10);
        if (*end == '\0' && v >= 0 && v <= 15) {
            return (int)v;
        }
    }
    return -1;
}

/* ---- modified immediate ------------------------------------------------ */

static uint32_t ror32(uint32_t value, unsigned amount)
{
    amount &= 31;
    if (amount == 0) {
        return value;
    }
    return (value >> amount) | (value << (32 - amount));
}

int gnw_thumb_encode_modified_immediate(uint32_t value)
{
    if (value <= 0xFF) {
        return (int)value;
    }

    uint32_t b0 = value & 0xFF;
    uint32_t b1 = (value >> 8) & 0xFF;
    uint32_t b2 = (value >> 16) & 0xFF;
    uint32_t b3 = (value >> 24) & 0xFF;

    if (b1 == 0 && b3 == 0 && b0 == b2) {
        return (int)((0x1u << 8) | b0);
    }
    if (b0 == 0 && b2 == 0 && b1 == b3) {
        return (int)((0x2u << 8) | b1);
    }
    if (b0 == b1 && b1 == b2 && b2 == b3) {
        return (int)((0x3u << 8) | b0);
    }

    for (unsigned rotation = 8; rotation < 32; rotation++) {
        /* Python: _ror32(value, -rotation % 32) undoes the right-rotate,
         * i.e. rotates left by `rotation` -- equivalent to a right-rotate
         * by (32 - rotation) here, done via ror32() with that amount. */
        uint32_t unrotated = ror32(value, (32 - rotation) % 32);
        if (unrotated <= 0xFF && (unrotated & 0x80)) {
            return (int)((rotation << 7) | (unrotated & 0x7F));
        }
    }

    return -1;
}

static bool split_modified_immediate(uint32_t constant, int *i, int *imm3, int *imm8,
                                      char **error_msg)
{
    int twelve = gnw_thumb_encode_modified_immediate(constant);
    if (twelve < 0) {
        return fail(error_msg, "0x%08X is not a valid Thumb modified immediate", constant);
    }
    *i = (twelve >> 11) & 1;
    *imm3 = (twelve >> 8) & 0x7;
    *imm8 = twelve & 0xFF;
    return true;
}

/* ---- tokenizing helpers ------------------------------------------------ */

#define MAX_OPS 4
#define MAX_TOKEN 64

typedef struct {
    char text[MAX_TOKEN];
} Token;

/* Splits on ',' and whitespace, like Python's text.replace(",", " ").split(). */
static int tokenize(const char *text, Token *tokens, int max_tokens)
{
    int n = 0;
    const char *p = text;
    while (*p && n < max_tokens) {
        while (*p == ',' || isspace((unsigned char)*p)) {
            p++;
        }
        if (!*p) {
            break;
        }
        size_t len = 0;
        while (*p && *p != ',' && !isspace((unsigned char)*p) && len < MAX_TOKEN - 1) {
            tokens[n].text[len++] = *p++;
        }
        while (*p && *p != ',' && !isspace((unsigned char)*p)) {
            p++; /* overflow: consume without storing (shouldn't happen for real input) */
        }
        tokens[n].text[len] = '\0';
        n++;
    }
    return n;
}

static bool parse_imm(const char *token, long *out)
{
    if (token[0] != '#') {
        return false;
    }
    char *end;
    long v = strtol(token + 1, &end, 0);
    if (*end != '\0') {
        return false;
    }
    *out = v;
    return true;
}

/* ---- per-instruction encoders ------------------------------------------ */

static bool enc_movw(Token *ops, int nops, ByteBuf *out, char **error_msg)
{
    if (nops != 2) {
        return fail(error_msg, "movw expects 'Rd, #imm16'");
    }
    int rd = reg_value(ops[0].text);
    if (rd < 0) {
        return fail(error_msg, "Invalid register '%s'", ops[0].text);
    }
    if (rd == 13 || rd == 15) {
        return fail(error_msg, "movw cannot target %s", ops[0].text);
    }
    long imm;
    if (!parse_imm(ops[1].text, &imm)) {
        return fail(error_msg, "Invalid immediate '%s'", ops[1].text);
    }
    if (imm < 0 || imm > 0xFFFF) {
        return fail(error_msg, "movw immediate 0x%lX out of 16-bit range", imm);
    }
    uint32_t imm4 = (imm >> 12) & 0xF;
    uint32_t i = (imm >> 11) & 1;
    uint32_t imm3 = (imm >> 8) & 0x7;
    uint32_t imm8 = imm & 0xFF;
    uint16_t hw1 = 0xF240 | (i << 10) | imm4;
    uint16_t hw2 = (imm3 << 12) | (rd << 8) | imm8;
    bb_push32(out, hw1, hw2);
    return true;
}

static bool enc_mov_w(Token *ops, int nops, ByteBuf *out, char **error_msg)
{
    if (nops != 2) {
        return fail(error_msg, "mov.w expects 'Rd, #const'");
    }
    int rd = reg_value(ops[0].text);
    if (rd < 0) {
        return fail(error_msg, "Invalid register '%s'", ops[0].text);
    }
    if (rd == 13 || rd == 15) {
        return fail(error_msg, "mov.w cannot target %s", ops[0].text);
    }
    long imm;
    if (!parse_imm(ops[1].text, &imm)) {
        return fail(error_msg, "Invalid immediate '%s'", ops[1].text);
    }
    int i = 0, imm3 = 0, imm8 = 0;
    if (!split_modified_immediate((uint32_t)imm, &i, &imm3, &imm8, error_msg)) {
        return false;
    }
    uint16_t hw1 = 0xF04F | (i << 10);
    uint16_t hw2 = (imm3 << 12) | (rd << 8) | imm8;
    bb_push32(out, hw1, hw2);
    return true;
}

static bool enc_addsub_w(Token *ops, int nops, uint16_t base_hw1, const char *name,
                          ByteBuf *out, char **error_msg)
{
    if (nops != 3) {
        return fail(error_msg, "%s expects 'Rd, Rn, #const'", name);
    }
    int rd = reg_value(ops[0].text);
    int rn = reg_value(ops[1].text);
    if (rd < 0 || rn < 0) {
        return fail(error_msg, "Invalid register in '%s'", name);
    }
    if (rd == 15) {
        return fail(error_msg, "%s cannot target pc", name);
    }
    if (rn == 15) {
        return fail(error_msg, "%s cannot use pc as the source register", name);
    }
    long imm;
    if (!parse_imm(ops[2].text, &imm)) {
        return fail(error_msg, "Invalid immediate '%s'", ops[2].text);
    }
    int i = 0, imm3 = 0, imm8 = 0;
    if (!split_modified_immediate((uint32_t)imm, &i, &imm3, &imm8, error_msg)) {
        return false;
    }
    uint16_t hw1 = base_hw1 | (i << 10) | rn;
    uint16_t hw2 = (imm3 << 12) | (rd << 8) | imm8;
    bb_push32(out, hw1, hw2);
    return true;
}

static bool enc_mov(Token *ops, int nops, ByteBuf *out, char **error_msg)
{
    if (nops != 2) {
        return fail(error_msg, "mov expects 'Rd, Rm'");
    }
    int rd = reg_value(ops[0].text);
    int rm = reg_value(ops[1].text);
    if (rd < 0 || rm < 0) {
        return fail(error_msg, "Invalid register in 'mov'");
    }
    uint32_t d = (rd >> 3) & 1;
    uint16_t hw = 0x4600 | (d << 7) | (rm << 3) | (rd & 0x7);
    bb_push16(out, hw);
    return true;
}

static bool enc_sub_sp(Token *ops, int nops, ByteBuf *out, char **error_msg)
{
    if (nops != 2 || reg_value(ops[0].text) != 13) {
        return fail(error_msg, "sub expects 'sp, #imm'");
    }
    long imm;
    if (!parse_imm(ops[1].text, &imm)) {
        return fail(error_msg, "Invalid immediate '%s'", ops[1].text);
    }
    if (imm % 4 != 0 || imm < 0 || imm > 0x1FC) {
        return fail(error_msg, "sub sp immediate 0x%lX must be a multiple of 4 in [0, 0x1FC]", imm);
    }
    bb_push16(out, 0xB080 | (imm >> 2));
    return true;
}

static bool enc_ldr_w_literal(const char *rt_token, const char *imm_token,
                               ByteBuf *out, char **error_msg)
{
    int rt = reg_value(rt_token);
    if (rt < 0) {
        return fail(error_msg, "Invalid register '%s'", rt_token);
    }
    char *end;
    long imm = strtol(imm_token, &end, 0);
    if (*end != '\0') {
        return fail(error_msg, "Invalid literal offset '%s'", imm_token);
    }
    if (imm < -0xFFF || imm > 0xFFF) {
        return fail(error_msg, "ldr.w literal offset %ld out of +/-4095 range", imm);
    }
    uint32_t u = imm >= 0 ? 1 : 0;
    uint16_t hw1 = 0xF85F | (u << 7);
    uint16_t hw2 = (rt << 12) | (labs(imm) & 0xFFF);
    bb_push32(out, hw1, hw2);
    return true;
}

static bool parse_branch_target(const char *token, long *out)
{
    if (token[0] == '#') {
        token++;
    }
    char *end;
    long v = strtol(token, &end, 0);
    if (*end != '\0') {
        return false;
    }
    *out = v;
    return true;
}

static bool enc_b(Token *ops, int nops, uint32_t addr, ByteBuf *out, char **error_msg)
{
    if (nops != 1) {
        return fail(error_msg, "b expects a single target");
    }
    long target;
    if (!parse_branch_target(ops[0].text, &target)) {
        return fail(error_msg, "Invalid branch target '%s'", ops[0].text);
    }
    long offset = target - (long)(addr + 4);
    if (offset & 1) {
        return fail(error_msg, "branch target must be halfword-aligned");
    }
    long imm = offset >> 1;
    if (imm < -0x400 || imm > 0x3FF) {
        return fail(error_msg, "b target out of range for the narrow encoding (offset %ld)", offset);
    }
    bb_push16(out, 0xE000 | (imm & 0x7FF));
    return true;
}

static bool enc_b_w(Token *ops, int nops, uint32_t addr, ByteBuf *out, char **error_msg)
{
    if (nops != 1) {
        return fail(error_msg, "b.w expects a single target");
    }
    long target;
    if (!parse_branch_target(ops[0].text, &target)) {
        return fail(error_msg, "Invalid branch target '%s'", ops[0].text);
    }
    long offset = target - (long)(addr + 4);
    if (offset & 1) {
        return fail(error_msg, "branch target must be halfword-aligned");
    }
    if (offset < -(1L << 24) || offset >= (1L << 24)) {
        return fail(error_msg, "b.w target out of +/-16MB range (offset %ld)", offset);
    }
    uint32_t s = (offset >> 24) & 1;
    uint32_t i1 = (offset >> 23) & 1;
    uint32_t i2 = (offset >> 22) & 1;
    uint32_t imm10 = (offset >> 12) & 0x3FF;
    uint32_t imm11 = (offset >> 1) & 0x7FF;
    uint32_t j1 = (~(i1 ^ s)) & 1;
    uint32_t j2 = (~(i2 ^ s)) & 1;
    uint16_t hw1 = 0xF000 | (s << 10) | imm10;
    uint16_t hw2 = 0x9000 | (j1 << 13) | (j2 << 11) | imm11;
    bb_push32(out, hw1, hw2);
    return true;
}

static bool enc_it(const char *mnemonic, Token *ops, int nops, ByteBuf *out, char **error_msg)
{
    if (nops != 1) {
        return fail(error_msg, "IT expects a single condition");
    }
    const char *pattern = mnemonic + 2; /* skip "it" */
    size_t plen = strlen(pattern);
    if (plen > 3) {
        return fail(error_msg, "Unsupported IT form '%s'", mnemonic);
    }
    for (size_t k = 0; k < plen; k++) {
        if (pattern[k] != 't' && pattern[k] != 'e') {
            return fail(error_msg, "Unsupported IT form '%s'", mnemonic);
        }
    }
    int cond = cond_value(ops[0].text);
    if (cond < 0) {
        return fail(error_msg, "Invalid condition '%s'", ops[0].text);
    }
    int fc0 = cond & 1;
    int n = 1 + (int)plen;
    uint32_t mask = 1u << (4 - n);
    for (size_t idx = 0; idx < plen; idx++) {
        int val = (pattern[idx] == 't') ? fc0 : (fc0 ^ 1);
        mask |= (uint32_t)val << (3 - idx);
    }
    bb_push16(out, 0xBF00 | (cond << 4) | mask);
    return true;
}

/* ---- top level ---------------------------------------------------------- */

static void to_lower_inplace(char *s)
{
    for (; *s; s++) {
        *s = (char)tolower((unsigned char)*s);
    }
}

static bool match_ldr_w_pc(const char *text, char *rt_out, size_t rt_out_size,
                            char *imm_out, size_t imm_out_size)
{
    /* ^ldr\.w\s+(\w+)\s*,\s*\[\s*pc\s*,\s*#\s*(-?(?:0x)?[0-9a-f]+)\s*\]$ */
    const char *p = text;
    static const char prefix[] = "ldr.w";
    size_t plen = sizeof(prefix) - 1;
    if (strncmp(p, prefix, plen) != 0) {
        return false;
    }
    p += plen;
    if (!isspace((unsigned char)*p)) {
        return false;
    }
    while (isspace((unsigned char)*p)) {
        p++;
    }
    size_t rt_len = 0;
    while (isalnum((unsigned char)*p) || *p == '_') {
        if (rt_len < rt_out_size - 1) {
            rt_out[rt_len++] = *p;
        }
        p++;
    }
    rt_out[rt_len] = '\0';
    if (rt_len == 0) {
        return false;
    }
    while (isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != ',') {
        return false;
    }
    p++;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != '[') {
        return false;
    }
    p++;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    if (strncmp(p, "pc", 2) != 0) {
        return false;
    }
    p += 2;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != ',') {
        return false;
    }
    p++;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != '#') {
        return false;
    }
    p++;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    size_t imm_len = 0;
    const char *imm_start = p;
    if (*p == '-') {
        p++;
    }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        while (isxdigit((unsigned char)*p)) {
            p++;
        }
    } else {
        while (isdigit((unsigned char)*p)) {
            p++;
        }
    }
    imm_len = (size_t)(p - imm_start);
    if (imm_len == 0 || imm_len >= imm_out_size) {
        return false;
    }
    memcpy(imm_out, imm_start, imm_len);
    imm_out[imm_len] = '\0';
    while (isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != ']') {
        return false;
    }
    p++;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    return *p == '\0';
}

static bool assemble_one(const char *text, uint32_t addr, ByteBuf *out, char **error_msg)
{
    char rt_buf[MAX_TOKEN], imm_buf[MAX_TOKEN];
    if (match_ldr_w_pc(text, rt_buf, sizeof(rt_buf), imm_buf, sizeof(imm_buf))) {
        return enc_ldr_w_literal(rt_buf, imm_buf, out, error_msg);
    }

    Token tokens[1 + MAX_OPS];
    int ntok = tokenize(text, tokens, 1 + MAX_OPS);
    if (ntok == 0) {
        return true; /* empty piece -- no bytes, matches Python's `if not tokens: return []` */
    }
    const char *mnemonic = tokens[0].text;
    Token *ops = &tokens[1];
    int nops = ntok - 1;

    if (strip_cond_w(mnemonic, "mov")) {
        return enc_mov_w(ops, nops, out, error_msg);
    }
    if (strcmp(mnemonic, "movw") == 0) {
        return enc_movw(ops, nops, out, error_msg);
    }
    if (strcmp(mnemonic, "mov") == 0) {
        return enc_mov(ops, nops, out, error_msg);
    }
    if (strip_cond_w(mnemonic, "add")) {
        return enc_addsub_w(ops, nops, 0xF100, "add.w", out, error_msg);
    }
    if (strip_cond_w(mnemonic, "sub")) {
        return enc_addsub_w(ops, nops, 0xF1A0, "sub.w", out, error_msg);
    }
    if (strcmp(mnemonic, "sub") == 0) {
        return enc_sub_sp(ops, nops, out, error_msg);
    }
    if (strcmp(mnemonic, "b.w") == 0) {
        return enc_b_w(ops, nops, addr, out, error_msg);
    }
    if (strcmp(mnemonic, "b") == 0) {
        return enc_b(ops, nops, addr, out, error_msg);
    }
    if (strncmp(mnemonic, "it", 2) == 0) {
        return enc_it(mnemonic, ops, nops, out, error_msg);
    }

    return fail(error_msg, "Unsupported instruction: '%s'", text);
}

bool gnw_thumb_assemble(const char *code, uint32_t addr,
                         uint8_t **out, size_t *out_len, char **error_msg)
{
    if (error_msg) {
        *error_msg = NULL;
    }

    char *lower = strdup(code);
    if (!lower) {
        return fail(error_msg, "out of memory");
    }
    to_lower_inplace(lower);

    ByteBuf result;
    bb_init(&result);

    bool ok = true;
    char *saveptr = NULL;
    char *piece = strtok_r(lower, ";", &saveptr);
    /* strtok_r skips leading empty fields; the Python version's split(";")
     * instead yields them and then `if not piece: continue`s -- equivalent
     * net effect since empty pieces emit no bytes either way. But strtok_r
     * also drops a genuinely-empty *trailing* piece (e.g. "foo;"), which
     * Python's split likewise turns into an empty (skipped) piece -- so
     * behavior matches for all inputs this patcher actually uses. */
    while (piece != NULL) {
        /* strip whitespace, matching Python's piece.strip() */
        while (isspace((unsigned char)*piece)) {
            piece++;
        }
        char *end = piece + strlen(piece);
        while (end > piece && isspace((unsigned char)*(end - 1))) {
            *(--end) = '\0';
        }
        if (*piece != '\0') {
            if (!assemble_one(piece, addr + (uint32_t)result.len, &result, error_msg)) {
                ok = false;
                break;
            }
        }
        piece = strtok_r(NULL, ";", &saveptr);
    }

    free(lower);

    if (!ok) {
        bb_free(&result);
        return false;
    }

    *out = result.data;
    *out_len = result.len;
    return true;
}
