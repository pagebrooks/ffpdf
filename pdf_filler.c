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
#include "pdf_filler.h"
#include "pdf_lex.h"          // find_key, read_literal, parse_ref_num, dict helpers
#include "field_map.h"        // build_field_map, get_object_raw, field_array_text
#include "pdf_parser.h"       // find_startxref, parse_xref_table
#include "xfa.h"              // xfa_datasets_set
#include "crypto.h"           // pdf_encrypt* (re-encrypt appended objects)
#include "os_compat.h"        // portable_fopen (UTF-8 paths on Windows)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

// Filled values may be PHI/PII, so they are never logged by default -- progress
// output shows the field name and byte length only. Set FFPDF_VERBOSE=1 to
// include the values (debugging on non-sensitive data).
static int log_field_values(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("FFPDF_VERBOSE"); v = (e && *e && *e != '0'); }
    return v;
}

/* ==========================================================================
 * Small growable byte buffer (the filled PDF is assembled here so that byte
 * offsets for the cross-reference stream are simply the current length).
 * ========================================================================== */
typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} Buf;

static int buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra <= b->cap) return 0;
    size_t ncap = b->cap ? b->cap * 2 : 65536;
    while (ncap < b->len + extra) ncap *= 2;
    unsigned char *nd = realloc(b->data, ncap);
    if (!nd) return -1;
    b->data = nd;
    b->cap = ncap;
    return 0;
}

static void buf_put(Buf *b, const void *p, size_t n) {
    if (buf_reserve(b, n + 1) != 0) return;        // +1 to keep a NUL terminator
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';                        // so b->data is a valid C string
}

static void buf_puts(Buf *b, const char *s) { buf_put(b, s, strlen(s)); }

static void buf_printf(Buf *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof(tmp)) {
        buf_put(b, tmp, (size_t)n);
    } else {
        char *big = malloc((size_t)n + 1);
        if (!big) return;
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        buf_put(b, big, (size_t)n);
        free(big);
    }
}

/* ==========================================================================
 * Reading the trailer (Root / Info / Size) from the existing PDF.
 * ========================================================================== */
// Locate Root / Info references (object number and generation) and the current
// highest object number. Works for both classic trailer<<...>> and
// cross-reference-stream files.
static int read_trailer_info(FILE *f, long startxref, int *root, int *root_gen,
                             int *info, int *info_gen, int *size) {
    *root = 0; *root_gen = 0; *info = 0; *info_gen = 0; *size = 0;

    fseek(f, 0, SEEK_END);
    long file_end = ftell(f);
    if (startxref < 0 || startxref >= file_end) return -1;

    long span = file_end - startxref;
    char *buf = malloc((size_t)span + 1);
    if (!buf) return -1;
    fseek(f, startxref, SEEK_SET);
    size_t got = fread(buf, 1, (size_t)span, f);
    buf[got] = '\0';

    const char *rp = strstr(buf, "/Root");
    if (rp) parse_indirect_ref(rp + 5, root, root_gen);
    const char *ip = strstr(buf, "/Info");
    if (ip) parse_indirect_ref(ip + 5, info, info_gen);
    const char *sp = strstr(buf, "/Size");
    if (sp) *size = parse_ref_num(sp + 5);

    free(buf);
    return (*root > 0) ? 0 : -1;
}

/* ==========================================================================
 * Field matching (fill side).
 * ========================================================================== */

// Match an FDF field name against the field map. Prefer an exact qualified-name
// match; otherwise fall back to matching the terminal (last '.'-separated)
// component so callers can supply either form.
static FieldLoc *match_field(FieldMap *map, const char *name) {
    for (int i = 0; i < map->count; i++)
        if (strcmp(map->items[i].qname, name) == 0) return &map->items[i];

    for (int i = 0; i < map->count; i++) {
        const char *dot = strrchr(map->items[i].qname, '.');
        const char *leaf = dot ? dot + 1 : map->items[i].qname;
        if (strcmp(leaf, name) == 0) return &map->items[i];
    }
    return NULL;
}

/* ==========================================================================
 * Building updated object bodies.
 * ========================================================================== */
static void append_escaped_string(Buf *b, const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p == '(' || *p == ')' || *p == '\\') buf_put(b, "\\", 1);
        buf_put(b, p, 1);
    }
}

// Decode one UTF-8 code point from *s (NUL-terminated) and advance *s past it.
// On a malformed lead/continuation byte, consume a single byte and return it
// unchanged (U+0000..U+00FF), so no input is lost on non-UTF-8 or truncated data.
static unsigned utf8_next(const unsigned char **s) {
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

// Write `s` (assumed UTF-8) as a PDF text-string value. Pure-ASCII values are
// emitted as a readable literal string; any value with a byte >= 0x80 is emitted
// as a UTF-16BE hex string with a leading BOM (U+FEFF) -- the only encoding PDF
// text strings define for characters outside PDFDocEncoding.
static void append_text_value(Buf *out, const char *s) {
    int high = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p >= 0x80) { high = 1; break; }
    if (!high) {
        buf_puts(out, "(");
        append_escaped_string(out, s);
        buf_puts(out, ")");
        return;
    }
    buf_puts(out, "<FEFF");
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned cp = utf8_next(&p);
        if (cp > 0xFFFF) {                        // astral plane: surrogate pair
            cp -= 0x10000;
            buf_printf(out, "%04X%04X", 0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF));
        } else {
            buf_printf(out, "%04X", cp);
        }
    }
    buf_puts(out, ">");
}

/* ==========================================================================
 * Object emit helpers (encrypt appended objects when the document is encrypted).
 *
 * Objects were decrypted on read, so their re-emitted strings/streams are
 * plaintext. In a still-encrypted document every appended string and stream must
 * be re-encrypted for its (num,gen); cross-reference streams stay plaintext.
 * ========================================================================== */

// Append "num gen obj\n<<inner>>\nendobj\n", encrypting the dict strings for
// (num,gen) when `crypt` is set.
static void emit_dict_object(Buf *o, const PdfCrypt *crypt, int num, int gen, const char *inner) {
    char *enc = crypt ? pdf_encrypt_dict_strings(crypt, num, gen, inner) : NULL;
    buf_printf(o, "%d %d obj\n<<", num, gen);
    buf_puts(o, enc ? enc : inner);
    buf_puts(o, ">>\nendobj\n");
    free(enc);
}

// Append a stream object. `dict_inner` is the dict text WITHOUT /Length and
// WITHOUT the closing ">>"; /Length is written to match the emitted bytes. The
// stream is encrypted for (num,gen) when `crypt` is set.
static void emit_stream_object(Buf *o, const PdfCrypt *crypt, int num, int gen,
                               const char *dict_inner, const unsigned char *sdata, size_t slen) {
    unsigned char *enc_s = NULL;
    size_t elen = slen;
    if (crypt) {
        enc_s = malloc(slen + 64);
        int r = enc_s ? pdf_encrypt(crypt, num, gen, sdata, slen, enc_s, slen + 64) : -1;
        if (r >= 0) elen = (size_t)r; else { free(enc_s); enc_s = NULL; }
    }
    char *enc_d = crypt ? pdf_encrypt_dict_strings(crypt, num, gen, dict_inner) : NULL;
    buf_printf(o, "%d %d obj\n<<", num, gen);
    buf_puts(o, enc_d ? enc_d : dict_inner);
    buf_printf(o, "/Length %zu>>\nstream\n", elen);
    buf_put(o, enc_s ? (const char *)enc_s : (const char *)sdata, elen);
    buf_puts(o, "\nendstream\nendobj\n");
    free(enc_s);
    free(enc_d);
}

/* ==========================================================================
 * Text-field appearance streams.
 *
 * /NeedAppearances tells a viewer to (re)build field appearances itself, but
 * many renderers (Chrome/Firefox built-ins, poppler rasterizers, print
 * pipelines) do not honor it -- they draw the field's existing /AP stream and
 * nothing else. To make filled text visible everywhere, we also generate an
 * appearance: a Form XObject that draws the value with the field's /DA font,
 * and point the widget's /AP /N at it.
 *
 * Scope: single-line text fields whose value is representable in WinAnsi (ASCII
 * + Latin-1), left-aligned, using a standard-14 font. Values with characters
 * outside WinAnsi (CJK, emoji) or fields without a usable /Rect are skipped and
 * fall back to /NeedAppearances, which stays set.
 * ========================================================================== */

// Case-insensitive substring search, for classifying full DA font names.
static int name_has(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}

// Map a /DA font resource name to a standard-14 BaseFont. Handles the Adobe
// abbreviations (Helv, HeBo, ...) and full names carrying a Bold/Italic style
// (e.g. HelveticaLTStd-Bold); defaults to Helvetica.
static const char *base_font_for(const char *name) {
    int bold = name_has(name, "bo") || name_has(name, "black") || name_has(name, "heavy");
    int ital = name_has(name, "it") || name_has(name, "ob");   // italic / oblique
    if (name_has(name, "cour") || !strncmp(name, "Co", 2))
        return bold ? "Courier-Bold" : "Courier";
    if (name_has(name, "times") || name_has(name, "tiro") || !strncmp(name, "Ti", 2))
        return bold ? "Times-Bold" : (ital ? "Times-Italic" : "Times-Roman");
    if (bold && ital) return "Helvetica-BoldOblique";
    if (bold) return "Helvetica-Bold";
    if (ital) return "Helvetica-Oblique";
    return "Helvetica";
}

// Encode `s` (UTF-8) as an escaped WinAnsi PDF-string body into `out`. Returns 1
// if every character is WinAnsi-representable, 0 otherwise (caller skips the AP).
// WinAnsi and Latin-1 agree on 0x20..0x7E and 0xA0..0xFF; 0x80..0x9F and code
// points above 0xFF are treated as not representable.
static int winansi_escape(const char *s, Buf *out) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned cp = utf8_next(&p);
        if (!((cp >= 0x20 && cp <= 0x7E) || (cp >= 0xA0 && cp <= 0xFF))) return 0;
        if (cp == '(' || cp == ')' || cp == '\\') buf_put(out, "\\", 1);
        unsigned char b = (unsigned char)cp;
        buf_put(out, &b, 1);
    }
    return 1;
}

// Parse font name, size and trailing color operators from a /DA string such as
// "/Helv 10 Tf 0 g". Missing pieces default to Helv / 0 (auto) / "0 g".
static void parse_da(const char *da, char *font, size_t fn, double *size,
                     char *color, size_t cn) {
    snprintf(font, fn, "Helv");
    snprintf(color, cn, "0 g");
    *size = 0;
    if (!da) return;
    const char *tf = strstr(da, "Tf");
    if (!tf) return;

    // Font name and size are the "/Name size" immediately before "Tf".
    const char *nm = tf;
    while (nm > da && *nm != '/') nm--;
    if (*nm == '/') {
        nm++;
        size_t k = 0;
        while (nm < tf && !isspace((unsigned char)*nm) && k < fn - 1) font[k++] = *nm++;
        font[k] = '\0';
        *size = atof(nm);                        // atof skips the leading space
        if (*size < 0) *size = 0;
    }

    // Color operators are whatever follows "Tf" (e.g. "0 g", "0 0 1 rg").
    const char *c = tf + 2;
    while (*c == ' ' || *c == '\n' || *c == '\r' || *c == '\t') c++;
    size_t k = 0;
    while (*c && k < cn - 1) color[k++] = *c++;
    while (k > 0 && isspace((unsigned char)color[k - 1])) k--;
    color[k] = '\0';
    if (k == 0) snprintf(color, cn, "0 g");
}

// Parse "/Rect [x0 y0 x1 y1]" from `dict` into width/height. Returns 1 if the
// rectangle is present and large enough to draw into.
static int rect_wh(const char *dict, double *w, double *h) {
    const char *r = find_key(dict, "/Rect");
    if (!r || *r != '[') return 0;
    double x0, y0, x1, y1;
    if (sscanf(r + 1, "%lf %lf %lf %lf", &x0, &y0, &x1, &y1) != 4) return 0;
    *w = x1 > x0 ? x1 - x0 : x0 - x1;
    *h = y1 > y0 ? y1 - y0 : y0 - y1;
    return (*w > 1.0 && *h > 1.0);
}

// Emit a Form XObject (object `ap_obj`) drawing `text` (already WinAnsi-escaped)
// into a `w` x `h` box using the given DA font/size/color. `da_size` <= 0 means
// auto-size from the box height. The stream is encrypted when `crypt` is set.
static void append_appearance_stream(Buf *out, const PdfCrypt *crypt, int ap_obj,
                                     double w, double h, const char *da_font,
                                     double da_size, const char *da_color, const Buf *text) {
    double fs = da_size > 0 ? da_size : h - 2.0;   // auto: fill the box height
    if (fs > 12.0 && da_size <= 0) fs = 12.0;      // but cap auto-size
    if (fs < 4.0) fs = 4.0;
    double ty = (h - fs) / 2.0 + fs * 0.2;         // vertical center + descent
    if (ty < 2.0) ty = 2.0;

    Buf cs = {0};                                   // content stream
    buf_puts(&cs, "/Tx BMC\nq\nBT\n");
    buf_printf(&cs, "/%s %.2f Tf\n", da_font, fs);
    buf_printf(&cs, "%s\n", da_color);
    buf_printf(&cs, "2 %.2f Td\n", ty);
    buf_put(&cs, "(", 1);
    buf_put(&cs, text->data, text->len);
    buf_puts(&cs, ") Tj\nET\nQ\nEMC\n");

    char dict[256];
    snprintf(dict, sizeof(dict),
             "/Type/XObject/Subtype/Form/FormType 1/BBox[0 0 %.2f %.2f]"
             "/Resources<</Font<</%s<</Type/Font/Subtype/Type1"
             "/BaseFont/%s/Encoding/WinAnsiEncoding>>>>>>",
             w, h, da_font, base_font_for(da_font));
    emit_stream_object(out, crypt, ap_obj, 0, dict, (const unsigned char *)cs.data, cs.len);
    free(cs.data);
}

// Enumerate a choice field's /Opt display strings into disp[] (each option's
// display text: the 2nd element of an [export display] pair, else the string).
// Returns the count (<= max), or -1 if any option is not WinAnsi-representable
// (so the caller skips the list-box appearance and relies on /NeedAppearances).
static int choice_displays(FILE *f, XRefTable *xref, const char *dict,
                           char disp[][256], int max) {
    char *opt = field_array_text(f, xref, dict, "/Opt");
    if (!opt) return 0;
    int n = 0;
    const char *p = opt;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    while (*p && n < max) {
        while (isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == ']') break;
        char d[256] = "";
        if (*p == '(') {
            p = read_literal(p, d, sizeof(d));
        } else if (*p == '[') {                     // [export display]
            char ex[256] = "";
            p++;
            while (isspace((unsigned char)*p)) p++;
            if (*p == '(') p = read_literal(p, ex, sizeof(ex));
            while (isspace((unsigned char)*p)) p++;
            if (*p == '(') p = read_literal(p, d, sizeof(d));
            else snprintf(d, sizeof(d), "%s", ex);
            while (*p && *p != ']') p++;
            if (*p == ']') p++;
        } else break;
        Buf tb = {0};                                // WinAnsi-representable?
        int ok = winansi_escape(d, &tb);
        free(tb.data);
        if (!ok) { free(opt); return -1; }
        snprintf(disp[n++], 256, "%s", d);
    }
    free(opt);
    return n;
}

// Emit a list-box appearance (object `ap_obj`): each option drawn as a row from
// the top (starting at option `top`), with the rows whose indices are in sel[]
// highlighted. Falls back to /NeedAppearances-style rendering for anything it
// can't draw (handled by the caller before choosing this path).
static void append_listbox_appearance(Buf *out, const PdfCrypt *crypt, int ap_obj,
                                      double w, double h, const char *da_font,
                                      double da_size, const char *da_color,
                                      char disp[][256], int nopt,
                                      const int *sel, int nsel, int top) {
    double fs = da_size > 0 ? da_size : 12.0;
    if (fs < 4.0) fs = 4.0;
    double lh = fs * 1.15;                            // line height

    Buf cs = {0};
    buf_puts(&cs, "/Tx BMC\nq\n");
    for (int k = 0; k < nsel; k++) {                 // highlight selected rows (behind text)
        int row = sel[k] - top;
        if (row < 0) continue;
        double ytop = h - row * lh, ybot = ytop - lh;
        if (ytop <= 0) continue;                     // fully below the box
        buf_puts(&cs, "0.6 0.756863 0.854902 rg\n");  // Adobe selection blue
        buf_printf(&cs, "0 %.2f %.2f %.2f re\nf\n", ybot, w, lh);
    }
    buf_puts(&cs, "BT\n");
    buf_printf(&cs, "/%s %.2f Tf\n", da_font, fs);
    buf_printf(&cs, "%s\n", da_color);
    for (int i = top; i < nopt; i++) {
        double baseline = h - (i - top) * lh - fs;
        if (baseline < -fs) break;                   // fully below the box
        buf_printf(&cs, "1 0 0 1 2 %.2f Tm\n", baseline);
        Buf esc = {0};
        winansi_escape(disp[i], &esc);
        buf_put(&cs, "(", 1);
        if (esc.len) buf_put(&cs, esc.data, esc.len);
        buf_puts(&cs, ") Tj\n");
        free(esc.data);
    }
    buf_puts(&cs, "ET\nQ\nEMC\n");

    char dict[256];
    snprintf(dict, sizeof(dict),
             "/Type/XObject/Subtype/Form/FormType 1/BBox[0 0 %.2f %.2f]"
             "/Resources<</Font<</%s<</Type/Font/Subtype/Type1"
             "/BaseFont/%s/Encoding/WinAnsiEncoding>>>>>>",
             w, h, da_font, base_font_for(da_font));
    emit_stream_object(out, crypt, ap_obj, 0, dict, (const unsigned char *)cs.data, cs.len);
    free(cs.data);
}

// Integer /Ff field flags (0 if absent). Bit 18 (0x20000) marks a combo box.
static int field_flags(const char *dict) {
    const char *ff = find_key(dict, "/Ff");
    return ff ? atoi(ff) : 0;
}

// Index of `value` within a choice field's /Opt array, matching either the
// export value or the display text, or -1 if /Opt is absent or has no match.
// Each /Opt entry is a string (export == display) or a two-element
// [export display] array. On a match, the option's export and display strings
// are copied out (export goes in /V; display is what a combo box renders).
static int choice_index(FILE *f, XRefTable *xref, const char *dict, const char *value,
                        char *export_out, size_t exp_cap, char *display_out, size_t disp_cap) {
    if (exp_cap) export_out[0] = '\0';
    if (disp_cap) display_out[0] = '\0';
    char *opt = field_array_text(f, xref, dict, "/Opt");
    if (!opt) return -1;
    int idx = -1, i = 0;
    const char *p = opt;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == ']') break;
        char exp[512] = "", disp[512] = "";
        if (*p == '(') {
            p = read_literal(p, exp, sizeof(exp));
            snprintf(disp, sizeof(disp), "%s", exp);
        } else if (*p == '[') {                     // [export display]
            p++;
            while (isspace((unsigned char)*p)) p++;
            if (*p == '(') p = read_literal(p, exp, sizeof(exp));
            while (isspace((unsigned char)*p)) p++;
            if (*p == '(') p = read_literal(p, disp, sizeof(disp));
            else snprintf(disp, sizeof(disp), "%s", exp);
            while (*p && *p != ']') p++;
            if (*p == ']') p++;
        } else break;                                // unexpected token
        if (strcmp(exp, value) == 0 || strcmp(disp, value) == 0) {
            idx = i;
            snprintf(export_out, exp_cap, "%s", exp);
            snprintf(display_out, disp_cap, "%s", disp);
            break;
        }
        i++;
    }
    free(opt);
    return idx;
}

// Copy the first name key that is not "/Off" from a dictionary value `d`
// (pointing at "<<...>>") into `out`. Returns 1 on success.
static int first_non_off_key(const char *d, char *out, size_t cap) {
    if (!(d[0] == '<' && d[1] == '<')) return 0;
    const char *p = d + 2;
    int depth = 1;
    while (*p && depth > 0) {
        if (*p == '(') { while (*p && *p != ')') { if (*p == '\\' && p[1]) p++; p++; } if (*p) p++; continue; }
        if (p[0] == '<' && p[1] == '<') { depth += 1; p += 2; continue; }
        if (p[0] == '>' && p[1] == '>') { depth -= 1; p += 2; continue; }
        if (*p == '/' && depth == 1) {
            const char *s = p + 1;
            char name[128];
            size_t k = 0;
            while (*s && !isspace((unsigned char)*s) && !strchr("/<>[](){}", *s) && k < sizeof(name) - 1)
                name[k++] = *s++;
            name[k] = '\0';
            if (strcmp(name, "Off") != 0) { snprintf(out, cap, "%s", name); return 1; }
            p = s;
            continue;
        }
        p++;
    }
    return 0;
}

// A checkbox's "on" appearance state: the non-/Off key of its /AP /N dictionary
// (e.g. "1", "2", "Yes"). /AP and /N may each be inline or an indirect ref.
// Returns 1 and fills `out` if found (single-widget checkboxes only; radio-group
// parents have no /AP and fall back to the caller-supplied value).
static int checkbox_on_state(FILE *f, XRefTable *xref, const char *dict, char *out, size_t cap) {
    const char *ap = find_key(dict, "/AP");
    if (!ap) return 0;
    char *apbuf = NULL;
    const char *apd = ap;
    if (!(ap[0] == '<' && ap[1] == '<')) {
        if (!isdigit((unsigned char)*ap)) return 0;
        apbuf = get_object_raw(f, xref, atoi(ap), NULL);
        apd = apbuf;
    }
    int ok = 0;
    if (apd) {
        const char *n = find_key(apd, "/N");
        char *nbuf = NULL;
        const char *nd = NULL;
        if (n && n[0] == '<' && n[1] == '<') nd = n;
        else if (n && isdigit((unsigned char)*n)) { nbuf = get_object_raw(f, xref, atoi(n), NULL); nd = nbuf; }
        if (nd) ok = first_non_off_key(nd, out, cap);
        free(nbuf);
    }
    free(apbuf);
    return ok;
}

// Resolve the appearance-state name to write for a button field. An off-like
// value maps to /Off; otherwise the widget's real on-state (so a caller value of
// "Yes"/"On"/"1" checks the box even when the on-state is named differently),
// falling back to the caller's value when no /AP on-state can be found.
static void resolve_button_state(FILE *f, XRefTable *xref, const char *dict,
                                 const char *value, char *out, size_t cap) {
    const char *offs[] = { "", "Off", "off", "OFF", "0", "false", "False", "No", "no" };
    for (size_t i = 0; i < sizeof(offs) / sizeof(offs[0]); i++)
        if (strcmp(value, offs[i]) == 0) { snprintf(out, cap, "Off"); return; }
    char on[128];
    if (checkbox_on_state(f, xref, dict, on, sizeof(on))) snprintf(out, cap, "%s", on);
    else snprintf(out, cap, "%s", value);
}

// Build the updated inner dictionary text (between << and >>) for a field
// object: original dict with a new /V. For a button, `btn_state` is the resolved
// on/off appearance-state name (also written to /AS). For a choice field, a
// single selection sets /V + /I[choice_idx]; a multi-selection (fld->nvalues > 1)
// sets /V and /I to arrays. If `ap_obj` > 0, /AP /N points at the appearance
// stream. Strings stay plaintext here; the emit step encrypts them if needed.
static int build_field_inner(Buf *inner, FILE *f, XRefTable *xref, int obj_num,
                             const char *orig_dict, const char *value, const FdfField *fld,
                             char ftype, int ap_obj, int choice_idx, const char *btn_state) {
    char *tmp = extract_dict_inner_alloc(orig_dict);   // grows to the field's size
    if (!tmp) {
        fprintf(stderr, "WARNING: field object %d dictionary is malformed; skipping\n", obj_num);
        return -1;
    }

    remove_entry(tmp, "/V");
    if (ftype == 'B') remove_entry(tmp, "/AS");
    if (ftype == 'C') remove_entry(tmp, "/I");
    if (ap_obj > 0) remove_entry(tmp, "/AP");

    // trim trailing whitespace
    size_t ilen = strlen(tmp);
    while (ilen > 0 && isspace((unsigned char)tmp[ilen - 1])) tmp[--ilen] = '\0';

    buf_puts(inner, tmp);
    free(tmp);
    if (ftype == 'B') {
        // checkbox / radio: /V and /AS are the resolved appearance-state name
        buf_printf(inner, "/V/%s/AS/%s", btn_state, btn_state);
    } else if (ftype == 'C' && fld->nvalues > 1) {
        // multi-select list box: /V is an array of the selected export values,
        // /I an ascending array of their /Opt indices.
        int idxs[64], ni = 0;
        buf_puts(inner, "/V[");
        for (int j = 0; j < fld->nvalues; j++) {
            char ex[512], dp[512];
            int idx = choice_index(f, xref, orig_dict, fld->values[j], ex, sizeof(ex), dp, sizeof(dp));
            append_text_value(inner, idx >= 0 ? ex : fld->values[j]);
            if (idx >= 0 && ni < 64) idxs[ni++] = idx;
        }
        buf_puts(inner, "]");
        for (int a = 0; a < ni; a++)                 // insertion sort (ascending)
            for (int b = a + 1; b < ni; b++)
                if (idxs[b] < idxs[a]) { int t = idxs[a]; idxs[a] = idxs[b]; idxs[b] = t; }
        if (ni > 0) {
            buf_puts(inner, "/I[");
            for (int a = 0; a < ni; a++) buf_printf(inner, "%s%d", a ? " " : "", idxs[a]);
            buf_puts(inner, "]");
        }
    } else {
        buf_puts(inner, "/V");
        append_text_value(inner, value);
        // list/combo box: record the selected option index so viewers highlight it
        if (ftype == 'C' && choice_idx >= 0) buf_printf(inner, "/I[%d]", choice_idx);
    }
    if (ap_obj > 0) buf_printf(inner, "/AP<</N %d 0 R>>", ap_obj);
    return 0;
}

/* ==========================================================================
 * Cross-reference stream writer.
 * ========================================================================== */
typedef struct { int obj_num; long offset; int gen; } XEntry;

static int xentry_cmp(const void *a, const void *b) {
    return ((const XEntry *)a)->obj_num - ((const XEntry *)b)->obj_num;
}

// Write a type-1 (uncompressed) cross-reference stream describing `entries`
// (which must include the xref stream object itself). /W is [1 4 2]. When the
// document is encrypted, `enc_obj` (>0) and `id0`/`id0_len` are carried into the
// new trailer so a reader still recognizes and decrypts the file.
static void append_xref_stream(Buf *out, XEntry *entries, int n, int size,
                               int root, int root_gen, int info, int info_gen, long prev,
                               int enc_obj, const unsigned char *id0, int id0_len) {
    qsort(entries, n, sizeof(XEntry), xentry_cmp);

    // Binary cross-reference data.
    Buf bin = {0};
    for (int i = 0; i < n; i++) {
        unsigned char rec[7];
        long off = entries[i].offset;
        int g = entries[i].gen;
        rec[0] = 1;                                  // type: in-use, uncompressed
        rec[1] = (unsigned char)((off >> 24) & 0xff);
        rec[2] = (unsigned char)((off >> 16) & 0xff);
        rec[3] = (unsigned char)((off >> 8) & 0xff);
        rec[4] = (unsigned char)(off & 0xff);
        rec[5] = (unsigned char)((g >> 8) & 0xff);
        rec[6] = (unsigned char)(g & 0xff);
        buf_put(&bin, rec, 7);
    }

    int xref_obj = entries[n - 1].obj_num;   // xref stream is the highest obj num
    // (offset for this object was recorded by the caller before this write)

    buf_printf(out, "%d 0 obj\n<</Type/XRef/Size %d/Root %d %d R", xref_obj, size, root, root_gen);
    if (info > 0) buf_printf(out, "/Info %d %d R", info, info_gen);
    if (prev >= 0) buf_printf(out, "/Prev %ld", prev);
    if (enc_obj > 0) {
        buf_printf(out, "/Encrypt %d 0 R/ID[<", enc_obj);
        for (int i = 0; i < id0_len; i++) buf_printf(out, "%02x", id0[i]);
        buf_puts(out, "><");
        for (int i = 0; i < id0_len; i++) buf_printf(out, "%02x", id0[i]);
        buf_puts(out, ">]");
    }
    buf_puts(out, "/W[1 4 2]/Index[");
    for (int i = 0; i < n; i++)
        buf_printf(out, "%s%d 1", i ? " " : "", entries[i].obj_num);
    buf_printf(out, "]/Length %zu>>\nstream\n", bin.len);
    buf_put(out, bin.data, bin.len);
    buf_puts(out, "\nendstream\nendobj\n");
    free(bin.data);
}

/* ==========================================================================
 * FDF parsing (tolerant of spacing variations).
 * ========================================================================== */
static void fdf_add(FdfData *d, const char *name, const char *value) {
    if (!name) return;
    if (d->field_count >= d->capacity) {
        int ncap = d->capacity ? d->capacity * 2 : 32;
        FdfField *nf = realloc(d->fields, ncap * sizeof(FdfField));
        if (!nf) return;
        d->fields = nf;
        d->capacity = ncap;
    }
    d->fields[d->field_count].field_name = strdup(name);
    d->fields[d->field_count].field_value = strdup(value ? value : "");
    d->fields[d->field_count].values = NULL;
    d->fields[d->field_count].nvalues = 0;
    d->field_count++;
}


FdfData *parse_fdf_file(const char *fdf_filename) {
    FILE *fp = portable_fopen(fdf_filename, "rb");
    if (!fp) { fprintf(stderr, "ERROR: cannot open FDF file: %s\n", fdf_filename); return NULL; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *content = malloc((size_t)sz + 1);
    if (!content) { fclose(fp); return NULL; }
    size_t got = fread(content, 1, (size_t)sz, fp);
    content[got] = '\0';
    fclose(fp);

    FdfData *d = calloc(1, sizeof(FdfData));
    if (!d) { free(content); return NULL; }

    // Walk field dictionaries: each has /T (name) and optionally /V (value).
    // We scan for "/T" occurrences and read the paired "/V" that follows within
    // the same dictionary entry.
    const char *p = content;
    while ((p = strstr(p, "/T")) != NULL) {
        const char *v = p + 2;
        if (isalnum((unsigned char)*v)) { p = v; continue; }   // e.g. /Type
        while (*v == ' ' || *v == '\n' || *v == '\r' || *v == '\t') v++;
        if (*v != '(') { p = v; continue; }
        char name[256];
        const char *after = read_literal(v, name, sizeof(name));

        // Find the value: the next "/V" before the next "/T". `value` is heap-
        // allocated (unbounded) so long text values are not silently truncated.
        char *value = NULL;
        char *marray[64];
        int mcount = 0;
        const char *next_t = strstr(after, "/T");
        const char *vp = strstr(after, "/V");
        if (vp && (!next_t || vp < next_t)) {
            const char *w = vp + 2;
            while (*w == ' ' || *w == '\n' || *w == '\r' || *w == '\t') w++;
            if (*w == '(') read_literal_alloc(w, &value);
            else if (*w == '[') {   // multi-select choice: array of literal strings
                const char *q = w + 1;
                while (*q && *q != ']' && mcount < 64) {
                    while (isspace((unsigned char)*q)) q++;
                    if (*q == '(') q = read_literal_alloc(q, &marray[mcount++]);
                    else if (*q == ']') break;
                    else q++;
                }
                if (mcount > 0 && marray[0]) value = strdup(marray[0]);   // first element for compat
            }
            else if (*w == '/') {   // name value (checkbox state): always short
                char st[256]; size_t k = 0; w++;
                while (*w && !isspace((unsigned char)*w) && !strchr("()<>[]{}/%", *w) && k < sizeof(st) - 1)
                    st[k++] = *w++;
                st[k] = '\0';
                value = strdup(st);
            }
        }
        fdf_add(d, name, value ? value : "");
        free(value);
        if (mcount > 1 && d->field_count > 0) {         // attach the multi-select array
            FdfField *fl = &d->fields[d->field_count - 1];
            fl->values = malloc((size_t)mcount * sizeof(char *));
            if (fl->values) { for (int j = 0; j < mcount; j++) fl->values[j] = marray[j]; fl->nvalues = mcount; }
        } else {
            for (int j = 0; j < mcount; j++) free(marray[j]);   // single (or none): not kept
        }
        p = after;
    }

    free(content);
    return d;
}

void free_fdf_data(FdfData *d) {
    if (!d) return;
    for (int i = 0; i < d->field_count; i++) {
        free(d->fields[i].field_name);
        free(d->fields[i].field_value);
        for (int j = 0; j < d->fields[i].nvalues; j++) free(d->fields[i].values[j]);
        free(d->fields[i].values);
    }
    free(d->fields);
    free(d);
}

/* ==========================================================================
 * XFA datasets sync.
 *
 * Dynamic XFA forms (Adobe LiveCycle) are rendered from an XML "datasets"
 * packet, not the AcroForm, so filling only AcroForm /V values is invisible in
 * Adobe. The datasets packet holds one element per field, named by the field's
 * leaf name without the [n] occurrence index (e.g. <f1_1/> or <c1_1>0</c1_1>).
 * We locate it via the AcroForm /XFA entry, set each filled text field's
 * element, and rewrite the containing stream object.
 *
 * /XFA is either an array [(name) ref (name) ref ...] whose (datasets) entry is
 * the datasets XML, or a single stream reference whose object holds the whole
 * XDP document with the datasets packet embedded. xfa_datasets_set() locates
 * <xfa:data> within whatever XML it is given, so both work the same way.
 *
 * Checkboxes/radios are synced too: XFA encodes them as the selected value --
 * "0" when off, otherwise the AcroForm on-state name, which LiveCycle keeps
 * equal to the XFA data value (e.g. "1"/"2"). Signature fields carry no data
 * value and are skipped.
 * ========================================================================== */

// The object number of the XFA stream to edit (the (datasets) packet for an
// array /XFA, or the whole XDP stream for a single-reference /XFA), or 0.
static int find_xfa_datasets(const char *afd) {
    const char *xfa = find_key(afd, "/XFA");
    if (!xfa) return 0;
    if (*xfa == '[') {                          // array form: (datasets) N 0 R
        const char *ds = strstr(xfa, "(datasets)");
        if (!ds) return 0;
        ds += 10;
        while (*ds == ' ' || *ds == '\t' || *ds == '\n' || *ds == '\r') ds++;
        return atoi(ds);
    }
    if (isdigit((unsigned char)*xfa))           // single stream: /XFA N 0 R
        return atoi(xfa);
    return 0;
}

// Load the datasets packet's XML (decompressed, NUL-terminated). The datasets
// object is a stream, so it always has a direct byte offset: stream objects
// cannot be stored inside an object stream (ISO 32000-1 7.5.7), so a negative
// offset means the object is absent/malformed, not compressed. malloc'd or NULL.
static char *load_datasets_xml(FILE *f, XRefTable *xref, int obj) {
    long off = xref_lookup(xref, obj);
    if (off < 0) return NULL;                   // absent/malformed object
    PdfObject o = parse_obj_at_offset(f, off, pdf_doc_crypt());
    char *xml = NULL;
    if (o.stream && o.stream_len > 0) {
        // decompress_stream handles Flate, LZW, and raw (no-/Filter) streams
        // and applies the decompression caps, so the resulting size is always
        // bounded. Streams with an unsupported filter yield NULL (they could
        // never parse as XML anyway).
        size_t dl;
        char *dec = decompress_stream(o.dictionary, o.stream, o.stream_len, &dl);
        if (dec) {
            xml = malloc(dl + 1);
            if (xml) { memcpy(xml, dec, dl); xml[dl] = '\0'; }
            free(dec);
        }
    }
    free(o.stream);
    return xml;
}


/* ==========================================================================
 * Top-level fill.
 *
 * The incremental update is assembled in `FillCtx`: appended object bytes grow
 * in `o`, and every overridden/created object records an `entries` row so the
 * new cross-reference stream can point at it. The per-field, AcroForm and XFA
 * emitters below all append through this shared context.
 * ========================================================================== */
// One appearance to bake into page content when flattening: the widget it
// belongs to, the appearance XObject to draw, its rectangle, and whether it is
// a tool-generated 0-origin appearance (drawn with a plain translation).
typedef struct {
    int widget_obj;
    int ap_ref;
    double rect[4];             // [x0 y0 x1 y1], lower-left normalized
    int generated;
} Stamp;

typedef struct {
    FILE *f;
    XRefTable *xref;
    const PdfCrypt *crypt;      // non-NULL => re-encrypt appended strings/streams
    Buf o;                      // appended bytes (after the verbatim original)
    long base_len;              // file length the appended objects start at
    int next_obj;               // next free object number for new objects
    XEntry *entries;            // xref rows for appended/overridden objects
    int nentries, entries_cap;
    int updated_fields;
    char af_da[256];            // AcroForm default /DA (inherited by fields)
    int flatten;                // -flatten: bake appearances, remove the form
    Stamp *stamps;              // appearances to stamp (when flattening)
    int nstamps, stamps_cap;
} FillCtx;

// Parse "/Rect [x0 y0 x1 y1]" into rect[4], normalizing to lower-left origin.
static int parse_rect4(const char *dict, double rect[4]) {
    const char *r = find_key(dict, "/Rect");
    if (!r || *r != '[') return 0;
    r++;
    for (int i = 0; i < 4; i++) {
        char *end;
        rect[i] = strtod(r, &end);
        if (end == r) return 0;
        r = end;
    }
    if (rect[2] < rect[0]) { double t = rect[0]; rect[0] = rect[2]; rect[2] = t; }
    if (rect[3] < rect[1]) { double t = rect[1]; rect[1] = rect[3]; rect[3] = t; }
    return 1;
}

static void record_stamp(FillCtx *c, int widget_obj, int ap_ref, const double rect[4], int gen) {
    if (ap_ref <= 0) return;
    if (c->nstamps >= c->stamps_cap) {
        int nc = c->stamps_cap ? c->stamps_cap * 2 : 32;
        Stamp *ns = realloc(c->stamps, (size_t)nc * sizeof(Stamp));
        if (!ns) return;
        c->stamps = ns; c->stamps_cap = nc;
    }
    Stamp *s = &c->stamps[c->nstamps++];
    s->widget_obj = widget_obj; s->ap_ref = ap_ref; s->generated = gen;
    for (int i = 0; i < 4; i++) s->rect[i] = rect[i];
}

// Record an xref row for an appended object (silently drops if over capacity,
// which cannot happen given entries_cap = 2*field_count + slack).
static void fill_record(FillCtx *c, int obj_num, long offset, int gen) {
    if (c->nentries >= c->entries_cap) {          // grow (flatten adds page/content objects)
        int nc = c->entries_cap ? c->entries_cap * 2 : 16;
        XEntry *ne = realloc(c->entries, (size_t)nc * sizeof(XEntry));
        if (!ne) return;                          // drop the row rather than overflow
        c->entries = ne; c->entries_cap = nc;
    }
    c->entries[c->nentries].obj_num = obj_num;
    c->entries[c->nentries].offset = offset;
    c->entries[c->nentries].gen = gen;
    c->nentries++;
}

// Object number of a checkbox/radio's appearance for state `state` (the ref at
// /AP /N /<state>). /AP and /N may each be an inline dict or an indirect ref.
static int button_on_state_ref(FILE *f, XRefTable *xref, const char *fodict, const char *state) {
    const char *ap = find_key(fodict, "/AP");
    char *apd = NULL; const char *apbody = NULL;
    if (ap && *ap == '<') apbody = ap;
    else if (ap && isdigit((unsigned char)*ap)) { apd = get_object_raw(f, xref, atoi(ap), NULL); apbody = apd; }
    int ref = 0;
    if (apbody) {
        char *api = extract_dict_inner_alloc(apbody);
        const char *nn = api ? find_key(api, "/N") : NULL;
        char *nd = NULL; const char *nbody = NULL;
        if (nn && *nn == '<') nbody = nn;
        else if (nn && isdigit((unsigned char)*nn)) { nd = get_object_raw(f, xref, atoi(nn), NULL); nbody = nd; }
        if (nbody) {
            char *ni = extract_dict_inner_alloc(nbody);
            char key[130]; snprintf(key, sizeof key, "/%s", state);
            const char *sv = ni ? find_key(ni, key) : NULL;
            if (sv && isdigit((unsigned char)*sv)) ref = atoi(sv);
            free(ni);
        }
        free(nd); free(api);
    }
    free(apd);
    return ref;
}

// Emit the appearance stream for a field being filled, choosing the list-box or
// single-line variant. `fodict` is the field's dict; the DA is inherited from
// the AcroForm when the field has none.
static void emit_field_appearance(FillCtx *c, const char *fodict, int ap_obj,
                                  double aw, double ah, const Buf *aptext,
                                  char (*ldisp)[256], int lnopt, const int *lsel, int lnsel, int ltop) {
    char da_font[64], da_color[128], da[256];
    double da_size;
    da[0] = '\0';
    const char *d = find_key(fodict, "/DA");
    if (d && *d == '(') read_literal(d, da, sizeof(da));
    parse_da(da[0] ? da : c->af_da, da_font, sizeof(da_font), &da_size, da_color, sizeof(da_color));
    long apoff = c->base_len + (long)c->o.len;
    if (ldisp)
        append_listbox_appearance(&c->o, c->crypt, ap_obj, aw, ah, da_font, da_size, da_color,
                                  ldisp, lnopt, lsel, lnsel, ltop);
    else
        append_appearance_stream(&c->o, c->crypt, ap_obj, aw, ah, da_font, da_size, da_color, aptext);
    fill_record(c, ap_obj, apoff, 0);
}

// Emit the updated object (and any appearance stream) for FDF field `i`, which
// maps to `loc`. Skips signature fields and no-ops on unresolved dictionaries.
static void emit_filled_field(FillCtx *c, FieldMap *map, FdfData *fdf, int i, FieldLoc *loc) {
    const char *value = fdf->fields[i].field_value;

    if (loc->ftype == 'S') {   // signature: a cryptographic /V dict, not fillable
        fprintf(stderr, "WARNING: skipping signature field '%s'\n", loc->qname);
        return;
    }
    (void)map;

    char *fodict = get_object_raw(c->f, c->xref, loc->obj_num, NULL);

    // Choice: selected option index (for /I), its export value (/V) vs display
    // text (appearance), and whether it is a combo box (drawn like a text field).
    int choice_idx = -1, combo = 0;
    char cexport[512] = "", cdisplay[512] = "";
    if (fodict && loc->ftype == 'C') {
        choice_idx = choice_index(c->f, c->xref, fodict, value,
                                  cexport, sizeof(cexport), cdisplay, sizeof(cdisplay));
        combo = (field_flags(fodict) & 0x20000) != 0;
    }

    char btn_state[128] = "";   // button: resolved on/off appearance-state name
    if (fodict && loc->ftype == 'B')
        resolve_button_state(c->f, c->xref, fodict, value, btn_state, sizeof(btn_state));

    int matched = (loc->ftype == 'C' && choice_idx >= 0);
    const char *vval = matched ? cexport : value;
    const char *aval = matched ? cdisplay : value;

    // Text or combo box: single-line appearance if the value is WinAnsi and the
    // widget has a usable /Rect. Otherwise fall back to /NeedAppearances.
    int ap_obj = 0;
    double aw = 0, ah = 0;
    Buf aptext = {0};
    if (fodict && (loc->ftype == 'T' || (loc->ftype == 'C' && combo)) && aval && *aval &&
        rect_wh(fodict, &aw, &ah) && winansi_escape(aval, &aptext)) {
        ap_obj = c->next_obj++;
    }

    // List box: a multi-row appearance drawing every WinAnsi option, selected
    // rows highlighted.
    char (*ldisp)[256] = NULL;
    int lnopt = 0, lnsel = 0, ltop = 0, lsel[64];
    if (fodict && loc->ftype == 'C' && !combo && ap_obj == 0 && *value &&
        rect_wh(fodict, &aw, &ah)) {
        ldisp = malloc(128 * 256);
        if (ldisp) {
            lnopt = choice_displays(c->f, c->xref, fodict, ldisp, 128);
            if (lnopt > 0) {
                if (fdf->fields[i].nvalues > 1) {         // multi-select
                    for (int j = 0; j < fdf->fields[i].nvalues && lnsel < 64; j++) {
                        char ex[512], dp[512];
                        int idx = choice_index(c->f, c->xref, fodict, fdf->fields[i].values[j],
                                               ex, sizeof(ex), dp, sizeof(dp));
                        if (idx >= 0) lsel[lnsel++] = idx;
                    }
                } else if (choice_idx >= 0) lsel[lnsel++] = choice_idx;
                const char *ti = find_key(fodict, "/TI");   // top visible option
                ltop = ti ? atoi(ti) : 0;
                if (ltop < 0) ltop = 0;   // negative would index disp[] out of bounds
                ap_obj = c->next_obj++;
            } else { free(ldisp); ldisp = NULL; }           // no options / non-WinAnsi
        }
    }

    long off = c->base_len + (long)c->o.len;
    Buf fb = {0};
    if (fodict && build_field_inner(&fb, c->f, c->xref, loc->obj_num, fodict, vval, &fdf->fields[i],
                                    loc->ftype, ap_obj, choice_idx, btn_state) == 0) {
        emit_dict_object(&c->o, c->crypt, loc->obj_num, loc->gen_num, (const char *)fb.data);
        fill_record(c, loc->obj_num, off, loc->gen_num);
        c->updated_fields++;
        if (log_field_values())
            fprintf(stderr, "Filled '%s' (obj %d) = '%s'%s%s\n", loc->qname, loc->obj_num, value,
                    (loc->ftype == 'C' && choice_idx >= 0) ? " [+/I]" : "",
                    ap_obj ? " [+appearance]" : "");
        else
            fprintf(stderr, "Filled '%s' (obj %d) [%zu bytes]%s%s\n", loc->qname, loc->obj_num,
                    strlen(value), (loc->ftype == 'C' && choice_idx >= 0) ? " [+/I]" : "",
                    ap_obj ? " [+appearance]" : "");
        if (ap_obj > 0)
            emit_field_appearance(c, fodict, ap_obj, aw, ah, &aptext, ldisp, lnopt, lsel, lnsel, ltop);

        // When flattening, remember the appearance to bake into page content.
        if (c->flatten) {
            double rect[4];
            if (ap_obj > 0 && parse_rect4(fodict, rect))       // text / combo / list box
                record_stamp(c, loc->obj_num, ap_obj, rect, 1);
            else if (loc->ftype == 'B' && btn_state[0] && parse_rect4(fodict, rect))
                record_stamp(c, loc->obj_num,                  // checkbox/radio on-state
                             button_on_state_ref(c->f, c->xref, fodict, btn_state), rect, 0);
        }
    }
    free(fb.data);
    free(aptext.data);
    free(ldisp);
    free(fodict);
}

// Rewrite the AcroForm object with /NeedAppearances true so viewers render the
// filled values.
static void emit_acroform_needappearances(FillCtx *c, int acroform) {
    int af_gen = 0;
    char *afd = get_object_raw(c->f, c->xref, acroform, &af_gen);
    char *inner = afd ? extract_dict_inner_alloc(afd) : NULL;
    if (inner) {
        remove_entry(inner, "/NeedAppearances");
        size_t ilen = strlen(inner);
        while (ilen > 0 && isspace((unsigned char)inner[ilen - 1])) inner[--ilen] = '\0';
        long off = c->base_len + (long)c->o.len;
        Buf ab = {0};
        buf_puts(&ab, inner);
        buf_puts(&ab, "/NeedAppearances true");
        emit_dict_object(&c->o, c->crypt, acroform, af_gen, (const char *)ab.data);
        free(ab.data);
        fill_record(c, acroform, off, af_gen);
    } else {
        fprintf(stderr, "WARNING: AcroForm dictionary malformed; /NeedAppearances not set "
                        "(field values may not render until edited)\n");
    }
    free(inner);
    free(afd);
}

// Sync the XFA datasets packet (dynamic-XFA forms render from it, not the
// AcroForm) and append the rewritten datasets stream.
static void sync_xfa_datasets(FillCtx *c, FdfData *fdf, FieldMap *map, int acroform) {
    char *afd = get_object_raw(c->f, c->xref, acroform, NULL);
    int ds_obj = afd ? find_xfa_datasets(afd) : 0;
    free(afd);
    if (ds_obj <= 0 || ds_obj == acroform) return;

    char *xml = load_datasets_xml(c->f, c->xref, ds_obj);
    if (!xml) return;
    int synced = 0;
    for (int i = 0; i < fdf->field_count; i++) {
        FieldLoc *loc = match_field(map, fdf->fields[i].field_name);
        if (!loc || loc->ftype == 'S') continue;   // signatures carry no data value

        const char *val = fdf->fields[i].field_value;
        char btnval[128];
        if (loc->ftype == 'B') {
            // Checkboxes/radios render from the datasets too. Resolve the same
            // on/off decision the AcroForm layer uses, then encode it the XFA
            // way: /Off -> "0", otherwise the selected on-state name ("1"/"2"/...
            // for LiveCycle forms, which keep the AcroForm on-state equal to the
            // XFA data value).
            char *fodict = get_object_raw(c->f, c->xref, loc->obj_num, NULL);
            if (!fodict) continue;   // can't resolve on/off -> leave to AcroForm
            char state[128] = "";
            resolve_button_state(c->f, c->xref, fodict, val, state, sizeof(state));
            free(fodict);
            snprintf(btnval, sizeof(btnval), "%s", strcmp(state, "Off") == 0 ? "0" : state);
            val = btnval;
        }
        char *nx = xfa_datasets_set(xml, fdf->fields[i].field_name, val);
        free(xml);
        xml = nx;
        synced++;
    }
    // Preserve the original object's dictionary rather than assuming a type: an
    // array (datasets) packet is usually /Type/EmbeddedFile, but a single-stream
    // /XFA points at the whole XDP stream, which typically has no /Type. Strip
    // the keys we invalidate by rewriting uncompressed (we emit plain XML) and
    // by changing the length; emit_stream_object re-adds /Length.
    char *raw = get_object_raw(c->f, c->xref, ds_obj, NULL);
    char *dict = raw ? extract_dict_inner_alloc(raw) : NULL;
    free(raw);
    if (dict) {
        remove_entry(dict, "/Filter");
        remove_entry(dict, "/DecodeParms");
        remove_entry(dict, "/Length");
    }
    long off = c->base_len + (long)c->o.len;
    size_t xl = strlen(xml);
    emit_stream_object(&c->o, c->crypt, ds_obj, 0,
                       dict ? dict : "/Type/EmbeddedFile", (const unsigned char *)xml, xl);
    free(dict);
    fill_record(c, ds_obj, off, 0);
    fprintf(stderr, "XFA datasets synced (obj %d, %d field(s), %zu bytes)\n", ds_obj, synced, xl);
    free(xml);
}

/* ==========================================================================
 * Flattening (-flatten): bake filled appearances into page content and remove
 * the interactive form (widgets, AcroForm, XFA) so the result is not editable.
 * Scope: the common case -- field==widget forms with an inline page /Resources
 * and a single or array /Contents. Runs after the normal fill has generated the
 * appearance XObjects and recorded a Stamp per filled field.
 * ========================================================================== */

// Collect object numbers of "N 0 R" entries from an inline array at `p` ('[').
static void parse_ref_array(const char *p, int *out, int *n, int max) {
    *n = 0;
    if (!p || *p != '[') return;
    p++;
    while (*p && *p != ']' && *n < max) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!isdigit((unsigned char)*p)) { if (*p == ']' || !*p) break; p++; continue; }
        int num = atoi(p); while (isdigit((unsigned char)*p)) p++;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        while (isdigit((unsigned char)*p)) p++;          // generation
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p == 'R') { out[(*n)++] = num; p++; }
    }
}

// Produce the "a b c d e f" of a `cm` that maps the appearance into its widget
// rectangle (PDF 12.5.5). Tool-generated appearances are 0-origin => translate.
static void appearance_placement(FILE *f, XRefTable *xref, const Stamp *s, char *out, size_t outsz) {
    double x0 = s->rect[0], y0 = s->rect[1];
    double rw = s->rect[2] - s->rect[0], rh = s->rect[3] - s->rect[1];
    if (s->generated) { snprintf(out, outsz, "1 0 0 1 %.4f %.4f", x0, y0); return; }
    double bb[4] = {0, 0, rw, rh}, m[6] = {1, 0, 0, 1, 0, 0};
    char *ad = get_object_raw(f, xref, s->ap_ref, NULL);
    if (ad) {
        const char *b = find_key(ad, "/BBox");
        if (b && *b == '[') { b++; for (int i = 0; i < 4; i++) { char *e; bb[i] = strtod(b, &e); b = (e == b) ? b + 1 : e; } }
        const char *mm = find_key(ad, "/Matrix");
        if (mm && *mm == '[') { mm++; for (int i = 0; i < 6; i++) { char *e; m[i] = strtod(mm, &e); mm = (e == mm) ? mm + 1 : e; } }
        free(ad);
    }
    double cx[4] = {bb[0], bb[2], bb[2], bb[0]}, cy[4] = {bb[1], bb[1], bb[3], bb[3]};
    double tx0 = 1e30, ty0 = 1e30, tx1 = -1e30, ty1 = -1e30;
    for (int i = 0; i < 4; i++) {
        double X = m[0] * cx[i] + m[2] * cy[i] + m[4];
        double Y = m[1] * cx[i] + m[3] * cy[i] + m[5];
        if (X < tx0) tx0 = X;
        if (X > tx1) tx1 = X;
        if (Y < ty0) ty0 = Y;
        if (Y > ty1) ty1 = Y;
    }
    double dw = tx1 - tx0, dh = ty1 - ty0;
    double sx = dw != 0 ? rw / dw : 1.0, sy = dh != 0 ? rh / dh : 1.0;
    snprintf(out, outsz, "%.4f 0 0 %.4f %.4f %.4f", sx, sy, x0 - sx * tx0, y0 - sy * ty0);
}

// Appearance XObject ref for a widget from its own /AP /N (stream ref, or a
// state dict resolved via /AS). Used for widgets we did not fill.
static int widget_appearance_ref(FILE *f, XRefTable *xref, const char *wdict) {
    const char *ap = find_key(wdict, "/AP");
    char *apd = NULL; const char *apbody = NULL;
    if (ap && *ap == '<') apbody = ap;
    else if (ap && isdigit((unsigned char)*ap)) { apd = get_object_raw(f, xref, atoi(ap), NULL); apbody = apd; }
    int ref = 0;
    if (apbody) {
        char *api = extract_dict_inner_alloc(apbody);
        const char *nn = api ? find_key(api, "/N") : NULL;
        if (nn && isdigit((unsigned char)*nn)) ref = atoi(nn);       // /N N 0 R
        else if (nn && *nn == '<') {                                 // state dict -> /AS
            const char *as = find_key(wdict, "/AS"); char state[128] = "";
            if (as && *as == '/') { int k = 0; as++; while (*as && !strchr(" \t\r\n/<>[]()", *as) && k < 127) state[k++] = *as++; state[k] = '\0'; }
            if (state[0]) ref = button_on_state_ref(f, xref, wdict, state);
        }
        free(api);
    }
    free(apd);
    return ref;
}

// Gather page object numbers by walking /Pages /Kids (leaf = a node with no /Kids).
static void collect_pages(FILE *f, XRefTable *xref, int node, int *out, int *n, int max, int depth) {
    if (node <= 0 || depth > 100 || *n >= max) return;
    char *d = get_object_raw(f, xref, node, NULL);
    if (!d) return;
    const char *kids = find_key(d, "/Kids");
    if (kids && *kids == '[') {
        int k[8192], nk = 0;
        parse_ref_array(kids, k, &nk, 8192);
        for (int i = 0; i < nk; i++) collect_pages(f, xref, k[i], out, n, max, depth + 1);
    } else if (*n < max) {
        out[(*n)++] = node;
    }
    free(d);
}

// Rebuild a page's inner dict: append the stamp content stream to /Contents,
// register the stamp XObjects in /Resources /XObject, and drop the flattened
// widget refs from /Annots. `cs_obj` <= 0 => only strip /Annots. Returns malloc'd.
static char *rewrite_page_dict(const char *pd, int cs_obj, const char *xobj_entries,
                               int had_annots, const int *kept, int nkept) {
    char *inner = extract_dict_inner_alloc(pd);
    if (!inner) return NULL;

    Buf nc = {0}, nr = {0};
    if (cs_obj > 0) {
        const char *cv = find_key(inner, "/Contents");
        if (cv && *cv == '[') {
            const char *end = skip_value(cv);
            buf_put(&nc, cv, (size_t)((end - 1) - cv));
            buf_printf(&nc, " %d 0 R]", cs_obj);
        } else if (cv) {
            const char *end = skip_value(cv);
            buf_puts(&nc, "[");
            buf_put(&nc, cv, (size_t)(end - cv));
            buf_printf(&nc, " %d 0 R]", cs_obj);
        } else {
            buf_printf(&nc, "[%d 0 R]", cs_obj);
        }
        const char *rv = find_key(inner, "/Resources");
        if (rv && *rv == '<') {
            char *rin = extract_dict_inner_alloc(rv);
            const char *xo = rin ? find_key(rin, "/XObject") : NULL;
            if (xo && *xo == '<') {
                char *xin = extract_dict_inner_alloc(xo);
                char *rcopy = rin ? strdup(rin) : NULL;
                if (rcopy) remove_entry(rcopy, "/XObject");
                buf_printf(&nr, "<<%s/XObject<<%s%s>>>>", rcopy ? rcopy : "", xin ? xin : "", xobj_entries);
                free(rcopy); free(xin);
            } else {
                buf_printf(&nr, "<<%s/XObject<<%s>>>>", rin ? rin : "", xobj_entries);
            }
            free(rin);
        } else {
            buf_printf(&nr, "<</XObject<<%s>>>>", xobj_entries);
        }
    }

    char *base = strdup(inner);
    if (cs_obj > 0) { remove_entry(base, "/Contents"); remove_entry(base, "/Resources"); }
    if (had_annots) remove_entry(base, "/Annots");   // handles an inline array or an indirect ref

    Buf out = {0};
    buf_puts(&out, base);
    if (cs_obj > 0) buf_printf(&out, "/Contents%s/Resources%s",
                               nc.data ? (char *)nc.data : "", nr.data ? (char *)nr.data : "");
    if (had_annots) {                                // rewrite /Annots inline with only the kept refs
        buf_puts(&out, "/Annots[");
        for (int i = 0; i < nkept; i++) buf_printf(&out, "%d 0 R ", kept[i]);
        buf_puts(&out, "]");
    }

    char *result = out.data ? strdup((char *)out.data) : strdup(base);
    free(out.data); free(nc.data); free(nr.data); free(base); free(inner);
    return result;
}

// Stamp every widget on `page_obj` (filled ones from ctx->stamps, others from
// their own /AP) into a new content stream, then rewrite the page.
static void flatten_page(FillCtx *c, int page_obj) {
    int pgen = 0;
    char *pd = get_object_raw(c->f, c->xref, page_obj, &pgen);
    if (!pd) return;

    // /Annots may be an inline array or an indirect reference to one.
    const char *av = find_key(pd, "/Annots");
    int had_annots = (av != NULL);
    char *annots_obj = NULL;
    const char *arr = NULL;
    if (av && *av == '[') arr = av;
    else if (av && isdigit((unsigned char)*av)) {
        annots_obj = get_object_raw(c->f, c->xref, atoi(av), NULL);
        arr = annots_obj ? strchr(annots_obj, '[') : NULL;
    }
    int all[8192], nall = 0;
    if (arr) parse_ref_array(arr, all, &nall, 8192);

    Buf content = {0}, xobj = {0};
    int kept[8192], nkept = 0, drawn = 0, had_widget = 0;
    for (int i = 0; i < nall; i++) {
        int w = all[i];
        Stamp st = {0}; int have = 0, is_widget = 0;
        for (int k = 0; k < c->nstamps; k++)
            if (c->stamps[k].widget_obj == w) { st = c->stamps[k]; have = 1; is_widget = 1; break; }
        if (!have) {
            char *wd = get_object_raw(c->f, c->xref, w, NULL);
            if (wd) {
                const char *sub = find_key(wd, "/Subtype");
                is_widget = sub && !strncmp(sub, "/Widget", 7);
                double rect[4];
                int apref = widget_appearance_ref(c->f, c->xref, wd);
                if (is_widget && apref > 0 && parse_rect4(wd, rect)) {
                    st.widget_obj = w; st.ap_ref = apref; st.generated = 0;
                    memcpy(st.rect, rect, sizeof rect); have = 1;
                }
                free(wd);
            }
        }
        if (is_widget) had_widget = 1;                 // widgets are dropped (flattened)
        else if (nkept < 8192) kept[nkept++] = w;      // non-widget annots are preserved
        if (have) {
            char cm[160];
            appearance_placement(c->f, c->xref, &st, cm, sizeof cm);
            buf_printf(&content, "q %s cm /Fl%d Do Q\n", cm, drawn);
            buf_printf(&xobj, "/Fl%d %d 0 R", drawn, st.ap_ref);
            drawn++;
        }
    }
    free(annots_obj);

    // Nothing to change on this page (no stamps, no widgets to strip).
    if (drawn == 0 && !had_widget) { free(content.data); free(xobj.data); free(pd); return; }

    int cs_obj = 0;
    if (drawn > 0) {
        cs_obj = c->next_obj++;
        long csoff = c->base_len + (long)c->o.len;
        emit_stream_object(&c->o, c->crypt, cs_obj, 0, "", content.data, content.len);
        fill_record(c, cs_obj, csoff, 0);
    }
    char *newpage = rewrite_page_dict(pd, cs_obj, xobj.data ? (char *)xobj.data : "",
                                      had_annots, kept, nkept);
    if (newpage) {
        long off = c->base_len + (long)c->o.len;
        emit_dict_object(&c->o, c->crypt, page_obj, pgen, newpage);
        fill_record(c, page_obj, off, pgen);
        free(newpage);
    }
    free(content.data); free(xobj.data); free(pd);
}

// Flatten the whole document: stamp every page, then remove /AcroForm from the
// catalog (which also drops the XFA packet and /NeedAppearances).
static void flatten_form(FillCtx *c, int root_obj) {
    int rgen = 0;
    char *cat = get_object_raw(c->f, c->xref, root_obj, &rgen);
    if (!cat) return;
    const char *pp = find_key(cat, "/Pages");
    int pages_ref = (pp && isdigit((unsigned char)*pp)) ? atoi(pp) : 0;
    int pageobjs[8192], npages = 0;
    if (pages_ref > 0) collect_pages(c->f, c->xref, pages_ref, pageobjs, &npages, 8192, 0);
    for (int i = 0; i < npages; i++) flatten_page(c, pageobjs[i]);

    char *cinner = extract_dict_inner_alloc(cat);
    if (cinner) {
        remove_entry(cinner, "/AcroForm");
        remove_entry(cinner, "/NeedsRendering");
        long off = c->base_len + (long)c->o.len;
        emit_dict_object(&c->o, c->crypt, root_obj, rgen, cinner);
        fill_record(c, root_obj, off, rgen);
        free(cinner);
    }
    free(cat);
    fprintf(stderr, "Flattened %d page(s); removed the interactive form\n", npages);
}

int fill_pdf_with_fdf_ex(const char *pdf_filename, const char *fdf_filename, FILE *out, int flatten) {
    FdfData *fdf = parse_fdf_file(fdf_filename);
    if (!fdf) return 1;
    fprintf(stderr, "Parsed %d field value(s) from FDF\n", fdf->field_count);

    // All resources released together at `cleanup` (rc carries the exit code);
    // xref/map/ctx are safe to free in their zero-initialized state, so every
    // error just sets up its message and jumps there.
    FILE *f = NULL;
    XRefTable xref;
    xref_init(&xref);
    FieldMap map = {0};
    FillCtx ctx = {0};
    int rc = 1;

    f = portable_fopen(pdf_filename, "rb");
    if (!f) { fprintf(stderr, "ERROR: cannot open PDF: %s\n", pdf_filename); goto cleanup; }

    long startxref = find_startxref(f);
    if (startxref < 0 || parse_xref_table(f, startxref, &xref) <= 0) {
        fprintf(stderr, "ERROR: failed to parse xref table\n");
        goto cleanup;
    }

    int root = 0, root_gen = 0, info = 0, info_gen = 0, size = 0;
    if (read_trailer_info(f, startxref, &root, &root_gen, &info, &info_gen, &size) != 0) {
        fprintf(stderr, "ERROR: could not locate /Root in trailer\n");
        goto cleanup;
    }

    // Build the shared field map. Field objects inside compressed object streams
    // are resolved lazily by get_object_raw (only the needed streams are ever
    // decompressed); objstm_cache_reset() frees that cache at cleanup.
    int acroform = build_field_map(f, &xref, root, &map);
    if (acroform <= 0) {
        fprintf(stderr, "ERROR: no AcroForm found in document catalog\n");
        goto cleanup;
    }
    fprintf(stderr, "AcroForm is object %d; Root %d, Info %d, Size %d\n", acroform, root, info, size);
    fprintf(stderr, "Discovered %d named field node(s) in the form\n", map.count);

    // Assemble the incremental update in `ctx` (appended bytes + xref rows).
    // Capacity bound: one updated field + one appearance object per FDF field,
    // plus AcroForm, XFA datasets and the xref stream itself.
    ctx.f = f;
    ctx.xref = &xref;
    ctx.crypt = pdf_doc_crypt();
    ctx.flatten = flatten;
    ctx.entries_cap = fdf->field_count * 2 + 8;
    ctx.entries = malloc((size_t)ctx.entries_cap * sizeof(XEntry));
    if (!ctx.entries) { fprintf(stderr, "ERROR: out of memory\n"); goto cleanup; }

    // The update appends after the verbatim original; we need only its length
    // (the base offset for appended objects) and whether it ends in a newline.
    fseek(f, 0, SEEK_END);
    long orig_size = ftell(f);
    int need_nl = 0;
    if (orig_size > 0) { fseek(f, orig_size - 1, SEEK_SET); need_nl = (fgetc(f) != '\n'); }
    ctx.base_len = orig_size + (need_nl ? 1 : 0);

    // Next free object number: strictly greater than every existing object and
    // the trailer /Size hint, so new objects can never collide with a live one.
    ctx.next_obj = size > 0 ? size : 1;
    for (int i = 0; i < xref.count; i++)
        if (xref.entries[i].obj_num >= ctx.next_obj) ctx.next_obj = xref.entries[i].obj_num + 1;

    // AcroForm /DA is inherited by fields lacking their own.
    {
        char *afd = get_object_raw(f, &xref, acroform, NULL);
        const char *d = afd ? find_key(afd, "/DA") : NULL;
        if (d && *d == '(') read_literal(d, ctx.af_da, sizeof(ctx.af_da));
        free(afd);
    }

    // Encryption: carried into the new trailer; ctx.crypt re-encrypts appended objects.
    unsigned char enc_id0[64];
    int enc_obj = pdf_doc_encrypt_obj();
    int enc_id0_len = pdf_doc_id0(enc_id0, sizeof(enc_id0));

    // One updated object per matched FDF field (dedup by object number: last
    // value wins if two names map to the same object).
    for (int i = 0; i < fdf->field_count; i++) {
        FieldLoc *loc = match_field(&map, fdf->fields[i].field_name);
        if (!loc) {
            fprintf(stderr, "WARNING: field not found in PDF: '%s'\n", fdf->fields[i].field_name);
            continue;
        }
        int superseded = 0;
        for (int k = i + 1; k < fdf->field_count && !superseded; k++) {
            FieldLoc *l2 = match_field(&map, fdf->fields[k].field_name);
            if (l2 && l2->obj_num == loc->obj_num) superseded = 1;
        }
        if (superseded) continue;
        emit_filled_field(&ctx, &map, fdf, i, loc);
    }

    if (ctx.flatten) {
        flatten_form(&ctx, root);                  // bake appearances, remove the form
    } else {
        emit_acroform_needappearances(&ctx, acroform);
        sync_xfa_datasets(&ctx, fdf, &map, acroform);
    }

    if (ctx.nentries == 0) {
        fprintf(stderr, "ERROR: no fields were updated; not writing output\n");
        goto cleanup;
    }

    // The xref stream's own object number: greater than every object referenced.
    int max_obj = 0;
    for (int i = 0; i < xref.count; i++)
        if (xref.entries[i].obj_num > max_obj) max_obj = xref.entries[i].obj_num;
    for (int i = 0; i < ctx.nentries; i++)
        if (ctx.entries[i].obj_num > max_obj) max_obj = ctx.entries[i].obj_num;
    int xref_obj = max_obj + 1;
    if (size > xref_obj) xref_obj = size;         // respect trailer /Size as next-free hint
    if (xref_obj < 1) xref_obj = 1;

    long xref_off = ctx.base_len + (long)ctx.o.len;
    fill_record(&ctx, xref_obj, xref_off, 0);
    append_xref_stream(&ctx.o, ctx.entries, ctx.nentries, xref_obj + 1,
                       root, root_gen, info, info_gen, startxref,
                       enc_obj, enc_id0, enc_id0_len);
    buf_printf(&ctx.o, "startxref\n%ld\n%%%%EOF\n", xref_off);

    // Emit: original file streamed verbatim, then the appended objects. Nothing
    // is written to `out` before this point, so failures above leave no output.
    fseek(f, 0, SEEK_SET);
    {
        char cb[65536];
        size_t r;
        while ((r = fread(cb, 1, sizeof(cb), f)) > 0) fwrite(cb, 1, r, out);
    }
    if (need_nl) fputc('\n', out);
    fwrite(ctx.o.data, 1, ctx.o.len, out);
    fprintf(stderr, "Wrote filled PDF: %ld bytes, %d field(s) updated\n",
            ctx.base_len + (long)ctx.o.len, ctx.updated_fields);
    rc = 0;

cleanup:
    free(ctx.stamps);
    free(ctx.o.data);
    free(ctx.entries);
    field_map_free(&map);
    objstm_cache_reset();
    xref_free(&xref);
    if (f) fclose(f);
    free_fdf_data(fdf);
    return rc;
}

int fill_pdf_with_fdf(const char *pdf_filename, const char *fdf_filename, FILE *out) {
    return fill_pdf_with_fdf_ex(pdf_filename, fdf_filename, out, 0);
}
