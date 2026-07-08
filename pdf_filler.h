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

#ifndef PDF_FILLER_H
#define PDF_FILLER_H

#include <stdio.h>
#include "utils.h"
#include "pdf_parser.h"

// A single field name/value pair parsed from an FDF file.
typedef struct {
    char *field_name;   // fully-qualified partial name, e.g. "form1[0].f1_1[0]"
    char *field_value;  // string value (already unescaped); first element for arrays
    char **values;      // multi-select choice: all selected values (NULL if single)
    int nvalues;        // count in `values` (0 => single-valued, use field_value)
} FdfField;

// Parsed contents of an FDF file.
typedef struct {
    FdfField *fields;
    int field_count;
    int capacity;
} FdfData;

// Fill the AcroForm fields of `pdf_filename` using the values in `fdf_filename`,
// writing the resulting PDF to `out` (typically stdout). Returns 0 on success.
//
// The fill is emitted as a PDF incremental update: the original bytes are copied
// verbatim, then updated field objects, an updated AcroForm (with
// /NeedAppearances true) and a fresh cross-reference stream are appended. This is
// correct for modern PDFs that store fields inside compressed object streams.
int fill_pdf_with_fdf(const char *pdf_filename, const char *fdf_filename, FILE *out);

// As fill_pdf_with_fdf, but when `flatten` is non-zero the filled appearances
// are baked into the page content and the interactive form (widgets, AcroForm,
// XFA) is removed, producing a non-editable ("flattened") PDF.
int fill_pdf_with_fdf_ex(const char *pdf_filename, const char *fdf_filename, FILE *out, int flatten);

// FDF parsing (exposed for testing).
FdfData *parse_fdf_file(const char *fdf_filename);
void free_fdf_data(FdfData *fdf_data);

#endif // PDF_FILLER_H
