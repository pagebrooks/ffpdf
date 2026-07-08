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

#ifndef PDF_PARSER_H
#define PDF_PARSER_H

#include "utils.h"
#include "crypto.h"   // PdfCrypt

// PDF parsing functions
long find_startxref(FILE *f);
// Parse the object at `offset`. When `crypt` is non-NULL the object's stream is
// decrypted for it (cross-reference streams are never decrypted); pass NULL for
// unencrypted documents or when reading the xref itself.
PdfObject parse_obj_at_offset(FILE *f, long offset, const PdfCrypt *crypt);
int parse_xref_table(FILE *f, long xref_offset, XRefTable *xref_table);
void parse_xref_stream_data(const char *data, size_t data_len, const char *dict, XRefTable *xref_table);
void print_xref_table(const XRefTable *xref_table, FILE *f);

// Form field extraction helper functions
int extract_form_field_name(const char *data, size_t data_len, size_t start_pos, char *name_buffer, size_t buffer_size);
int extract_form_field_value(const char *data, size_t data_len, size_t search_start, char *value_buffer, size_t buffer_size);

#endif // PDF_PARSER_H
