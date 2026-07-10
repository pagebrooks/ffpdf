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
