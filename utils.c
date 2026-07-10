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

#include "utils.h"
#include "pdf_lex.h"          // dict_int (for /Predictor /DecodeParms parameters)
#include <stdint.h>

/* ==========================================================================
 * Cross-reference table (dynamic, O(1) dedup + lookup).
 * ========================================================================== */
void xref_init(XRefTable *t) {
    t->entries = NULL;
    t->count = 0;
    t->cap = 0;
    t->index = NULL;
    t->index_cap = 0;
}

void xref_free(XRefTable *t) {
    if (!t) return;
    free(t->entries);
    free(t->index);
    xref_init(t);
}

// Grow the obj_num -> index map to cover `obj_num`, filling new slots with -1.
static int xref_index_ensure(XRefTable *t, int obj_num) {
    if (obj_num < t->index_cap) return 1;
    int ncap = t->index_cap ? t->index_cap : 256;
    while (ncap <= obj_num) ncap *= 2;
    int *ni = realloc(t->index, (size_t)ncap * sizeof(int));
    if (!ni) return 0;
    for (int i = t->index_cap; i < ncap; i++) ni[i] = -1;
    t->index = ni;
    t->index_cap = ncap;
    return 1;
}

// Append an entry with pre-filled fields. Deduplicates in O(1): if obj_num is
// already present (from a newer xref section, visited first) it is kept.
static int xref_add_entry(XRefTable *t, int obj_num, long offset, int stream_obj, int stream_idx) {
    if (obj_num <= 0 || obj_num > XREF_MAX_OBJECTS) return 0;
    if (!xref_index_ensure(t, obj_num)) return 0;
    if (t->index[obj_num] >= 0) return 0;          // already present -> newest wins

    if (t->count >= t->cap) {
        int ncap = t->cap ? t->cap * 2 : 256;
        XRefEntry *ne = realloc(t->entries, (size_t)ncap * sizeof(XRefEntry));
        if (!ne) return 0;
        t->entries = ne;
        t->cap = ncap;
    }
    t->index[obj_num] = t->count;
    t->entries[t->count].obj_num = obj_num;
    t->entries[t->count].offset = offset;
    t->entries[t->count].stream_obj = stream_obj;
    t->entries[t->count].stream_idx = stream_idx;
    t->count++;
    return 1;
}

// Add an uncompressed object (byte offset).
int xref_add(XRefTable *t, int obj_num, long offset) {
    return xref_add_entry(t, obj_num, offset, 0, 0);
}

// Add a compressed object (lives in object stream `stream_obj` at `stream_idx`).
int xref_add_compressed(XRefTable *t, int obj_num, int stream_obj, int stream_idx) {
    return xref_add_entry(t, obj_num, -1, stream_obj, stream_idx);
}

// O(1) offset lookup by object number. XREF_ABSENT if the object is unknown.
long xref_lookup(const XRefTable *t, int obj_num) {
    const XRefEntry *e = xref_get(t, obj_num);
    return e ? e->offset : XREF_ABSENT;
}

// O(1) entry lookup by object number. NULL if the object is unknown.
const XRefEntry *xref_get(const XRefTable *t, int obj_num) {
    if (obj_num < 0 || obj_num >= t->index_cap) return NULL;
    int i = t->index[obj_num];
    if (i < 0) return NULL;
    return &t->entries[i];
}

// Check if string looks like a PDF drawing command
int is_pdf_command(const char *str) {
    if (strlen(str) <= 3) {
        // Common single/double/triple letter PDF commands
        const char *commands[] = {"q", "Q", "n", "w", "W", "S", "s", "f", "F", "B", "b", 
                                 "BT", "ET", "Tf", "Td", "TL", "Tj", "re", "cm", "gs", 
                                 "RG", "rg", "BMC", "EMC", "BDC", NULL};
        for (int i = 0; commands[i]; i++) {
            if (strcmp(str, commands[i]) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

// FLATE decompression using zlib
// Ceiling on a single stream's decompressed size -- a defense against
// decompression bombs (a tiny compressed stream that inflates to gigabytes).
// Overridable via PDF_MAX_DECOMPRESSED (bytes); default 256 MB, generous for
// forms. Exceeding it makes the decoder return NULL, which every caller already
// treats as a corrupt stream.
size_t decompress_limit(void) {
    const char *e = getenv("PDF_MAX_DECOMPRESSED");
    if (e) { long v = atol(e); if (v > 0) return (size_t)v; }
    return 256u * 1024 * 1024;
}

// Cumulative decompressed bytes this run. Bounds the total across every stream
// (the object-stream cache retains all it decompresses), so a file packed with
// many near-limit streams cannot accumulate unbounded memory/CPU. Reset per run
// by decompress_reset() (called at the start of build_field_map).
static size_t g_decompressed_total = 0;

void decompress_reset(void) { g_decompressed_total = 0; }

static size_t decompress_total_limit(void) {
    const char *e = getenv("PDF_MAX_TOTAL_DECOMPRESSED");
    if (e) { long v = atol(e); if (v > 0) return (size_t)v; }
    return 1024u * 1024 * 1024;   // 1 GB per run
}

// Charge `n` decompressed bytes to the run budget. Returns 0 if within budget,
// -1 if it would exceed it (overflow-safe: compares against remaining headroom).
static int decompress_account(size_t n) {
    size_t lim = decompress_total_limit();
    if (n > lim - g_decompressed_total) return -1;
    g_decompressed_total += n;
    return 0;
}

char* decompress_flate(const char *data, size_t data_len, size_t *out_len) {
    z_stream strm = {0};
    if (inflateInit(&strm) != Z_OK) {
        return NULL;
    }
    size_t limit = decompress_limit();

    // Grow the output buffer as needed: the decompressed size is unbounded
    // relative to the input (xref/object streams routinely expand 5x or more),
    // so a fixed guess would truncate and fail with Z_BUF_ERROR.
    size_t cap = data_len ? data_len * 4 + 64 : 256;
    if (cap > limit) cap = limit ? limit : 256;
    char *output = malloc(cap);
    if (!output) {
        inflateEnd(&strm);
        return NULL;
    }

    strm.next_in = (unsigned char*)data;
    strm.avail_in = data_len;
    strm.next_out = (unsigned char*)output;
    strm.avail_out = cap;

    int ret;
    do {
        if (strm.avail_out == 0) {                 // ran out of room -> grow
            if (cap >= limit) {                    // decompression-bomb guard
                free(output); inflateEnd(&strm); return NULL;
            }
            size_t used = cap;
            size_t ncap = cap * 2;
            if (ncap > limit) ncap = limit;        // grow up to the ceiling, then stop
            char *grown = realloc(output, ncap);
            if (!grown) { free(output); inflateEnd(&strm); return NULL; }
            output = grown;
            cap = ncap;
            strm.next_out = (unsigned char*)(output + used);
            strm.avail_out = cap - used;
        }
        ret = inflate(&strm, Z_NO_FLUSH);
    } while (ret == Z_OK);

    if (ret != Z_STREAM_END) {
        free(output);
        inflateEnd(&strm);
        return NULL;
    }

    *out_len = cap - strm.avail_out;
    inflateEnd(&strm);
    if (decompress_account(*out_len) != 0) { free(output); return NULL; }
    // NUL-terminate so callers can safely run string ops (strstr/find_key) on
    // the result; the terminator sits past *out_len and never affects length.
    if (*out_len + 1 > cap) {
        char *g = realloc(output, *out_len + 1);
        if (!g) { free(output); return NULL; }
        output = g;
    }
    output[*out_len] = '\0';
    return output;
}

// Decode PDF LZWDecode data: variable-width, MSB-first codes that grow 9->12
// bits. Codes 0-255 are literal bytes, 256 clears the table, 257 is end-of-data;
// new codes start at 258. EarlyChange is 1 (the PDF default): the code width
// grows one code before the table is actually full. Returns a malloc'd buffer
// (caller frees) with *out_len set, or NULL on a corrupt stream.
char* decompress_lzw(const char *data, size_t data_len, size_t *out_len) {
    const int EARLY = 1;
    int prefix[4096];                                 // table entry -> previous code
    unsigned char append[4096];                       // table entry -> appended byte
    unsigned char stack[4096];                        // string reconstruction (reversed)
    size_t limit = decompress_limit();                // decompression-bomb guard

    size_t cap = data_len ? data_len * 4 + 64 : 256;
    unsigned char *out = malloc(cap);
    if (!out) return NULL;
    size_t olen = 0;

    int width = 9, next_code = 258, old = -1;
    unsigned long bitbuf = 0;
    int bitcnt = 0;
    size_t pos = 0;

    for (;;) {
        while (bitcnt < width && pos < data_len) {    // refill the bit buffer, MSB-first
            bitbuf = (bitbuf << 8) | (unsigned char)data[pos++];
            bitcnt += 8;
        }
        if (bitcnt < width) break;                    // no full code remains
        int code = (int)((bitbuf >> (bitcnt - width)) & ((1u << width) - 1));
        bitcnt -= width;

        if (code == 256) { width = 9; next_code = 258; old = -1; continue; }   // clear
        if (code == 257) break;                                                // EOD

        if (old < 0) {                                // first code after a clear/start
            if (code > 255) { free(out); return NULL; }
            if (olen + 1 > cap) { cap = cap*2 + 16; unsigned char *g = realloc(out, cap); if (!g) { free(out); return NULL; } out = g; }
            out[olen++] = (unsigned char)code;
            old = code;
            continue;
        }

        int cur;
        if (code < next_code)      cur = code;        // in table
        else if (code == next_code) cur = old;        // KwKwK: not yet in table
        else { free(out); return NULL; }              // corrupt

        int sp = 0, c = cur;                          // reconstruct string(cur) reversed
        while (c >= 256) { stack[sp++] = append[c]; c = prefix[c]; }
        unsigned char firstchar = (unsigned char)c;
        stack[sp++] = firstchar;

        size_t need = olen + sp + 1;                  // +1 for the KwKwK repeat
        if (need > limit) { free(out); return NULL; } // decompression-bomb guard
        if (need > cap) { while (need > cap) cap = cap*2 + 16; unsigned char *g = realloc(out, cap); if (!g) { free(out); return NULL; } out = g; }
        for (int i = sp - 1; i >= 0; i--) out[olen++] = stack[i];
        if (code == next_code) out[olen++] = firstchar;

        if (next_code < 4096) {                        // add old-string + firstchar
            prefix[next_code] = old;
            append[next_code] = firstchar;
            next_code++;
            if (width < 12 && next_code >= (1 << width) - EARLY) width++;
        }
        old = code;
    }

    *out_len = olen;
    if (decompress_account(olen) != 0) { free(out); return NULL; }
    if (olen + 1 > cap) {                              // ensure room for the terminator
        unsigned char *g = realloc(out, olen + 1);
        if (!g) { free(out); return NULL; }
        out = g;
    }
    out[olen] = '\0';                                  // NUL-terminate (see decompress_flate)
    return (char *)out;
}

// Undo a PNG predictor (Predictor >= 10). `data` is rows of (columns+1) bytes;
// returns malloc'd rows*columns unfiltered bytes (caller frees) or NULL.
static unsigned char *png_unpredict(const unsigned char *data, size_t len,
                                    int columns, int bpp, size_t *out_len) {
    if (columns <= 0 || bpp <= 0) return NULL;
    size_t stride = (size_t)columns + 1;
    size_t rows = len / stride;
    unsigned char *out = malloc(rows * (size_t)columns + 1);
    unsigned char *prev = calloc((size_t)columns, 1);
    if (!out || !prev) { free(out); free(prev); return NULL; }
    const unsigned char *p = data;
    for (size_t r = 0; r < rows; r++) {
        int ft = *p++;
        unsigned char *cur = out + r * (size_t)columns;
        for (int i = 0; i < columns; i++) {
            int a = (i >= bpp) ? cur[i - bpp] : 0;   // left
            int b = prev[i];                          // up
            int c = (i >= bpp) ? prev[i - bpp] : 0;   // upper-left
            int x = p[i], val;
            switch (ft) {
                case 1: val = x + a; break;                       // Sub
                case 2: val = x + b; break;                       // Up
                case 3: val = x + ((a + b) >> 1); break;          // Average
                case 4: {                                         // Paeth
                    int q = a + b - c, pa = abs(q - a), pb = abs(q - b), pc = abs(q - c);
                    val = x + ((pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c));
                    break;
                }
                default: val = x; break;                          // None
            }
            cur[i] = (unsigned char)val;
        }
        memcpy(prev, cur, (size_t)columns);
        p += columns;
    }
    free(prev);
    *out_len = rows * (size_t)columns;
    return out;
}

// Undo TIFF Predictor 2 (horizontal differencing), 8-bit components only. `data`
// is `rows` of (columns*colors) bytes; each sample is the running sum of the
// deltas for its component within the row. Returns malloc'd bytes or NULL.
static unsigned char *tiff2_unpredict(const unsigned char *data, size_t len,
                                      int columns, int colors, size_t *out_len) {
    if (columns <= 0 || colors <= 0) return NULL;
    size_t rowbytes = (size_t)columns * (size_t)colors;
    if (rowbytes == 0) return NULL;
    size_t rows = len / rowbytes;
    unsigned char *out = malloc(rows * rowbytes + 1);
    if (!out) return NULL;
    for (size_t r = 0; r < rows; r++) {
        const unsigned char *in = data + r * rowbytes;
        unsigned char *o = out + r * rowbytes;
        for (size_t i = 0; i < rowbytes; i++)
            o[i] = (unsigned char)(in[i] + ((i >= (size_t)colors) ? o[i - colors] : 0));
    }
    *out_len = rows * rowbytes;
    return out;
}

// Reverse a /Predictor filter, if the stream's /DecodeParms declares one, on
// already-decompressed bytes. Takes ownership of `data` (malloc'd, *len bytes):
// returns it unchanged when there is no predictor, a new buffer (freeing `data`)
// when one is reversed, or NULL (freeing `data`) on an unsupported predictor.
static char *apply_predictor(const char *dict, char *data, size_t *len) {
    const char *dp = dict ? strstr(dict, "/DecodeParms") : NULL;
    int predictor = dp ? dict_int(dp, "/Predictor", 1) : 1;
    if (predictor <= 1) return data;             // Predictor 1 / none: as-is
    int columns = dp ? dict_int(dp, "/Columns", 1) : 1;
    int colors  = dp ? dict_int(dp, "/Colors", 1) : 1;
    int bpc     = dp ? dict_int(dp, "/BitsPerComponent", 8) : 8;
    size_t olen = 0;
    unsigned char *unp = NULL;
    if (predictor >= 10) {                        // PNG predictors
        int bpp = (colors * bpc + 7) / 8; if (bpp < 1) bpp = 1;
        unp = png_unpredict((const unsigned char *)data, *len, columns, bpp, &olen);
    } else if (predictor == 2) {                 // TIFF horizontal differencing
        if (bpc != 8) { free(data); return NULL; }   // only 8-bit components supported
        unp = tiff2_unpredict((const unsigned char *)data, *len, columns, colors, &olen);
    } else {
        return data;                             // unknown predictor value: leave as-is
    }
    free(data);
    if (!unp) return NULL;
    *len = olen;
    return (char *)unp;
}

// Decompress a stream by its dictionary's filter (FlateDecode or LZWDecode) and
// reverse any /Predictor declared in its /DecodeParms, so callers get the fully
// decoded bytes. A stream with no /Filter at all is legal PDF and passes
// through raw; notably, ffpdf's own fill emits its appended xref stream
// unfiltered. Returns malloc'd data or NULL (unsupported filter / error).
char* decompress_stream(const char *dict, const char *data, size_t data_len, size_t *out_len) {
    char *dec;
    if (strstr(dict, "FlateDecode"))    dec = decompress_flate(data, data_len, out_len);
    else if (strstr(dict, "LZWDecode")) dec = decompress_lzw(data, data_len, out_len);
    else if (!strstr(dict, "/Filter")) {
        dec = malloc(data_len + 1);
        if (!dec) return NULL;
        memcpy(dec, data, data_len);
        dec[data_len] = '\0';
        *out_len = data_len;
    }
    else return NULL;                       // unsupported filter
    if (!dec) return NULL;
    return apply_predictor(dict, dec, out_len);
}

// Skip whitespace characters
void skip_whitespace(FILE *f) {
    int c;
    while ((c = fgetc(f)) != EOF && isspace(c));
    if (c != EOF) {
        ungetc(c, f);
    }
}

// Read a PDF token
int read_token(FILE *f, char *token, int max_len) {
    skip_whitespace(f);
    
    int i = 0;
    int c;
    
    while ((c = fgetc(f)) != EOF && i < max_len - 1) {
        if (isspace(c)) {
            break;
        }
        if (c == '<' || c == '>' || c == '[' || c == ']' || c == '(' || c == ')' || c == '/') {
            if (i == 0) {
                token[i++] = c;
                if (c == '<' || c == '>') {
                    int next = fgetc(f);
                    if ((c == '<' && next == '<') || (c == '>' && next == '>')) {
                        token[i++] = next;
                    } else if (next != EOF) {
                        ungetc(next, f);
                    }
                }
                break;
            } else {
                ungetc(c, f);
                break;
            }
        }
        token[i++] = c;
    }
    
    token[i] = '\0';
    return i > 0;
}

// Parse object reference (e.g., "123 0 R" -> 123)
int parse_object_ref(const char *ref) {
    int obj_num;
    if (sscanf(ref, "%d", &obj_num) == 1) {
        return obj_num;
    }
    return 0;
}

// Find value for a key in dictionary string
char* find_dict_value(const char *dict, const char *key, char *value, size_t max_len) {
    // Look for "/ Key " pattern
    char search_pattern[MAX_LINE];
    snprintf(search_pattern, sizeof(search_pattern), "/ %s ", key);
    
    char *pos = strstr(dict, search_pattern);
    if (!pos) {
        // Try without space before key
        snprintf(search_pattern, sizeof(search_pattern), "/%s ", key);
        pos = strstr(dict, search_pattern);
    }
    
    if (!pos) return NULL;
    
    // Move to after the pattern
    pos += strlen(search_pattern);
    
    // Skip any additional whitespace
    while (*pos && isspace(*pos)) pos++;
    
    size_t i = 0;
    // Read until we hit whitespace, slash, or bracket
    while (*pos && i + 1 < max_len && !isspace((unsigned char)*pos) && *pos != '/' && *pos != '[' && *pos != '<' && *pos != '>') {
        value[i++] = *pos++;
    }
    value[i] = '\0';
    
    return strlen(value) > 0 ? value : NULL;
}
