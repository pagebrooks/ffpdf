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

#define _POSIX_C_SOURCE 200809L
#include "field_map.h"
#include "pdf_lex.h"          // find_key, skip_value, read_literal, parse_ref_num, ...
#include "pdf_parser.h"       // find_startxref, parse_obj_at_offset
#include "crypto.h"           // PdfCrypt, pdf_crypt_init, pdf_decrypt_dict_strings

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>


/* ==========================================================================
 * Lazy object-stream cache.
 *
 * Object streams are decompressed on first access and kept, so only the streams
 * actually needed to resolve requested objects are ever inflated (once each) --
 * rather than decompressing every stream in the file up front. The xref stream's
 * type-2 entries tell us each compressed object's containing stream and index
 * directly, so no /Extends chain walking is needed for lookup.
 * ========================================================================== */
typedef struct {
    int stream_obj;       // the /ObjStm container object number
    char *data;           // decompressed bytes
    size_t len;
    int first;            // /First: offset of the first object's data
    int n;                // number of objects
    int *onum;            // [n] object numbers (from the index table)
    int *ooff;            // [n] object offsets, relative to `first`
} ObjStm;

// All document-scoped reader state in one place (this is a single-document CLI,
// so it is file-static rather than threaded through every call). Reset between
// runs by objstm_cache_reset(); the crypt half is (re)initialized per run by
// pdf_crypt_setup().
static struct {
    ObjStm *objstm;                 // lazy object-stream cache
    int objstm_n, objstm_cap;
    PdfCrypt crypt;                 // document crypt handler, valid iff `encrypted`
    int encrypted;
    int enc_obj;                    // /Encrypt object number (for the fill trailer)
    unsigned char enc_id0[64];      // first /ID element
    int enc_id0_len;
} g_doc;

// The active document crypt handler, or NULL if the document is not encrypted.
const PdfCrypt *pdf_doc_crypt(void) { return g_doc.encrypted ? &g_doc.crypt : NULL; }

void objstm_cache_reset(void) {
    for (int i = 0; i < g_doc.objstm_n; i++) {
        free(g_doc.objstm[i].data);
        free(g_doc.objstm[i].onum);
        free(g_doc.objstm[i].ooff);
    }
    free(g_doc.objstm);
    g_doc.objstm = NULL;
    g_doc.objstm_n = g_doc.objstm_cap = 0;
}

// Decompress + index the object stream `stream_obj` (cached on first access).
static ObjStm *objstm_load(FILE *f, XRefTable *xref, int stream_obj) {
    for (int i = 0; i < g_doc.objstm_n; i++)
        if (g_doc.objstm[i].stream_obj == stream_obj) return &g_doc.objstm[i];

    long off = xref_lookup(xref, stream_obj);
    if (off < 0) return NULL;
    PdfObject o = parse_obj_at_offset(f, off, pdf_doc_crypt());
    if (!o.stream || !strstr(o.dictionary, "/ObjStm")) { free(o.stream); return NULL; }
    size_t dl;
    char *dec = decompress_stream(o.dictionary, o.stream, o.stream_len, &dl);
    free(o.stream);
    if (!dec) return NULL;

    const char *pn = find_key(o.dictionary, "/N");
    const char *pf = find_key(o.dictionary, "/First");
    int N = pn ? atoi(pn) : 0;
    int First = pf ? atoi(pf) : 0;
    if (N <= 0 || First <= 0 || (size_t)First > dl) { free(dec); return NULL; }

    int *onum = malloc((size_t)N * sizeof(int));
    int *ooff = malloc((size_t)N * sizeof(int));
    if (!onum || !ooff) { free(onum); free(ooff); free(dec); return NULL; }

    const char *p = dec, *pend = dec + First;
    int k = 0;
    while (k < N && p < pend) {
        while (p < pend && isspace((unsigned char)*p)) p++;
        if (p >= pend || !isdigit((unsigned char)*p)) break;
        onum[k] = atoi(p); while (p < pend && isdigit((unsigned char)*p)) p++;
        while (p < pend && isspace((unsigned char)*p)) p++;
        if (p >= pend || !isdigit((unsigned char)*p)) break;
        ooff[k] = atoi(p); while (p < pend && isdigit((unsigned char)*p)) p++;
        k++;
    }
    N = k;   // in case the index table was short

    if (g_doc.objstm_n >= g_doc.objstm_cap) {
        int nc = g_doc.objstm_cap ? g_doc.objstm_cap * 2 : 16;
        ObjStm *ng = realloc(g_doc.objstm, (size_t)nc * sizeof(ObjStm));
        if (!ng) { free(onum); free(ooff); free(dec); return NULL; }
        g_doc.objstm = ng;
        g_doc.objstm_cap = nc;
    }
    ObjStm *s = &g_doc.objstm[g_doc.objstm_n++];
    s->stream_obj = stream_obj;
    s->data = dec; s->len = dl;
    s->first = First; s->n = N;
    s->onum = onum; s->ooff = ooff;
    return s;
}

// Full text of a compressed object via the lazy cache. malloc'd, or NULL.
static char *get_compressed_object(FILE *f, XRefTable *xref, int obj_num) {
    const XRefEntry *e = xref_get(xref, obj_num);
    if (!e || e->offset >= 0 || e->stream_obj <= 0) return NULL;
    ObjStm *s = objstm_load(f, xref, e->stream_obj);
    if (!s) return NULL;
    int idx = e->stream_idx;
    if (idx < 0 || idx >= s->n || s->ooff[idx] < 0) return NULL;
    size_t start = (size_t)s->first + (size_t)s->ooff[idx];
    size_t objend = (idx + 1 < s->n) ? (size_t)s->first + (size_t)s->ooff[idx + 1] : s->len;
    if (objend > s->len) objend = s->len;
    // The index table's offsets need not be increasing; a decreasing pair would
    // underflow `len` to ~SIZE_MAX and memcpy off the end of the heap buffer.
    if (start >= s->len || objend < start) return NULL;
    size_t len = objend - start;
    char *out = malloc(len + 1);
    if (out) { memcpy(out, s->data + start, len); out[len] = '\0'; }
    return out;
}

/* ==========================================================================
 * Raw object access.
 *
 * parse_obj_at_offset() reformats dictionaries (inserting spaces after every
 * '/'), while objects decompressed from object streams come back verbatim. To
 * parse and rewrite dictionaries uniformly we always want the *raw* PDF text:
 *   - uncompressed objects: read the bytes straight from the file, and
 *   - compressed objects:   resolve via the lazy object-stream cache.
 * ========================================================================== */

// Read the object at file offset `off` into a growable buffer, stopping once its
// top-level value (a "<<...>>" dictionary or a "[...]" array) is fully present,
// or at EOF. There is no SEEK_END/ftell: fread stops at EOF, so we never read
// past the file, and we start at 8 KB and grow only when the value doesn't fit
// (most field dictionaries are a few hundred bytes). Returns malloc'd
// NUL-terminated bytes (caller frees) with *got set to the length, or NULL.
static char *read_object_bytes(FILE *f, long off, size_t *got_out) {
    fseek(f, off, SEEK_SET);
    size_t cap = 8192, got = 0;
    char *buf = malloc(cap + 1);
    if (!buf) return NULL;
    for (;;) {
        got += fread(buf + got, 1, cap - got, f);
        buf[got] = '\0';

        // Is the object's value complete within what we've read so far?
        int complete = 1;
        const char *v = strstr(buf, "obj");
        if (v) {
            v += 3;
            while (*v == ' ' || *v == '\r' || *v == '\n' || *v == '\t') v++;
            if (v[0] == '<' && v[1] == '<')
                complete = (match_dict_end(v) != NULL);
            else if (*v == '[')
                complete = ((size_t)(skip_value(v) - buf) < got);  // closed before EOB
        }
        if (complete || got < cap) break;                 // done, or reached EOF
        if (cap >= 16u * 1024 * 1024) break;               // runaway guard
        size_t nc = cap * 2;
        char *g = realloc(buf, nc + 1);
        if (!g) break;
        buf = g;
        cap = nc;
    }
    *got_out = got;
    return buf;
}

char *get_object_raw(FILE *f, XRefTable *xref, int obj_num, int *gen_out) {
    if (gen_out) *gen_out = 0;

    // Uncompressed object present in the xref table: read raw bytes (O(1)
    // lookup). A negative offset marks a compressed object -> resolve via the
    // lazy cache below; XREF_ABSENT is also < 0 and falls through.
    long off = xref_lookup(xref, obj_num);
    if (off >= 0) {
        size_t got;
        char *buf = read_object_bytes(f, off, &got);
        if (!buf) return NULL;

        char *out = NULL;
        const char *start = strstr(buf, "<<");
        const char *close = start ? match_dict_end(start) : NULL;
        if (close) {
            if (gen_out) {                            // "N G obj" -> generation
                const char *p = buf;
                while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') p++;
                while (isdigit((unsigned char)*p)) p++;
                while (*p == ' ' || *p == '\t') p++;
                *gen_out = atoi(p);
            }
            size_t len = (size_t)(close - start);
            out = malloc(len + 1);
            if (out) { memcpy(out, start, len); out[len] = '\0'; }
        }
        free(buf);
        // Encrypted document: strings in this uncompressed object are ciphertext.
        // (Objects from object streams are already plaintext -- the whole stream
        // was decrypted when read -- so only this direct-read path decrypts.)
        const PdfCrypt *crypt = pdf_doc_crypt();
        if (out && crypt) {
            int gen = gen_out ? *gen_out : 0;
            char *dec = pdf_decrypt_dict_strings(crypt, obj_num, gen, out);
            if (dec) { free(out); out = dec; }
        }
        return out;
    }

    // Compressed object (in an object stream): resolve lazily via the cache
    // (objects in object streams always have generation 0).
    char *full = get_compressed_object(f, xref, obj_num);
    if (full && gen_out) *gen_out = 0;
    return full;
}

// Read the raw "[...]" text of an indirect array object (e.g. object 5 in
// "/Fields 5 0 R"). Returns malloc'd text or NULL.
static char *read_array_object(FILE *f, XRefTable *xref, int num) {
    long off = xref_lookup(xref, num);
    if (off >= 0) {
        size_t got;
        char *buf = read_object_bytes(f, off, &got);
        if (!buf) return NULL;

        const char *kw = strstr(buf, "obj");          // past "N G obj"
        const char *lb = kw ? strchr(kw, '[') : NULL;
        char *out = NULL;
        if (lb) {
            const char *e = skip_value(lb);           // just past matching ']'
            size_t len = (size_t)(e - lb);
            out = malloc(len + 1);
            if (out) { memcpy(out, lb, len); out[len] = '\0'; }
        }
        free(buf);
        return out;
    }

    // Compressed object: extract its array from the lazily resolved text.
    char *full = get_compressed_object(f, xref, num);
    char *out = NULL;
    const char *lb = full ? strchr(full, '[') : NULL;
    if (lb) {
        const char *e = skip_value(lb);
        size_t len = (size_t)(e - lb);
        out = malloc(len + 1);
        if (out) { memcpy(out, lb, len); out[len] = '\0'; }
    }
    free(full);
    return out;
}

char *field_array_text(FILE *f, XRefTable *xref, const char *dict, const char *key) {
    const char *v = find_key(dict, key);
    if (!v) return NULL;
    if (*v == '[') {
        const char *e = skip_value(v);                // just past matching ']'
        size_t len = (size_t)(e - v);
        char *out = malloc(len + 1);
        if (out) { memcpy(out, v, len); out[len] = '\0'; }
        return out;
    }
    if (isdigit((unsigned char)*v))                   // indirect reference "N G R"
        return read_array_object(f, xref, atoi(v));
    return NULL;
}

/* ==========================================================================
 * Field-tree walk.
 * ========================================================================== */
static void fieldmap_add(FieldMap *m, const char *qname, int obj_num, int gen, char ftype) {
    if (m->count >= m->cap) {
        int ncap = m->cap ? m->cap * 2 : 64;
        FieldLoc *ni = realloc(m->items, ncap * sizeof(FieldLoc));
        if (!ni) return;
        m->items = ni;
        m->cap = ncap;
    }
    FieldLoc *fl = &m->items[m->count++];
    snprintf(fl->qname, sizeof(fl->qname), "%s", qname);
    fl->obj_num = obj_num;
    fl->gen_num = gen;
    fl->ftype = ftype;
    fl->terminal = 0;
}

// Classify a field from its /FT entry (falls back to inherited type).
static char field_type_from_dict(const char *dict, char inherited) {
    const char *ft = find_key(dict, "/FT");
    if (!ft) return inherited;
    if (strncmp(ft, "/Tx", 3) == 0) return 'T';
    if (strncmp(ft, "/Btn", 4) == 0) return 'B';
    if (strncmp(ft, "/Ch", 3) == 0) return 'C';
    if (strncmp(ft, "/Sig", 4) == 0) return 'S';
    return inherited;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Read the partial field name (/T) into `out`. Handles both literal strings
// "(name)" and hex strings "<FEFF..>" (UTF-16BE, as produced by LiveCycle).
// Returns 1 if a name is present.
static int read_partial_name(const char *dict, char *out, size_t out_size) {
    const char *t = find_key(dict, "/T");
    if (!t) return 0;
    size_t k = 0;

    if (*t == '(') {
        t++;
        while (*t && *t != ')' && k < out_size - 1) {
            if (*t == '\\' && t[1]) { t++; }   // unescape
            out[k++] = *t++;
        }
        out[k] = '\0';
        return 1;
    }

    if (*t == '<') {                            // hex string, UTF-16BE
        t++;
        int nib[2]; int ncount = 0;
        int high_byte = -1;
        while (*t && *t != '>' && k < out_size - 1) {
            int v = hexval((unsigned char)*t);
            t++;
            if (v < 0) continue;
            nib[ncount++] = v;
            if (ncount == 2 && high_byte < 0) { high_byte = nib[0] * 16 + nib[1]; ncount = 0; continue; }
            if (ncount == 2 && high_byte >= 0) {
                int low_byte = nib[0] * 16 + nib[1];
                ncount = 0;
                if (high_byte == 0xFE && low_byte == 0xFF) { high_byte = -1; continue; } // BOM
                if (high_byte == 0 && low_byte >= 32 && low_byte < 127)
                    out[k++] = (char)low_byte;                  // ASCII code point
                else
                    out[k++] = '?';                             // non-ASCII placeholder
                high_byte = -1;
            }
        }
        out[k] = '\0';
        return k > 0;
    }

    return 0;
}

// Shared state for one field-tree walk.
typedef struct {
    FILE *f;
    XRefTable *xref;
    FieldMap *map;
    unsigned char *seen;   // seen[obj_num] != 0 once visited (cycle guard)
    int seen_max;          // highest valid index into `seen`
} WalkCtx;

static void walk_fields(WalkCtx *ctx, int obj_num, const char *parent_qname,
                        char inherited_ft, int depth);

// Iterate the "N G R" object references in a resolved field array (starting at
// '['), recursing into each child.
static void walk_field_refs(WalkCtx *ctx, const char *arr,
                            const char *parent_qname, char ftype, int depth) {
    const char *p = arr;
    if (*p == '[') p++;
    while (*p && *p != ']') {
        if (isdigit((unsigned char)*p)) {
            int child = atoi(p);
            while (isdigit((unsigned char)*p)) p++;      // obj num
            while (*p == ' ' || *p == '\t') p++;
            while (isdigit((unsigned char)*p)) p++;      // gen
            while (*p == ' ' || *p == '\t') p++;
            if (*p == 'R') p++;                          // 'R'
            walk_fields(ctx, child, parent_qname, ftype, depth);
        } else {
            p++;
        }
    }
}

// Recursively walk the field tree, recording every named node with its object
// number and qualified name. A node is flagged `terminal` when it has a name but
// no named descendant (i.e. a leaf/fillable field). Kids are followed even when
// they live in compressed object streams, and /Kids may be inline or indirect.
// Each object is visited at most once (a well-formed field tree is acyclic; the
// seen-set prevents runaway recursion on malformed /Kids cycles).
static void walk_fields(WalkCtx *ctx, int obj_num, const char *parent_qname,
                        char inherited_ft, int depth) {
    if (depth > 32 || obj_num <= 0) return;
    if (obj_num <= ctx->seen_max) {
        if (ctx->seen[obj_num]) return;      // already visited -> cycle/shared node
        ctx->seen[obj_num] = 1;
    }

    FILE *f = ctx->f;
    XRefTable *xref = ctx->xref;
    FieldMap *map = ctx->map;

    int gen = 0;
    char *dict = get_object_raw(f, xref, obj_num, &gen);
    if (!dict) return;

    char partial[256];
    int has_name = read_partial_name(dict, partial, sizeof(partial));
    char ftype = field_type_from_dict(dict, inherited_ft);
    if (getenv("FILL_DEBUG"))
        fprintf(stderr, "[dbg] walk obj %d depth %d name=%s(%d) ft=%c dict=%.120s\n",
                obj_num, depth, has_name ? partial : "-", has_name, ftype ? ftype : '?', dict);

    // Build the qualified name "parent.partial" with explicit bounds (memcpy,
    // not snprintf, so there's no format-truncation ambiguity and the rare
    // too-deep case has defined behavior: fall back to the leaf name).
    char qname[256];
    if (has_name && parent_qname[0]) {
        size_t pl = strlen(parent_qname), sl = strlen(partial);
        if (pl + 1 + sl < sizeof(qname)) {
            memcpy(qname, parent_qname, pl);
            qname[pl] = '.';
            memcpy(qname + pl + 1, partial, sl);
            qname[pl + 1 + sl] = '\0';
        } else {
            size_t n = sl < sizeof(qname) - 1 ? sl : sizeof(qname) - 1;
            memcpy(qname, partial, n);
            qname[n] = '\0';
        }
    } else {
        const char *src = has_name ? partial : parent_qname;
        size_t n = strlen(src);
        if (n >= sizeof(qname)) n = sizeof(qname) - 1;
        memcpy(qname, src, n);
        qname[n] = '\0';
    }

    int self_idx = -1;
    if (has_name) {
        fieldmap_add(map, qname, obj_num, gen, ftype);
        self_idx = map->count - 1;
    }
    int before_kids = map->count;

    // Descend into /Kids (inline array or indirect reference). Widgets have no
    // /T and simply contribute nothing.
    char *kids = field_array_text(f, xref, dict, "/Kids");
    if (kids) {
        walk_field_refs(ctx, kids, qname, ftype, depth + 1);
        free(kids);
    }

    // A named node with no named descendant is a terminal (fillable) field.
    if (self_idx >= 0 && map->count == before_kids)
        map->items[self_idx].terminal = 1;

    free(dict);
}

/* ==========================================================================
 * Locating the AcroForm and building the map.
 * ========================================================================== */
int find_acroform_obj(FILE *f, XRefTable *xref, int root_obj) {
    char *cat = get_object_raw(f, xref, root_obj, NULL);
    if (!cat) return 0;
    const char *ap = strstr(cat, "/AcroForm");
    int result = 0;
    if (ap) result = parse_ref_num(ap + 9);
    free(cat);
    return result;
}

// Resolve the /Root (catalog) object number from the trailer / xref-stream dict.
static int find_root_obj(FILE *f) {
    long sx = find_startxref(f);
    if (sx < 0) return 0;
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    long span = end - sx;
    if (span <= 0) return 0;
    char *buf = malloc((size_t)span + 1);
    if (!buf) return 0;
    fseek(f, sx, SEEK_SET);
    size_t got = fread(buf, 1, (size_t)span, f);
    buf[got] = '\0';
    const char *rp = strstr(buf, "/Root");
    int root = rp ? parse_ref_num(rp + 5) : 0;
    free(buf);
    return root;
}

int pdf_doc_encrypt_obj(void) { return g_doc.encrypted ? g_doc.enc_obj : 0; }
int pdf_doc_id0(unsigned char *out, int cap) {
    if (!g_doc.encrypted) return 0;
    int n = g_doc.enc_id0_len < cap ? g_doc.enc_id0_len : cap;
    memcpy(out, g_doc.enc_id0, n);
    return n;
}

// Detect /Encrypt in the trailer / xref-stream dict and, for the empty user
// password, initialize the document crypt handler so subsequent object reads
// decrypt. No-op (and clears any previous handler) when the document is not
// encrypted or the handler is unsupported.
static void pdf_crypt_setup(FILE *f, XRefTable *xref) {
    g_doc.encrypted = 0;
    long sx = find_startxref(f);
    if (sx < 0) return;
    fseek(f, 0, SEEK_END);
    long end = ftell(f), span = end - sx;
    if (span <= 0) return;
    char *buf = malloc((size_t)span + 1);
    if (!buf) return;
    fseek(f, sx, SEEK_SET);
    size_t got = fread(buf, 1, (size_t)span, f);
    buf[got] = '\0';

    const char *ep = strstr(buf, "/Encrypt");
    if (!ep) { free(buf); return; }
    int enc_obj = parse_ref_num(ep + 8);

    // First element of the /ID array (part of the R2-4 key derivation).
    unsigned char id0[64];
    int id0_len = 0;
    const char *ip = strstr(buf, "/ID");
    const char *b = ip ? strchr(ip, '[') : NULL;
    if (b) {
        b++;
        while (*b == ' ' || *b == '\n' || *b == '\r' || *b == '\t') b++;
        if (*b == '<') {
            b++;
            int hi = -1;
            while (*b && *b != '>' && id0_len < (int)sizeof(id0)) {
                int d; char ch = *b++;
                if (ch >= '0' && ch <= '9') d = ch - '0';
                else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
                else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
                else continue;
                if (hi < 0) hi = d; else { id0[id0_len++] = (unsigned char)(hi*16 + d); hi = -1; }
            }
            if (hi >= 0 && id0_len < (int)sizeof(id0)) id0[id0_len++] = (unsigned char)(hi*16);
        } else if (*b == '(') {
            b++;
            while (*b && *b != ')' && id0_len < (int)sizeof(id0)) {
                if (*b == '\\' && b[1]) { b++; id0[id0_len++] = (unsigned char)*b++; continue; }
                id0[id0_len++] = (unsigned char)*b++;
            }
        }
    }
    free(buf);
    if (enc_obj <= 0) return;

    // Read the /Encrypt dictionary raw -- the handler is not active yet, so its
    // /O /U /Perms strings (which are never encrypted) are read verbatim.
    char *ed = get_object_raw(f, xref, enc_obj, NULL);
    if (ed) {
        if (pdf_crypt_init(&g_doc.crypt, ed, id0, id0_len)) {
            g_doc.encrypted = 1;
            g_doc.enc_obj = enc_obj;
            g_doc.enc_id0_len = id0_len < (int)sizeof(g_doc.enc_id0) ? id0_len : (int)sizeof(g_doc.enc_id0);
            memcpy(g_doc.enc_id0, id0, g_doc.enc_id0_len);
            fprintf(stderr, "Encrypted document: standard handler V%d R%d, %s (empty user password)\n",
                    g_doc.crypt.V, g_doc.crypt.R,
                    g_doc.crypt.cfm == CFM_RC4 ? "RC4" : g_doc.crypt.cfm == CFM_AESV2 ? "AES-128" : "AES-256");
        } else {
            fprintf(stderr, "WARNING: /Encrypt present but unsupported or non-empty password; "
                            "reads will fail\n");
        }
        free(ed);
    }
}

int build_field_map(FILE *f, XRefTable *xref, int root_obj, FieldMap *map) {
    decompress_reset();       // fresh per-run decompression budget
    pdf_crypt_setup(f, xref);
    if (root_obj <= 0) root_obj = find_root_obj(f);
    if (root_obj <= 0) return 0;

    int acroform = find_acroform_obj(f, xref, root_obj);
    if (acroform <= 0) return 0;

    // Cycle guard sized to the highest object number in the xref table.
    int seen_max = 0;
    for (int i = 0; i < xref->count; i++)
        if (xref->entries[i].obj_num > seen_max) seen_max = xref->entries[i].obj_num;
    unsigned char *seen = calloc((size_t)seen_max + 1, 1);

    WalkCtx ctx = { f, xref, map, seen, seen ? seen_max : -1 };

    char *afd = get_object_raw(f, xref, acroform, NULL);
    char *arr = afd ? field_array_text(f, xref, afd, "/Fields") : NULL;
    if (arr) {
        walk_field_refs(&ctx, arr, "", 0, 0);
        free(arr);
    }
    free(afd);
    free(seen);
    return acroform;
}

void field_map_free(FieldMap *map) {
    if (!map) return;
    free(map->items);
    map->items = NULL;
    map->count = map->cap = 0;
}
