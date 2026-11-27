/*
 * ffpdf — read and fill PDF form fields.
 * Copyright (C) 2026 Page Brooks
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy
 * of the License at <http://www.apache.org/licenses/LICENSE-2.0>. Distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND; see the
 * License for the specific language governing permissions and limitations.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pdf_lex.h"

#include <string.h>
#include <ctype.h>
#include <stdlib.h>

// Skip a literal string starting at '(' (handling escapes and balanced parens);
// return a pointer just past the closing ')'.
static const char *skip_literal_string(const char *p) {
    int depth = 1; p++;
    while (*p && depth > 0) {
        if (*p == '\\' && p[1]) { p += 2; continue; }
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        p++;
    }
    return p;
}

const char *match_dict_end(const char *start) {
    const char *p = start + 2;
    int depth = 1;
    while (*p) {
        if (*p == '(') { p = skip_literal_string(p); continue; }
        if (p[0] == '<' && p[1] == '<') { depth++; p += 2; continue; }
        if (p[0] == '>' && p[1] == '>') { depth--; p += 2; if (depth == 0) return p; continue; }
        if (*p == '<') {                      // hex string
            p++;
            while (*p && *p != '>') p++;
            if (*p == '>') p++;
            continue;
        }
        p++;
    }
    return NULL;
}

// As skip_literal_string, but bounded by `end` instead of a NUL: a raw 0x00
// byte inside the string is an ordinary byte (ISO 32000-1 7.3.4.2).
static const char *skip_literal_n(const char *p, const char *end) {
    int depth = 1; p++;
    while (p < end && depth > 0) {
        if (*p == '\\') { p += (p + 1 < end) ? 2 : 1; continue; }
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        p++;
    }
    return p;
}

const char *match_dict_end_n(const char *start, const char *end) {
    const char *p = start + 2;
    int depth = 1;
    while (p < end) {
        if (*p == '(') { p = skip_literal_n(p, end); continue; }
        if (p + 1 < end && p[0] == '<' && p[1] == '<') { depth++; p += 2; continue; }
        if (p + 1 < end && p[0] == '>' && p[1] == '>') {
            depth--; p += 2;
            if (depth == 0) return p;
            continue;
        }
        if (*p == '<') {                      // hex string
            p++;
            while (p < end && *p != '>') p++;
            if (p < end) p++;
            continue;
        }
        p++;
    }
    return NULL;
}

// Find `key` (e.g. "/T") as a top-level dictionary key in `dict`, returning a
// pointer to its leading '/', or NULL. Skips over string/array/nested-dict
// contents so the same byte sequence inside a value (e.g. "/V" inside a /TU
// string) is not mistaken for a key. `dict` may or may not include the outer
// "<<"/">>"; keys are matched at the outermost dictionary level either way.
static const char *find_key_pos(const char *dict, const char *key) {
    size_t klen = strlen(key);
    int top = (dict[0] == '<' && dict[1] == '<') ? 1 : 0;
    int depth = 0;
    const char *p = dict;
    while (*p) {
        if (*p == '(') { p = skip_literal_string(p); continue; }
        if (p[0] == '<' && p[1] == '<') { depth++; p += 2; continue; }
        if (p[0] == '>' && p[1] == '>') { depth--; p += 2; continue; }
        if (*p == '<') { p++; while (*p && *p != '>') p++; if (*p == '>') p++; continue; }  // hex
        if (*p == '[') { depth++; p++; continue; }
        if (*p == ']') { depth--; p++; continue; }
        if (*p == '/' && depth == top && strncmp(p, key, klen) == 0) {
            char nc = p[klen];
            if (!(isalnum((unsigned char)nc) || nc == '_' || nc == '.' || nc == '-'))
                return p;
        }
        p++;
    }
    return NULL;
}

const char *find_key(const char *dict, const char *key) {
    const char *kp = find_key_pos(dict, key);
    if (!kp) return NULL;
    const char *v = kp + strlen(key);
    while (*v == ' ' || *v == '\n' || *v == '\r' || *v == '\t' || *v == '\f') v++;
    return v;
}

const char *skip_value(const char *v) {
    if (*v == '(') {                       // literal string
        int depth = 1; v++;
        while (*v && depth > 0) {
            if (*v == '\\' && v[1]) { v += 2; continue; }
            if (*v == '(') depth++;
            else if (*v == ')') depth--;
            v++;
        }
        return v;
    }
    if (v[0] == '<' && v[1] == '<') {      // dictionary
        int depth = 1; v += 2;
        while (*v && depth > 0) {
            if (v[0] == '<' && v[1] == '<') { depth++; v += 2; }
            else if (v[0] == '>' && v[1] == '>') { depth--; v += 2; }
            else v++;
        }
        return v;
    }
    if (*v == '<') {                       // hex string
        v++;
        while (*v && *v != '>') v++;
        if (*v == '>') v++;
        return v;
    }
    if (*v == '[') {                       // array
        int depth = 1; v++;
        while (*v && depth > 0) {
            if (*v == '[') depth++;
            else if (*v == ']') depth--;
            v++;
        }
        return v;
    }
    if (*v == '/') {                       // name
        v++;
        while (*v && !isspace((unsigned char)*v) && !strchr("()<>[]{}/%", *v)) v++;
        return v;
    }
    // number/boolean/keyword token, possibly an indirect ref "N G R"
    const char *tok_end = v;
    while (*tok_end && !isspace((unsigned char)*tok_end) && !strchr("()<>[]{}/%", *tok_end)) tok_end++;
    const char *r = tok_end;
    while (*r == ' ' || *r == '\t' || *r == '\n' || *r == '\r') r++;
    if (isdigit((unsigned char)*r)) {      // maybe "N G R"
        while (isdigit((unsigned char)*r)) r++;
        while (*r == ' ' || *r == '\t') r++;
        if (*r == 'R') return r + 1;
    }
    return tok_end;
}

void remove_entry(char *dict, const char *key) {
    size_t klen = strlen(key);
    char *p;
    while ((p = (char *)find_key_pos(dict, key)) != NULL) {
        const char *v = p + klen;
        while (*v == ' ' || *v == '\n' || *v == '\r' || *v == '\t' || *v == '\f') v++;
        const char *vend = skip_value(v);
        memmove(p, vend, strlen(vend) + 1);   // includes NUL
    }
}

int extract_dict_inner(const char *obj_dict, char *out, size_t out_size) {
    const char *open = strstr(obj_dict, "<<");
    if (!open) return -1;
    const char *end = match_dict_end(open);   // string-aware; just past ">>"
    if (!end) return -1;                       // unbalanced => truncated dictionary
    const char *inner = open + 2;
    size_t len = (size_t)((end - 2) - inner);
    if (len >= out_size) return -1;
    memcpy(out, inner, len);
    out[len] = '\0';
    return 0;
}

const char *dict_find(const char *dict, const char *key) {
    const char *p = dict ? strstr(dict, key) : NULL;
    if (!p) return NULL;
    p += strlen(key);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

int dict_int(const char *dict, const char *key, int dflt) {
    const char *p = dict_find(dict, key);
    if (!p || !isdigit((unsigned char)*p)) return dflt;
    return atoi(p);
}

const char *read_literal(const char *p, char *out, size_t out_size) {
    p++;   // skip '('
    int depth = 1;
    size_t k = 0;
    while (*p && depth > 0) {
        if (*p == '\\' && p[1]) { p++; if (k < out_size - 1) out[k++] = *p; p++; continue; }
        if (*p == '(') depth++;
        else if (*p == ')') { depth--; if (depth == 0) { p++; break; } }
        if (k < out_size - 1) out[k++] = *p;
        p++;
    }
    out[k] = '\0';
    return p;
}

const char *read_literal_alloc(const char *p, char **out) {
    p++;   // skip '('
    int depth = 1;
    size_t k = 0, cap = 64;
    char *buf = malloc(cap);
    if (!buf) { *out = NULL; return p; }
    while (*p && depth > 0) {
        char c;
        if (*p == '\\' && p[1]) { p++; c = *p++; }
        else if (*p == '(') { depth++; c = *p++; }
        else if (*p == ')') { depth--; if (depth == 0) { p++; break; } c = *p++; }
        else c = *p++;
        if (k + 1 >= cap) { cap *= 2; char *g = realloc(buf, cap); if (!g) { free(buf); *out = NULL; return p; } buf = g; }
        buf[k++] = c;
    }
    buf[k] = '\0';
    *out = buf;
    return p;
}

// Decoder behind pdf_literal_bytes. The string ends at `end`, or at a NUL when
// `end` is NULL.
static const char *literal_bytes_core(const char *p, const char *end,
                                      unsigned char *out, size_t cap, size_t *len) {
    size_t n = 0;
    int depth = 1;
#define PUT(b) do { if (n < cap) out[n++] = (unsigned char)(b); } while (0)
#define MORE() (end ? p < end : *p != '\0')
    p++;   // skip '('
    while (MORE()) {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            if (!MORE()) break;
            c = (unsigned char)*p++;
            switch (c) {
                case 'n': PUT('\n'); break;
                case 'r': PUT('\r'); break;
                case 't': PUT('\t'); break;
                case 'b': PUT('\b'); break;
                case 'f': PUT('\f'); break;
                case '\r': if (MORE() && *p == '\n') p++; break;   // line continuation: no byte
                case '\n': break;
                default:
                    if (c >= '0' && c <= '7') {           // \d, \dd or \ddd octal
                        int v = c - '0';
                        for (int i = 0; i < 2 && MORE() && *p >= '0' && *p <= '7'; i++)
                            v = v * 8 + (*p++ - '0');
                        PUT(v & 0xff);                    // high-order overflow ignored
                    } else {
                        PUT(c);                           // \( \) \\ and unknown escapes
                    }
            }
            continue;
        }
        if (c == '(') depth++;
        else if (c == ')' && --depth == 0) break;
        else if (c == '\r') { if (MORE() && *p == '\n') p++; c = '\n'; }   // bare EOL reads as LF
        PUT(c);
    }
#undef MORE
#undef PUT
    *len = n;
    return p;
}

const char *pdf_literal_bytes(const char *p, unsigned char *out, size_t cap, size_t *len) {
    return literal_bytes_core(p, NULL, out, cap, len);
}

char *pdf_hexify_literals(const char *s, size_t n) {
    // A literal of r bytes (parentheses included) decodes to at most r - 1
    // bytes, i.e. at most 2r hex digits with its <>; every other byte is copied
    // one for one. So 2n + 1 always suffices.
    static const char HEX[] = "0123456789ABCDEF";
    const char *p = s, *end = s + n;
    char *out = malloc(2 * n + 1);
    if (!out) return NULL;
    size_t k = 0;
    while (p < end) {
        if (*p == '(') {
            const char *q = skip_literal_n(p, end);
            size_t raw = (size_t)(q - p), bl = 0;
            unsigned char *b = malloc(raw ? raw : 1);
            if (!b) { free(out); return NULL; }
            literal_bytes_core(p, q, b, raw, &bl);
            out[k++] = '<';
            for (size_t i = 0; i < bl; i++) { out[k++] = HEX[b[i] >> 4]; out[k++] = HEX[b[i] & 15]; }
            out[k++] = '>';
            free(b);
            p = q;
            continue;
        }
        if (p + 1 < end && p[0] == '<' && p[1] == '<') { out[k++] = '<'; out[k++] = '<'; p += 2; continue; }
        if (*p == '<') {                              // hex string: copy through its '>'
            do { out[k++] = *p ? *p : ' '; } while (++p < end && p[-1] != '>');
            continue;
        }
        out[k++] = *p ? *p : ' ';                     // NUL is PDF white-space
        p++;
    }
    out[k] = '\0';
    return out;
}

/* ==========================================================================
 * PDF text strings -> UTF-8.
 * ========================================================================== */

unsigned utf8_next(const unsigned char **s) {
    const unsigned char *p = *s;
    unsigned c = p[0], cp;
    int n;                                        // continuation bytes expected
    if (c < 0x80)                { *s = p + 1; return c; }
    else if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
    else                         { *s = p + 1; return c; }   // stray continuation
    for (int i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *s = p + 1; return c; }  // truncated sequence
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *s = p + n + 1;
    return cp;
}

// PDFDocEncoding (ISO 32000-1 Annex D) agrees with Latin-1 except for these
// bytes: 0x18..0x1F, and 0x7F..0xA0 (offset 8 onward), plus 0xAD. 0 marks a
// byte the encoding leaves undefined.
static const unsigned short PDFDOC_18[8] = {
    0x02D8, 0x02C7, 0x02C6, 0x02D9, 0x02DD, 0x02DB, 0x02DA, 0x02DC,
};
static const unsigned short PDFDOC_7F[34] = {
    0,                                                        /* 0x7F */
    0x2022, 0x2020, 0x2021, 0x2026, 0x2014, 0x2013, 0x0192, 0x2044,  /* 0x80 */
    0x2039, 0x203A, 0x2212, 0x2030, 0x201E, 0x201C, 0x201D, 0x2018,  /* 0x88 */
    0x2019, 0x201A, 0x2122, 0xFB01, 0xFB02, 0x0141, 0x0152, 0x0160,  /* 0x90 */
    0x0178, 0x017D, 0x0131, 0x0142, 0x0153, 0x0161, 0x017E, 0,       /* 0x98 */
    0x20AC,                                                   /* 0xA0 */
};

static unsigned pdfdoc_to_unicode(unsigned char b) {
    unsigned u = b;
    if (b >= 0x18 && b <= 0x1F) u = PDFDOC_18[b - 0x18];
    else if (b >= 0x7F && b <= 0xA0) u = PDFDOC_7F[b - 0x7F];
    else if (b == 0xAD) u = 0;
    return u ? u : 0xFFFD;
}

// Append code point `cp` as UTF-8 if it fits (leaving room for the NUL).
static int put_cp(char *out, size_t cap, size_t *k, unsigned cp) {
    unsigned char e[4];
    size_t n;
    if (cp == 0) return 1;                                    // dropped
    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;            // lone surrogate
    if (cp < 0x80)         { e[0] = (unsigned char)cp; n = 1; }
    else if (cp < 0x800)   { e[0] = (unsigned char)(0xC0 | (cp >> 6));
                             e[1] = (unsigned char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { e[0] = (unsigned char)(0xE0 | (cp >> 12));
                             e[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                             e[2] = (unsigned char)(0x80 | (cp & 0x3F)); n = 3; }
    else                   { e[0] = (unsigned char)(0xF0 | (cp >> 18));
                             e[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
                             e[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                             e[3] = (unsigned char)(0x80 | (cp & 0x3F)); n = 4; }
    if (*k + n >= cap) return 0;                              // full: stop here
    memcpy(out + *k, e, n);
    *k += n;
    return 1;
}

// Are bytes `b` valid UTF-8 containing at least one multi-byte sequence?
static int utf8_with_non_ascii(const unsigned char *b, size_t n) {
    int multi = 0;
    for (size_t i = 0; i < n; ) {
        unsigned c = b[i];
        size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
                   : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!len || i + len > n || (len == 2 && c < 0xC2)) return 0;   // bad lead/overlong
        for (size_t j = 1; j < len; j++)
            if ((b[i + j] & 0xC0) != 0x80) return 0;
        if (len > 1) multi = 1;
        i += len;
    }
    return multi;
}

// Decode UTF-8 bytes (no BOM) into `out`, replacing malformed bytes with U+FFFD.
static size_t utf8_bytes_copy(const unsigned char *b, size_t n, char *out, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; i < n; ) {
        // utf8_next needs a NUL-terminated view; copy one sequence at a time.
        unsigned char seq[5] = {0};
        size_t sl = n - i < 4 ? n - i : 4;
        memcpy(seq, b + i, sl);
        const unsigned char *q = seq;
        unsigned cp = utf8_next(&q);
        size_t used = (size_t)(q - seq);
        if (cp >= 0x80 && used == 1) cp = 0xFFFD;             // malformed byte
        i += used ? used : 1;
        if (!put_cp(out, cap, &k, cp)) break;
    }
    out[k] = '\0';
    return k;
}

size_t pdf_text_bytes_utf8(const unsigned char *b, size_t n, char *out, size_t cap) {
    size_t k = 0;
    if (n >= 2 && b[0] == 0xFE && b[1] == 0xFF) {             // UTF-16BE
        for (size_t i = 2; i + 1 < n; i += 2) {
            unsigned cp = ((unsigned)b[i] << 8) | b[i + 1];
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < n) {  // surrogate pair
                unsigned lo = ((unsigned)b[i + 2] << 8) | b[i + 3];
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            if (!put_cp(out, cap, &k, cp)) break;
        }
    } else if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {   // UTF-8
        return utf8_bytes_copy(b + 3, n - 3, out, cap);
    } else {                                                  // PDFDocEncoding
        for (size_t i = 0; i < n; i++)
            if (!put_cp(out, cap, &k, pdfdoc_to_unicode(b[i]))) break;
    }
    out[k] = '\0';
    return k;
}

// Raw bytes of the string at `v` (literal or hex) into a malloc'd buffer.
// Returns NULL if `v` is not a string; sets *n and *end (past the string).
static unsigned char *string_bytes(const char *v, size_t *n, const char **end) {
    *n = 0;
    if (*v == '(') {
        size_t raw = (size_t)(skip_value(v) - v);
        unsigned char *b = malloc(raw + 1);
        if (!b) return NULL;
        *end = pdf_literal_bytes(v, b, raw, n);
        return b;
    }
    if (*v == '<' && v[1] != '<') {
        const char *p = v + 1;
        const char *close = strchr(p, '>');
        size_t raw = close ? (size_t)(close - p) : strlen(p);
        unsigned char *b = malloc(raw / 2 + 2);
        if (!b) return NULL;
        int hi = -1;
        for (; *p && *p != '>'; p++) {
            int d;
            if (*p >= '0' && *p <= '9') d = *p - '0';
            else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
            else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
            else continue;                                    // whitespace
            if (hi < 0) { hi = d; continue; }
            b[(*n)++] = (unsigned char)(hi * 16 + d);
            hi = -1;
        }
        if (hi >= 0) b[(*n)++] = (unsigned char)(hi * 16);    // odd digit pads with 0
        *end = *p == '>' ? p + 1 : p;
        return b;
    }
    return NULL;
}

// Decode bytes as a text string; `input` enables the raw-UTF-8 allowance.
static size_t text_utf8(const unsigned char *b, size_t n, char *out, size_t cap, int input) {
    int bom = n >= 2 && ((b[0] == 0xFE && b[1] == 0xFF) ||
                         (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF));
    if (input && !bom && utf8_with_non_ascii(b, n)) return utf8_bytes_copy(b, n, out, cap);
    return pdf_text_bytes_utf8(b, n, out, cap);
}

static const char *text_fixed(const char *v, char *out, size_t cap, int input) {
    out[0] = '\0';
    size_t n;
    const char *end = v;
    unsigned char *b = string_bytes(v, &n, &end);
    if (!b) return NULL;
    text_utf8(b, n, out, cap, input);
    free(b);
    return end;
}

static char *text_alloc(const char *v, int input) {
    size_t n;
    const char *end;
    unsigned char *b = string_bytes(v, &n, &end);
    if (!b) return NULL;
    size_t cap = 3 * n + 1;
    char *out = malloc(cap);
    if (out) {
        size_t k = text_utf8(b, n, out, cap, input);
        char *shrunk = realloc(out, k + 1);
        if (shrunk) out = shrunk;
    }
    free(b);
    return out;
}

const char *pdf_text(const char *v, char *out, size_t cap) { return text_fixed(v, out, cap, 0); }
char *pdf_text_alloc(const char *v) { return text_alloc(v, 0); }
const char *pdf_text_input(const char *v, char *out, size_t cap) { return text_fixed(v, out, cap, 1); }
char *pdf_text_input_alloc(const char *v) { return text_alloc(v, 1); }

char *extract_dict_inner_alloc(const char *obj_dict) {
    const char *open = strstr(obj_dict, "<<");
    if (!open) return NULL;
    const char *end = match_dict_end(open);
    if (!end) return NULL;
    const char *inner = open + 2;
    size_t len = (size_t)((end - 2) - inner);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, inner, len);
    out[len] = '\0';
    return out;
}

int parse_ref_num(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return atoi(p);
}

void parse_indirect_ref(const char *p, int *num, int *gen) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    *num = atoi(p);
    while (isdigit((unsigned char)*p)) p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    *gen = atoi(p);
}

// Does `v` start with an indirect reference "N G R" (R followed by a delimiter)?
int is_indirect_ref(const char *v) {
    const char *p = v;
    if (!isdigit((unsigned char)*p)) return 0;
    while (isdigit((unsigned char)*p)) p++;
    if (!isspace((unsigned char)*p)) return 0;
    while (isspace((unsigned char)*p)) p++;
    if (!isdigit((unsigned char)*p)) return 0;
    while (isdigit((unsigned char)*p)) p++;
    if (!isspace((unsigned char)*p)) return 0;
    while (isspace((unsigned char)*p)) p++;
    return *p == 'R' && (p[1] == '\0' || isspace((unsigned char)p[1]) ||
                         strchr("/<>[]()%", p[1]) != NULL);
}
