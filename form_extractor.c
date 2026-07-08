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
#include <stdio.h>

// Form-field extraction is a thin presentation layer over build_field_map (the
// same walk the fill command uses), so extraction and fill can never disagree
// about which fields exist. Only terminal (fillable) fields are emitted, with
// their fully-qualified names.

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
        printf("\">\n      <value></value>\n    </field>\n");
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
        printf(")\n/V ()\n>>\n");
    }
    printf("]\n>>\n>>\nendobj\ntrailer\n<<\n/Root 1 0 R\n>>\n%%%%EOF\n");

    field_map_free(&map);
    objstm_cache_reset();
}
