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

#include "form_extractor.h"
#include "field_map.h"       // build_field_map + the shared field-tree walk
#include "pdf_lex.h"         // find_key, skip_value, read_literal_alloc
#include "pdf_filler.h"      // field_choice_options, field_checkbox_on_state
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

// Form-field extraction is a thin presentation layer over build_field_map (the
// same walk the fill command uses), so extraction and fill can never disagree
// about which fields exist. Only terminal (fillable) fields are emitted, with
// their fully-qualified names and current /V values.

// Fetch a field's raw /V value from its object. Returns the start of the value
// inside *dict_out (caller frees *dict_out), or NULL when the field has no
// inline /V (absent, or an indirect reference, which is rare and skipped).
static const char *field_value(FILE *f, XRefTable *xref, int obj_num,
                               char **dict_out) {
    *dict_out = get_object_raw(f, xref, obj_num, NULL);
    if (!*dict_out) return NULL;
    const char *v = find_key(*dict_out, "/V");
    if (!v || isdigit((unsigned char)*v)) return NULL;   // absent or "N G R" ref
    return v;
}

// Emit `name` as the body of an FDF literal string (escaping "(", ")", "\").
static void print_fdf_name(const char *name) {
    for (const char *s = name; *s; s++) {
        if (*s == '(' || *s == ')' || *s == '\\') putchar('\\');
        putchar(*s);
    }
}

// Emit `name` as an XML attribute value (escaping & < > ").
static void print_xml_attr(const char *name) {
    for (const char *s = name; *s; s++) {
        switch (*s) {
            case '&': fputs("&amp;", stdout); break;
            case '<': fputs("&lt;", stdout); break;
            case '>': fputs("&gt;", stdout); break;
            case '"': fputs("&quot;", stdout); break;
            default: putchar(*s);
        }
    }
}

void extract_form_fields_xfdf(FILE *f, const XRefTable *xref_table) {
    FieldMap map = {0};
    int acroform = build_field_map(f, (XRefTable *)xref_table, 0, &map);

    printf("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    printf("<xfdf xmlns=\"http://ns.adobe.com/xfdf/\" xml:space=\"preserve\">\n");
    printf("  <fields>\n");
    for (int i = 0; i < map.count; i++) {
        if (!map.items[i].terminal) continue;      // only fillable leaf fields
        printf("    <field name=\"");
        print_xml_attr(map.items[i].qname);
        printf("\">\n");
        // Values XFDF can carry as plain text: literal strings (unescaped) and
        // names (without the slash). Hex/UTF-16 strings and arrays are beyond
        // this simple exporter and emit an empty <value>.
        char *dict = NULL;
        const char *v = field_value(f, (XRefTable *)xref_table,
                                    map.items[i].obj_num, &dict);
        printf("      <value>");
        if (v && *v == '(') {
            char *plain = NULL;
            read_literal_alloc(v, &plain);
            if (plain) { print_xml_attr(plain); free(plain); }
        } else if (v && *v == '/') {
            char name[128]; int n = 0;
            for (v++; *v && !isspace((unsigned char)*v) && *v != '/' &&
                      *v != '>' && *v != '(' && n < 127; v++) name[n++] = *v;
            name[n] = '\0';
            print_xml_attr(name);
        }
        printf("</value>\n    </field>\n");
        free(dict);
    }
    if (acroform <= 0)
        printf("    <!-- No AcroForm detected in this PDF -->\n");
    printf("  </fields>\n</xfdf>\n");

    field_map_free(&map);
    objstm_cache_reset();
}

void extract_form_fields_fdf(FILE *f, const XRefTable *xref_table) {
    FieldMap map = {0};
    build_field_map(f, (XRefTable *)xref_table, 0, &map);

    printf("%%FDF-1.2\n1 0 obj\n<<\n/FDF\n<<\n/Fields [\n");
    for (int i = 0; i < map.count; i++) {
        if (!map.items[i].terminal) continue;      // only fillable leaf fields
        printf("<<\n/T (");
        print_fdf_name(map.items[i].qname);
        printf(")\n");
        // The raw /V is already valid FDF syntax whatever its type: literal or
        // hex string, name (/Yes), or array of strings. Copy it verbatim.
        char *dict = NULL;
        const char *v = field_value(f, (XRefTable *)xref_table,
                                    map.items[i].obj_num, &dict);
        if (v) {
            const char *e = skip_value(v);
            printf("/V ");
            fwrite(v, 1, (size_t)(e - v), stdout);
            printf("\n>>\n");
        } else {
            printf("/V ()\n>>\n");
        }
        free(dict);
    }
    printf("]\n>>\n>>\nendobj\ntrailer\n<<\n/Root 1 0 R\n>>\n%%%%EOF\n");

    field_map_free(&map);
    objstm_cache_reset();
}

/* ==========================================================================
 * `fields`: JSON field listing for programmatic callers (scripts, AI agents).
 * ========================================================================== */

// Append a UTF-8 encoding of `cp` to stdout via json_put_escaped's caller.
static void put_utf8(unsigned long cp) {
    if (cp < 0x80) putchar((int)cp);
    else if (cp < 0x800) {
        putchar(0xC0 | (int)(cp >> 6));
        putchar(0x80 | (int)(cp & 0x3F));
    } else if (cp < 0x10000) {
        putchar(0xE0 | (int)(cp >> 12));
        putchar(0x80 | (int)((cp >> 6) & 0x3F));
        putchar(0x80 | (int)(cp & 0x3F));
    } else {
        putchar(0xF0 | (int)(cp >> 18));
        putchar(0x80 | (int)((cp >> 12) & 0x3F));
        putchar(0x80 | (int)((cp >> 6) & 0x3F));
        putchar(0x80 | (int)(cp & 0x3F));
    }
}

// Print `len` bytes as the body of a JSON string (no quotes), escaping the
// characters JSON requires. Bytes are emitted verbatim (the callers hand us
// UTF-8 or PDFDoc/ASCII text).
static void json_body(const unsigned char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = s[i];
        switch (c) {
            case '"':  fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\n': fputs("\\n", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            default:
                if (c < 0x20) printf("\\u%04x", c);
                else putchar(c);
        }
    }
}

// Print a PDF text string (raw bytes from a literal or hex string) as a JSON
// string, quotes included. UTF-16BE text (leading FE FF BOM, the PDF encoding
// for non-ASCII values) is converted to UTF-8, surrogate pairs included.
static void json_pdf_text(const unsigned char *s, size_t len) {
    putchar('"');
    if (len >= 2 && s[0] == 0xFE && s[1] == 0xFF) {
        for (size_t i = 2; i + 1 < len; i += 2) {
            unsigned long cp = ((unsigned long)s[i] << 8) | s[i + 1];
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < len) {   // surrogate pair
                unsigned long lo = ((unsigned long)s[i + 2] << 8) | s[i + 3];
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            if (cp == '"' || cp == '\\' || cp < 0x20) {
                char tmp[2] = { (char)cp, 0 };
                json_body((const unsigned char *)tmp, 1);
            } else put_utf8(cp);
        }
    } else {
        json_body(s, len);
    }
    putchar('"');
}

// Decode a hex string starting at '<' into malloc'd bytes; returns length.
static size_t hex_string_bytes(const char *p, unsigned char **out) {
    size_t n = 0, cap = 64;
    unsigned char *buf = malloc(cap);
    int hi = -1;
    for (p++; buf && *p && *p != '>'; p++) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
        else continue;
        if (hi < 0) { hi = d; continue; }
        if (n == cap) { cap *= 2; unsigned char *g = realloc(buf, cap); if (!g) { free(buf); buf = NULL; break; } buf = g; }
        buf[n++] = (unsigned char)((hi << 4) | d);
        hi = -1;
    }
    if (buf && hi >= 0 && n < cap) buf[n++] = (unsigned char)(hi << 4);  // odd digit: pad
    *out = buf;
    return buf ? n : 0;
}

// Print one /V value (literal string, hex string, name, or array of strings)
// as JSON. Emits null for absent values and indirect references.
static void json_value(const char *v) {
    if (!v) { fputs("null", stdout); return; }
    if (*v == '(') {
        char *plain = NULL;
        read_literal_alloc(v, &plain);
        if (!plain) { fputs("null", stdout); return; }
        json_pdf_text((const unsigned char *)plain, strlen(plain));
        free(plain);
    } else if (*v == '<' && v[1] != '<') {
        unsigned char *bytes = NULL;
        size_t n = hex_string_bytes(v, &bytes);
        if (!bytes) { fputs("null", stdout); return; }
        json_pdf_text(bytes, n);
        free(bytes);
    } else if (*v == '/') {
        v++;
        size_t n = 0;
        while (v[n] && !isspace((unsigned char)v[n]) && v[n] != '/' &&
               v[n] != '>' && v[n] != '(' && v[n] != '[' && v[n] != '<') n++;
        putchar('"');
        json_body((const unsigned char *)v, n);
        putchar('"');
    } else if (*v == '[') {
        putchar('[');
        int first = 1;
        for (v++; *v && *v != ']'; ) {
            if (isspace((unsigned char)*v)) { v++; continue; }
            if (*v == '(' || (*v == '<' && v[1] != '<')) {
                if (!first) fputs(", ", stdout);
                first = 0;
                json_value(v);
                v = skip_value(v);
            } else break;
        }
        putchar(']');
    } else {
        fputs("null", stdout);   // indirect reference or unexpected type
    }
}

void extract_form_fields_json(FILE *f, const XRefTable *xref_table) {
    FieldMap map = {0};
    build_field_map(f, (XRefTable *)xref_table, 0, &map);

    printf("{\n  \"fields\": [");
    int emitted = 0;
    for (int i = 0; i < map.count; i++) {
        if (!map.items[i].terminal) continue;
        char *dict = get_object_raw(f, (XRefTable *)xref_table,
                                    map.items[i].obj_num, NULL);
        printf("%s\n    {\n      \"name\": \"", emitted++ ? "," : "");
        json_body((const unsigned char *)map.items[i].qname,
                  strlen(map.items[i].qname));
        const char *type = "unknown";
        switch (map.items[i].ftype) {
            case 'T': type = "text"; break;
            case 'B': type = "button"; break;
            case 'C': type = "choice"; break;
            case 'S': type = "signature"; break;
        }
        printf("\",\n      \"type\": \"%s\",\n      \"value\": ", type);
        const char *v = dict ? find_key(dict, "/V") : NULL;
        json_value(v && !isdigit((unsigned char)*v) ? v : NULL);

        if (dict && map.items[i].ftype == 'B') {
            char on[128];
            if (field_checkbox_on_state(f, (XRefTable *)xref_table, dict,
                                        on, sizeof(on))) {
                printf(",\n      \"on_state\": ");
                json_pdf_text((const unsigned char *)on, strlen(on));
            }
        }
        if (dict && map.items[i].ftype == 'C') {
            const char *ff = find_key(dict, "/Ff");
            int flags = ff ? atoi(ff) : 0;
            printf(",\n      \"combo\": %s,\n      \"multi_select\": %s",
                   (flags & 0x20000) ? "true" : "false",
                   (flags & 0x200000) ? "true" : "false");
            char disp[64][256];
            int n = field_choice_options(f, (XRefTable *)xref_table, dict,
                                         disp, 64);
            if (n > 0) {
                printf(",\n      \"options\": [");
                for (int j = 0; j < n; j++) {
                    if (j) fputs(", ", stdout);
                    json_pdf_text((const unsigned char *)disp[j],
                                  strlen(disp[j]));
                }
                putchar(']');
            }
        }
        printf("\n    }");
        free(dict);
    }
    printf("\n  ],\n  \"count\": %d\n}\n", emitted);

    field_map_free(&map);
    objstm_cache_reset();
}
